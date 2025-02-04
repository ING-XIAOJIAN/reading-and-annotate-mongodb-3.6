/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_impl.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/feature_compatibility_version_command_parser.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/introspect.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/system_index.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
	//InitializeDatabaseFactory完成DatabaseImpl，真正在Database::makeImpl中执行
MONGO_INITIALIZER(InitializeDatabaseFactory)(InitializerContext* const) {
    Database::registerFactory(
		[](Database* const this_,
                                 OperationContext* const opCtx,
                                 const StringData name,
                                 DatabaseCatalogEntry* const dbEntry) {
                                 //真正在Database::makeImpl中执行
        return stdx::make_unique<DatabaseImpl>(this_, opCtx, name, dbEntry);
    }
	);
    return Status::OK();
}
MONGO_FP_DECLARE(hangBeforeLoggingCreateCollection);
}  // namespace

using std::unique_ptr;
using std::endl;
using std::list;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

//表中带有$的，不能做caller操作
void uassertNamespaceNotIndex(StringData ns, StringData caller) {
    uassert(17320,
            str::stream() << "cannot do " << caller << " on namespace with a $ in it: " << ns,
            NamespaceString::normal(ns));
}

//DatabaseImpl::createCollection   KVDatabaseCatalogEntryBase::createCollection中new该类
//该类结构最终保存在WiredTigerRecoveryUnit._changes
class DatabaseImpl::AddCollectionChange : public RecoveryUnit::Change {
public:
    AddCollectionChange(OperationContext* opCtx, DatabaseImpl* db, StringData ns)
        : _opCtx(opCtx), _db(db), _ns(ns.toString()) {}

    virtual void commit() {
        CollectionMap::const_iterator it = _db->_collections.find(_ns);

        if (it == _db->_collections.end())
            return;

        // Ban reading from this collection on committed reads on snapshots before now.
        auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
        auto snapshotName = replCoord->reserveSnapshotName(_opCtx);
        it->second->setMinimumVisibleSnapshot(snapshotName);
    }

    virtual void rollback() {
        CollectionMap::const_iterator it = _db->_collections.find(_ns);

        if (it == _db->_collections.end())
            return;

        delete it->second;
        _db->_collections.erase(it);
    }

    OperationContext* const _opCtx;
    DatabaseImpl* const _db;
    const std::string _ns;
};

class DatabaseImpl::RemoveCollectionChange : public RecoveryUnit::Change {
public:
    // Takes ownership of coll (but not db).
    RemoveCollectionChange(DatabaseImpl* db, Collection* coll) : _db(db), _coll(coll) {}

    virtual void commit() {
        delete _coll;
    }

    virtual void rollback() {
        Collection*& inMap = _db->_collections[_coll->ns().ns()];
        invariant(!inMap);
        inMap = _coll;
    }

    DatabaseImpl* const _db;
    Collection* const _coll;
};

DatabaseImpl::~DatabaseImpl() {
    for (CollectionMap::const_iterator i = _collections.begin(); i != _collections.end(); ++i)
        delete i->second;
}

//DatabaseHolderImpl::close调用
void DatabaseImpl::close(OperationContext* opCtx, const std::string& reason) {
    // XXX? - Do we need to close database under global lock or just DB-lock is sufficient ?
    invariant(opCtx->lockState()->isW());

    // Clear cache of oplog Collection pointer.
    repl::oplogCheckCloseDatabase(opCtx, this->_this);

    if (BackgroundOperation::inProgForDb(_name)) {
        log() << "warning: bg op in prog during close db? " << _name;
    }

    for (auto&& pair : _collections) {
        auto* coll = pair.second;
        coll->getCursorManager()->invalidateAll(opCtx, true, reason);
    }
}

//库名有效性检查
Status DatabaseImpl::validateDBName(StringData dbname) {
    if (dbname.size() <= 0)
        return Status(ErrorCodes::BadValue, "db name is empty");

    if (dbname.size() >= 64)
        return Status(ErrorCodes::BadValue, "db name is too long");

    if (dbname.find('.') != string::npos)
        return Status(ErrorCodes::BadValue, "db name cannot contain a .");

    if (dbname.find(' ') != string::npos)
        return Status(ErrorCodes::BadValue, "db name cannot contain a space");

#ifdef _WIN32
    static const char* windowsReservedNames[] = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

    string lower(dbname.toString());
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (size_t i = 0; i < (sizeof(windowsReservedNames) / sizeof(char*)); ++i) {
        if (lower == windowsReservedNames[i]) {
            stringstream errorString;
            errorString << "db name \"" << dbname.toString() << "\" is a reserved name";
            return Status(ErrorCodes::BadValue, errorString.str());
        }
    }
#endif

    return Status::OK();
}

//通过AutoGetCollection相关接口获取对应Collection，可以参考insertBatchAndHandleErrors
//初始化构造赋值见AutoGetCollection::AutoGetCollection, 
//DatabaseImpl::_getOrCreateCollectionInstance  AutoGetCollection::AutoGetCollection中调用获取collection
//DatabaseImpl::createCollection或者DatabaseImpl::init中调用_getOrCreateCollectionInstance真正生成collection信息


//查找collection类，找到直接返回，没找到构造一个新的collection类  
//DatabaseImpl::createCollection DatabaseImpl::init 调用
Collection* DatabaseImpl::_getOrCreateCollectionInstance(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    Collection* collection = getCollection(opCtx, nss);

    if (collection) { //找到直接返回
        return collection;
    }

	//DatabaseImpl._collections列表没用该collection，则从元数据_mdb_catalog.wt获取表信息

	//没找到则构造一个新collection类  
	//KVDatabaseCatalogEntryBase::getCollectionCatalogEntry获取nss对应KVCollectionCatalogEntry
    unique_ptr<CollectionCatalogEntry> cce(_dbEntry->getCollectionCatalogEntry(nss.ns()));
	//获取uuid BSONCollectionCatalogEntry::getCollectionOptions
    auto uuid = cce->getCollectionOptions(opCtx).uuid;
	//KVDatabaseCatalogEntryBase::getRecordStore
	//默认返回为StandardWiredTigerRecordStore类型
    unique_ptr<RecordStore> rs(_dbEntry->getRecordStore(nss.ns()));
    invariant(rs.get());  // if cce exists, so should this

    // Not registering AddCollectionChange since this is for collections that already exist.
    Collection* coll = new Collection(opCtx, nss.ns(), uuid, cce.release(), rs.release(), _dbEntry);
    if (uuid) {
        // We are not in a WUOW only when we are called from Database::init(). There is no need
        // to rollback UUIDCatalog changes because we are initializing existing collections.
        auto&& uuidCatalog = UUIDCatalog::get(opCtx);
        if (!opCtx->lockState()->inAWriteUnitOfWork()) {
			//UUIDCatalog::registerUUIDCatalogEntry
            uuidCatalog.registerUUIDCatalogEntry(uuid.get(), coll);
        } else {
        	//UUIDCatalog::onCreateCollection
            uuidCatalog.onCreateCollection(opCtx, coll, uuid.get());
        }
    }

    return coll;
}

//DatabaseHolderImpl::openDb中调用
DatabaseImpl::DatabaseImpl(Database* const this_,
                           OperationContext* const opCtx,
                           const StringData name,
                           DatabaseCatalogEntry* const dbEntry)
    : _name(name.toString()),
      _dbEntry(dbEntry),
      _profileName(_name + ".system.profile"),
      _indexesName(_name + ".system.indexes"),
      _viewsName(_name + "." + DurableViewCatalog::viewsCollectionName().toString()),
      _durableViews(DurableViewCatalogImpl(this_)),
      _views(&_durableViews),
      _this(this_) {}

//初始化构造Database类的时候就会调用该init，参考database.h中的explicit inline Database
void DatabaseImpl::init(OperationContext* const opCtx) {
    Status status = validateDBName(_name);

    if (!status.isOK()) {
        warning() << "tried to open invalid db: " << _name;
        uasserted(10028, status.toString());
    }

    _profile = serverGlobalParams.defaultProfile;

    list<string> collections;
	//KVDatabaseCatalogEntryBase::getCollectionNamespaces
	//从元数据文件_mdb_catalog.wt中获取所有的表信息
    _dbEntry->getCollectionNamespaces(&collections);

	//把从原数据文件_mdb_catalog.wt获取到的表信息全部添加到_collections数组
    for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
        const string ns = *it;
        NamespaceString nss(ns);
		//collection初始化，生成对应CollectionImpl
        _collections[ns] = _getOrCreateCollectionInstance(opCtx, nss);
    }

    // At construction time of the viewCatalog, the _collections map wasn't initialized yet, so no
    // system.views collection would be found. Now we're sufficiently initialized, signal a version
    // change. Also force a reload, so if there are problems with the catalog contents as might be
    // caused by incorrect mongod versions or similar, they are found right away.
    _views.invalidate();
    Status reloadStatus = _views.reloadIfNeeded(opCtx);

    if (!reloadStatus.isOK()) {
        warning() << "Unable to parse views: " << redact(reloadStatus)
                  << "; remove any invalid views from the " << _viewsName
                  << " collection to restore server functionality." << startupWarningsLog;
    }
}

//清除该库下面所有的临时表
void DatabaseImpl::clearTmpCollections(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    list<string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    for (list<string>::iterator i = collections.begin(); i != collections.end(); ++i) {
        string ns = *i;
        invariant(NamespaceString::normal(ns));

        CollectionCatalogEntry* coll = _dbEntry->getCollectionCatalogEntry(ns);

        CollectionOptions options = coll->getCollectionOptions(opCtx);

		//不是临时表
        if (!options.temp)
            continue;
		//清除这个临时表
        try {
            WriteUnitOfWork wunit(opCtx);
            Status status = dropCollection(opCtx, ns, {});

            if (!status.isOK()) {
                warning() << "could not drop temp collection '" << ns << "': " << redact(status);
                continue;
            }

            wunit.commit();
        } catch (const WriteConflictException&) {
            warning() << "could not drop temp collection '" << ns << "' due to "
                                                                     "WriteConflictException";
            opCtx->recoveryUnit()->abandonSnapshot();
        }
    }
}

//db.setProfilingLevel(2)命令设置，参考https://docs.mongodb.com/v3.6/reference/method/db.setProfilingLevel/
//创建慢日志表system.profile并设置日志打印级别
Status DatabaseImpl::setProfilingLevel(OperationContext* opCtx, int newLevel) {
    if (_profile == newLevel) {
        return Status::OK();
    }

    if (newLevel == 0) {
        _profile = 0;
        return Status::OK();
    }

    if (newLevel < 0 || newLevel > 2) {
        return Status(ErrorCodes::BadValue, "profiling level has to be >=0 and <= 2");
    }

    Status status = createProfileCollection(opCtx, this->_this);

    if (!status.isOK()) {
        return status;
    }

    _profile = newLevel;

    return Status::OK();
}

//设置删库标记
void DatabaseImpl::setDropPending(OperationContext* opCtx, bool dropPending) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    if (dropPending) {
        uassert(ErrorCodes::DatabaseDropPending,
                str::stream() << "Unable to drop database " << name()
                              << " because it is already in the process of being dropped.",
                !_dropPending);
        _dropPending = true;
    } else {
        _dropPending = false;
    }
}

//该库是否正在被删除
bool DatabaseImpl::isDropPending(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    return _dropPending;
}

//db.stats统计
void DatabaseImpl::getStats(OperationContext* opCtx, BSONObjBuilder* output, double scale) {
    list<string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    long long nCollections = 0;
    long long nViews = 0;
    long long objects = 0;
    long long size = 0;
    long long storageSize = 0;
    long long numExtents = 0;
    long long indexes = 0;
    long long indexSize = 0;

    for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
        const string ns = *it;

        Collection* collection = getCollection(opCtx, ns);

        if (!collection)
            continue;

        nCollections += 1;
        objects += collection->numRecords(opCtx);
        size += collection->dataSize(opCtx);

        BSONObjBuilder temp;
        storageSize += collection->getRecordStore()->storageSize(opCtx, &temp);
        numExtents += temp.obj()["numExtents"].numberInt();  // XXX

        indexes += collection->getIndexCatalog()->numIndexesTotal(opCtx);
        indexSize += collection->getIndexSize(opCtx);
    }

    getViewCatalog()->iterate(opCtx, [&](const ViewDefinition& view) { nViews += 1; });

    output->appendNumber("collections", nCollections);
    output->appendNumber("views", nViews);
    output->appendNumber("objects", objects);
    output->append("avgObjSize", objects == 0 ? 0 : double(size) / double(objects));
    output->appendNumber("dataSize", size / scale);
    output->appendNumber("storageSize", storageSize / scale);
    output->appendNumber("numExtents", numExtents);
    output->appendNumber("indexes", indexes);
    output->appendNumber("indexSize", indexSize / scale);

    _dbEntry->appendExtraStats(opCtx, output, scale);

    if (!opCtx->getServiceContext()->getGlobalStorageEngine()->isEphemeral()) {
        boost::filesystem::path dbpath(storageGlobalParams.dbpath);
        if (storageGlobalParams.directoryperdb) {
            dbpath /= _name;
        }

        boost::system::error_code ec;
        boost::filesystem::space_info spaceInfo = boost::filesystem::space(dbpath, ec);
        if (!ec) {
            output->appendNumber("fsUsedSize", (spaceInfo.capacity - spaceInfo.available) / scale);
            output->appendNumber("fsTotalSize", spaceInfo.capacity / scale);
        } else {
            output->appendNumber("fsUsedSize", -1);
            output->appendNumber("fsTotalSize", -1);
            log() << "Failed to query filesystem disk stats (code: " << ec.value()
                  << "): " << ec.message();
        }
    }
}

//view相关，先跳过，以后有空再分析
Status DatabaseImpl::dropView(OperationContext* opCtx, StringData fullns) {
    Status status = _views.dropView(opCtx, NamespaceString(fullns));
    Top::get(opCtx->getServiceContext()).collectionDropped(fullns);
    return status;
}

//drop删表CmdDrop::errmsgRun->dropCollection会调用
//DatabaseImpl::clearTmpCollections调用
Status DatabaseImpl::dropCollection(OperationContext* opCtx,
                                    StringData fullns,
                                    repl::OpTime dropOpTime) {
    //删除一个不存在的表直接当成功处理
    if (!getCollection(opCtx, fullns)) {
        // Collection doesn't exist so don't bother validating if it can be dropped.
        return Status::OK();
    }

    NamespaceString nss(fullns);
    {
		//确定要删除的表是否属于该db库
        verify(nss.db() == _name);

        if (nss.isSystem()) {
			//system.profile日志表删除前必须关闭日志profile记录
            if (nss.isSystemDotProfile()) {
                if (_profile != 0)
                    return Status(ErrorCodes::IllegalOperation,
                                  "turn off profiling before dropping system.profile collection");
            } else if (!(nss.isSystemDotViews() || nss.isHealthlog() ||
                         nss == SessionsCollection::kSessionsNamespaceString ||
                         nss == NamespaceString::kSystemKeysCollectionName)) {
                //这几个system.xx以外的系统表不允许删除
                return Status(ErrorCodes::IllegalOperation,
                              str::stream() << "can't drop system collection " << fullns);
            }
        }
    }

    return dropCollectionEvenIfSystem(opCtx, nss, dropOpTime);
}

//drop删表CmdDrop::errmsgRun->dropCollection->DatabaseImpl::dropCollectionEvenIfSystem

////drop删表CmdDrop::errmsgRun->dropCollection会调用
Status DatabaseImpl::dropCollectionEvenIfSystem(OperationContext* opCtx,
                                                const NamespaceString& fullns,
                                                repl::OpTime dropOpTime) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    LOG(1) << "dropCollection: " << fullns;

    // A valid 'dropOpTime' is not allowed when writes are replicated.
    if (!dropOpTime.isNull() && opCtx->writesAreReplicated()) {
        return Status(
            ErrorCodes::BadValue,
            "dropCollection() cannot accept a valid drop optime when writes are replicated.");
    }

	//获取表信息
    Collection* collection = getCollection(opCtx, fullns);

	//表本来就不存在，直接返回OK
    if (!collection) {
        return Status::OK();  // Post condition already met.
    }

	//获取表对应uuid
    auto uuid = collection->uuid();
    auto uuidString = uuid ? uuid.get().toString() : "no UUID";

	//$命名的表不能做删表操作
    uassertNamespaceNotIndex(fullns.toString(), "dropCollection");
	//正在后台加索引的表不能删除
    BackgroundOperation::assertNoBgOpInProgForNs(fullns);

    // Make sure no indexes builds are in progress.
    // Use massert() to be consistent with IndexCatalog::dropAllIndexes().
    auto numIndexesInProgress = collection->getIndexCatalog()->numIndexesInProgress(opCtx);
    massert(40461,
            str::stream() << "cannot drop collection " << fullns.ns() << " (" << uuidString
                          << ") when "
                          << numIndexesInProgress
                          << " index builds in progress.",
            numIndexesInProgress == 0);

	//审计日志
    audit::logDropCollection(&cc(), fullns.toString());

	//删表后，需要清空该表的统计信息，从usage中移除
    Top::get(opCtx->getServiceContext()).collectionDropped(fullns.toString());

    // Drop unreplicated collections immediately.
    // If 'dropOpTime' is provided, we should proceed to rename the collection.
    // Under master/slave, collections are always dropped immediately. This is because drop-pending
    // collections support the rollback process which is not applicable to master/slave.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
	//一般表都是false
    auto isOplogDisabledForNamespace = replCoord->isOplogDisabledFor(opCtx, fullns);
	//不是复制集方式启动
    auto isMasterSlave =
        repl::ReplicationCoordinator::modeMasterSlave == replCoord->getReplicationMode();
	//master slave方式启动
    if ((dropOpTime.isNull() && isOplogDisabledForNamespace) || isMasterSlave) {
		//真正的底层WT存储引擎相关表及其索引删除在这里
        auto status = _finishDropCollection(opCtx, fullns, collection);
        if (!status.isOK()) {
            return status;
        }

		//OpObserverImpl::onDropCollection
        opObserver->onDropCollection(opCtx, fullns, uuid);
        return Status::OK();
    }

    // Replicated collections will be renamed with a special drop-pending namespace and dropped when
    // the replica set optime reaches the drop optime.
    if (dropOpTime.isNull()) {
        // MMAPv1 requires that index namespaces are subject to the same length constraints as
        // indexes in collections that are not in a drop-pending state. Therefore, we check if the
        // drop-pending namespace is too long for any index names in the collection.
        // These indexes are dropped regardless of the storage engine on the current node because we
        // may still have nodes running MMAPv1 in the replica set.

        // Compile a list of any indexes that would become too long following the drop-pending
        // rename. In the case that this collection drop gets rolled back, this will incur a
        // performance hit, since those indexes will have to be rebuilt from scratch, but data
        // integrity is maintained.
        //获取该集合对应的索引信息
        std::vector<IndexDescriptor*> indexesToDrop;
		//索引迭代器
        auto indexIter = collection->getIndexCatalog()->getIndexIterator(opCtx, true);

        // Determine which index names are too long. Since we don't have the collection drop optime
        // at this time, use the maximum optime to check the index names.
        auto longDpns = fullns.makeDropPendingNamespace(repl::OpTime::max());
		//获取该表下面的所有索引信息长度超标的索引，存入indexesToDrop数组容器
        while (indexIter.more()) {
            auto index = indexIter.next();
			//只记录长度超限的索引，这类索引需要提前删除
			//其他的没有超限的索引在_finishDropCollection中的IndexCatalogImpl::dropAllIndexes删除
            auto status = longDpns.checkLengthForRename(index->indexName().size());
            if (!status.isOK()) {
                indexesToDrop.push_back(index);
            }
        }

        // Drop the offending indexes.
        for (auto&& index : indexesToDrop) {
            log() << "dropCollection: " << fullns << " (" << uuidString << ") - index namespace '"
                  << index->indexNamespace()
                  << "' would be too long after drop-pending rename. Dropping index immediately.";
			//IndexCatalogImpl::dropIndex 删除索引
			fassertStatusOK(40463, collection->getIndexCatalog()->dropIndex(opCtx, index));
			//OpObserverImpl::onDropIndex 该索引删除对应的oplog记录，用于主从同步
			opObserver->onDropIndex(
                opCtx, fullns, collection->uuid(), index->indexName(), index->infoObj());
        }

        // Log oplog entry for collection drop and proceed to complete rest of two phase drop
        // process.
        //OpObserverImpl::onDropCollection 删表对应的oplog记录，用于主从同步
        dropOpTime = opObserver->onDropCollection(opCtx, fullns, uuid);

        // Drop collection immediately if OpObserver did not write entry to oplog.
        // After writing the oplog entry, all errors are fatal. See getNextOpTime() comments in
        // oplog.cpp.
        if (dropOpTime.isNull()) {
            log() << "dropCollection: " << fullns << " (" << uuidString
                  << ") - no drop optime available for pending-drop. "
                  << "Dropping collection immediately.";
			//这里进行底层
            fassertStatusOK(40462, _finishDropCollection(opCtx, fullns, collection));
            return Status::OK();
        }
    } else {
        // If we are provided with a valid 'dropOpTime', it means we are dropping this collection
        // in the context of applying an oplog entry on a secondary.
        // OpObserver::onDropCollection() should be returning a null OpTime because we should not be
        // writing to the oplog.
       	//OpObserver::onDropCollection
        auto opTime = opObserver->onDropCollection(opCtx, fullns, uuid);
        if (!opTime.isNull()) {
            severe() << "dropCollection: " << fullns << " (" << uuidString
                     << ") - unexpected oplog entry written to the oplog with optime " << opTime;
            fassertFailed(40468);
        }
    }

    auto dpns = fullns.makeDropPendingNamespace(dropOpTime);

    // Rename collection using drop-pending namespace generated from drop optime.
    const bool stayTemp = true;
    log() << "dropCollection: " << fullns << " (" << uuidString
          << ") - renaming to drop-pending collection: " << dpns << " with drop optime "
          << dropOpTime;
    fassertStatusOK(40464, renameCollection(opCtx, fullns.ns(), dpns.ns(), stayTemp));

    // Register this drop-pending namespace with DropPendingCollectionReaper to remove when the
    // committed optime reaches the drop optime.
    repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(dropOpTime, dpns);

    return Status::OK();
}

//drop删表CmdDrop::errmsgRun->dropCollection->DatabaseImpl::dropCollectionEvenIfSystem->DatabaseImpl::_finishDropCollection
//    ->DatabaseImpl::_finishDropCollection
//真正的底层WT存储引擎相关表及其索引删除在这里
Status DatabaseImpl::_finishDropCollection(OperationContext* opCtx,
                                           const NamespaceString& fullns,
                                           Collection* collection) {
    LOG(1) << "dropCollection: " << fullns << " - dropAllIndexes start";
	//先删除表下的所有索引
	//IndexCatalogImpl::dropAllIndexes
    collection->getIndexCatalog()->dropAllIndexes(opCtx, true);

    invariant(collection->getCatalogEntry()->getTotalIndexCount(opCtx) == 0);
    LOG(1) << "dropCollection: " << fullns << " - dropAllIndexes done";

    // We want to destroy the Collection object before telling the StorageEngine to destroy the
    // RecordStore.
    ////从DatabaseImpl._collections集合中把删除的集合去掉，以后就不能通过DatabaseImpl找到该集合了
    _clearCollectionCache(
        opCtx, fullns.toString(), "collection dropped", /*collectionGoingAway*/ true);

	//获取集合uuid
    auto uuid = collection->uuid();
    auto uuidString = uuid ? uuid.get().toString() : "no UUID";
    log() << "Finishing collection drop for " << fullns << " (" << uuidString << ").";

	//KVDatabaseCatalogEntry::dropCollection
	//真正的表文件相关底层删除在这里
    return _dbEntry->dropCollection(opCtx, fullns.toString());
}

//drop删表CmdDrop::errmsgRun->dropCollection->DatabaseImpl::dropCollectionEvenIfSystem->DatabaseImpl::_finishDropCollection
//DatabaseImpl::_finishDropCollection中调用

//从DatabaseImpl._collections集合中把删除的集合去掉，以后就不能通过DatabaseImpl找到该集合了
void DatabaseImpl::_clearCollectionCache(OperationContext* opCtx,
                                         StringData fullns,
                                         const std::string& reason,
                                         bool collectionGoingAway) {
    verify(_name == nsToDatabaseSubstring(fullns));
    CollectionMap::const_iterator it = _collections.find(fullns.toString());

    if (it == _collections.end())
        return;

    // Takes ownership of the collection
    opCtx->recoveryUnit()->registerChange(new RemoveCollectionChange(this, it->second));

    it->second->getCursorManager()->invalidateAll(opCtx, collectionGoingAway, reason);
    _collections.erase(it);
}

//DatabaseImpl::_getOrCreateCollectionInstance  AutoGetCollection::AutoGetCollection中调用
Collection* DatabaseImpl::getCollection(OperationContext* opCtx, StringData ns) const {
    NamespaceString nss(ns);
    invariant(_name == nss.db());
    return getCollection(opCtx, nss);
}

//DatabaseImpl::_getOrCreateCollectionInstance  AutoGetCollection::AutoGetCollection中调用

//从_collections缓存中找出nss对应的表，DatabaseImpl::createCollection创建collection的时候添加到_collections数组
Collection* DatabaseImpl::getCollection(OperationContext* opCtx, const NamespaceString& nss) const {
    dassert(!cc().getOperationContext() || opCtx == cc().getOperationContext());
    CollectionMap::const_iterator it = _collections.find(nss.ns());
	
    if (it != _collections.end() && it->second) {
        Collection* found = it->second;
        if (enableCollectionUUIDs) {
			//更新NamespaceUUIDCache
            NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
            if (auto uuid = found->uuid())
                cache.ensureNamespaceInCache(nss, uuid.get());
        }
        return found;
    }

    return NULL;
}

//集合重命名
Status DatabaseImpl::renameCollection(OperationContext* opCtx,
                                      StringData fromNS,
                                      StringData toNS,
                                      bool stayTemp) {
    //审计相关
    audit::logRenameCollection(&cc(), fromNS, toNS);
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    BackgroundOperation::assertNoBgOpInProgForNs(fromNS);
    BackgroundOperation::assertNoBgOpInProgForNs(toNS);

    NamespaceString fromNSS(fromNS);
    NamespaceString toNSS(toNS);
    {  // remove anything cached
    	//查找该db下面是否有该collection
        Collection* coll = getCollection(opCtx, fromNS);

        if (!coll)
            return Status(ErrorCodes::NamespaceNotFound, "collection not found to rename");

        string clearCacheReason = str::stream() << "renamed collection '" << fromNS << "' to '"
                                                << toNS << "'";
        IndexCatalog::IndexIterator ii = coll->getIndexCatalog()->getIndexIterator(opCtx, true);

        while (ii.more()) {
            IndexDescriptor* desc = ii.next();
            _clearCollectionCache(
                opCtx, desc->indexNamespace(), clearCacheReason, /*collectionGoingAway*/ true);
        }

		//从db中清除from和to的collection信息
        _clearCollectionCache(opCtx, fromNS, clearCacheReason, /*collectionGoingAway*/ true);
        _clearCollectionCache(opCtx, toNS, clearCacheReason, /*collectionGoingAway*/ false);

		//统计中清除from集合
        Top::get(opCtx->getServiceContext()).collectionDropped(fromNS.toString());
    }

	//KVDatabaseCatalogEntry::renameCollection 存储引擎层从命名集合
    Status s = _dbEntry->renameCollection(opCtx, fromNS, toNS, stayTemp);
    opCtx->recoveryUnit()->registerChange(new AddCollectionChange(opCtx, this, toNS));
	//新的集合名添加到_collections[]数组
	_collections[toNS] = _getOrCreateCollectionInstance(opCtx, toNSS);

    return s;
}

//获取collection信息，没用则创建
Collection* DatabaseImpl::getOrCreateCollection(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    Collection* c = getCollection(opCtx, nss);

    if (!c) {
        c = createCollection(opCtx, nss.ns());
    }
    return c;
}

//检测是否可以创建集合
void DatabaseImpl::_checkCanCreateCollection(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& options) {
    massert(17399,
            str::stream() << "Cannot create collection " << nss.ns()
                          << " - collection already exists.",
            getCollection(opCtx, nss) == nullptr);
    uassertNamespaceNotIndex(nss.ns(), "createCollection");

    uassert(14037,
            "can't create user databases on a --configsvr instance",
            serverGlobalParams.clusterRole != ClusterRole::ConfigServer || nss.isOnInternalDb());

    // This check only applies for actual collections, not indexes or other types of ns.
    uassert(17381,
            str::stream() << "fully qualified namespace " << nss.ns() << " is too long "
                          << "(max is "
                          << NamespaceString::MaxNsCollectionLen
                          << " bytes)",
            !nss.isNormal() || nss.size() <= NamespaceString::MaxNsCollectionLen);

    uassert(17316, "cannot create a blank collection", nss.coll() > 0);
    uassert(28838, "cannot create a non-capped oplog collection", options.capped || !nss.isOplog());
    uassert(ErrorCodes::DatabaseDropPending,
            str::stream() << "Cannot create collection " << nss.ns()
                          << " - database is in the process of being dropped.",
            !_dropPending);
}

Status DatabaseImpl::createView(OperationContext* opCtx,
                                StringData ns,
                                const CollectionOptions& options) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    invariant(options.isView());

    NamespaceString nss(ns);
    NamespaceString viewOnNss(nss.db(), options.viewOn);
    _checkCanCreateCollection(opCtx, nss, options);
    audit::logCreateCollection(&cc(), ns);

    if (nss.isOplog())
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid namespace name for a view: " + nss.toString());

    return _views.createView(opCtx, nss, viewOnNss, BSONArray(options.pipeline), options.collation);
}

//AutoGetDb::AutoGetDb或者AutoGetOrCreateDb::AutoGetOrCreateDb->DatabaseHolderImpl::get从DatabaseHolderImpl._dbs数组查找获取Database
//DatabaseImpl::createCollection创建collection的表全部添加到_collections数组中
//AutoGetCollection::AutoGetCollection从UUIDCatalog._catalog数组通过查找uuid可以获取collection表信息

//手动建表流程：CmdCreate::run->createCollection->mongo::userCreateNSImpl->
//直接写入数据的时候会建表流程：insertBatchAndHandleErrors->makeCollection->mongo::userCreateNS
//    ->mongo::userCreateNSImpl->DatabaseImpl::createCollection

//insertBatchAndHandleErrors->makeCollection->mongo::userCreateNS->mongo::userCreateNSImpl->DatabaseImpl::createCollection
//在wiredtiger创建集合和索引
Collection* DatabaseImpl::createCollection(OperationContext* opCtx,
                                           StringData ns,
                                           const CollectionOptions& options,
                                           bool createIdIndex,
                                           const BSONObj& idIndex) {
    //注意这里检查锁，建集合必须是MODE_X锁
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    invariant(!options.isView());
    NamespaceString nss(ns);

	//是否允许隐式创建集合
    uassert(ErrorCodes::CannotImplicitlyCreateCollection,
            "request doesn't allow collection to be created implicitly",
            OperationShardingState::get(opCtx).allowImplicitCollectionCreation());

    CollectionOptions optionsWithUUID = options;
    bool generatedUUID = false;
	//默认开启 
	//每个collection会对应一个uuid，见DatabaseImpl::createCollection，只有3.6以上版本有该uuid
    if (enableCollectionUUIDs && !optionsWithUUID.uuid &&
        serverGlobalParams.featureCompatibility.isSchemaVersion36()) {
        auto coordinator = repl::ReplicationCoordinator::get(opCtx);
        bool fullyUpgraded = serverGlobalParams.featureCompatibility.getVersion() ==
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36;
        bool canGenerateUUID =
			//单机方式启动
            (coordinator->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) ||
			//复制集方式，是否可以写，如果主在就可以  或者 慢日志表
			coordinator->canAcceptWritesForDatabase(opCtx, nss.db()) || nss.isSystemDotProfile();

        if (fullyUpgraded && !canGenerateUUID) {
            std::string msg = str::stream() << "Attempted to create a new collection " << nss.ns()
                                            << " without a UUID";
            severe() << msg;
            uasserted(ErrorCodes::InvalidOptions, msg);
        }
		//每个表对应一个uuid
        if (canGenerateUUID) {
			//也就是UUID::gen()，生成唯一UUID
            optionsWithUUID.uuid.emplace(CollectionUUID::gen());
            generatedUUID = true;
        }
    }

	//检查相关参数是否有效，如集合名最大长度等
    _checkCanCreateCollection(opCtx, nss, optionsWithUUID);
    audit::logCreateCollection(&cc(), ns);

	//createCollection: test.test with generated UUID: 43e959f4-7bb4-4cd1-ba10-5075df67fa44
    if (optionsWithUUID.uuid) {
        log() << "createCollection: " << ns << " with "
              << (generatedUUID ? "generated" : "provided")
              << " UUID: " << optionsWithUUID.uuid.get(); //OptionalCollectionUUID
    } else {
        log() << "createCollection: " << ns << " with no UUID.";
    }

	//KVDatabaseCatalogEntryBase::createCollection 
	//通知wiredtiger创建collection对应的目录文件 
    massertStatusOK(
    //对应KVDatabaseCatalogEntryBase::createCollection
    //调用KV层的元数据管理模块
        _dbEntry->createCollection(opCtx, ns, optionsWithUUID, true /*allocateDefaultSpace*/));

	//wiredtiger对应WiredTigerRecoveryUnit::registerChange
    opCtx->recoveryUnit()->registerChange(new AddCollectionChange(opCtx, this, ns));
	//查找collection类，找到直接返回，没找到构造一个新的collection类
    Collection* collection = _getOrCreateCollectionInstance(opCtx, nss);
    invariant(collection);
	//保存到_collections数组
    _collections[ns] = collection; //ns集合对应collection

    BSONObj fullIdIndexSpec;

	//id索引创建
    if (createIdIndex) {
        if (collection->requiresIdIndex()) {
            if (optionsWithUUID.autoIndexId == CollectionOptions::YES ||
                optionsWithUUID.autoIndexId == CollectionOptions::DEFAULT) {
                const auto featureCompatibilityVersion =
                    serverGlobalParams.featureCompatibility.getVersion();
                IndexCatalog* ic = collection->getIndexCatalog();
				//IndexCatalogImpl::createIndexOnEmptyCollection
				//建id索引
                fullIdIndexSpec = uassertStatusOK(ic->createIndexOnEmptyCollection(
                    opCtx,
                    !idIndex.isEmpty() ? idIndex
                                       : ic->getDefaultIdIndexSpec(featureCompatibilityVersion)));
            }
        }

		//如锅集合是"system.X"相关的索引
        if (nss.isSystem()) { //wiredtiger创建普通索引文件
            createSystemIndexes(opCtx, collection);
        }
    }

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLoggingCreateCollection);

    opCtx->getServiceContext()->getOpObserver()->onCreateCollection(
        opCtx, collection, nss, optionsWithUUID, fullIdIndexSpec);

    return collection;
}

const DatabaseCatalogEntry* DatabaseImpl::getDatabaseCatalogEntry() const {
    return _dbEntry;
}

//drop_database.cpp中的dropDatabase和DatabaseImpl::dropDatabase  dropDatabaseImpl什么区别？需要进一步分析
//区别如下：drop_database.cpp中的dropDatabase会通过_finishDropDatabase调用DatabaseImpl::dropDatabase
//drop_database.cpp中的dropDatabase删库及其库下面的表，DatabaseImpl::dropDatabase只删库

//删库dropDatabase->_finishDropDatabase调用
void DatabaseImpl::dropDatabase(OperationContext* opCtx, Database* db) {
    invariant(db);

    // Store the name so we have if for after the db object is deleted
    const string name = db->name();
    LOG(1) << "dropDatabase " << name;

    invariant(opCtx->lockState()->isDbLockedForMode(name, MODE_X));

	//检查是否有在创建后台索引等
    BackgroundOperation::assertNoBgOpInProgForDb(name);

    audit::logDropDatabase(opCtx->getClient(), name);

    auto const serviceContext = opCtx->getServiceContext();

	//从统计信息中清除该表，表示不用对该表做计算操作了，因为表已经清除
    for (auto&& coll : *db) {
        Top::get(serviceContext).collectionDropped(coll->ns().ns(), true);
    }

	//database_holder_impl::close
	//从全局DatabaseHolder中清除该db
    dbHolder().close(opCtx, name, "database dropped");

    auto const storageEngine = serviceContext->getGlobalStorageEngine();
    writeConflictRetry(opCtx, "dropDatabase", name, [&] {
		//KVStorageEngine::dropDatabase
		//这里面先删除该db下面的索引表，然后再从全局engine中清除该db
        storageEngine->dropDatabase(opCtx, name).transitional_ignore();
    });
}

StatusWith<NamespaceString> DatabaseImpl::makeUniqueCollectionNamespace(
    OperationContext* opCtx, StringData collectionNameModel) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    // There must be at least one percent sign within the first MaxNsCollectionLen characters of the
    // generated namespace after accounting for the database name prefix and dot separator:
    //     <db>.<truncated collection model name>
    auto maxModelLength = NamespaceString::MaxNsCollectionLen - (_name.length() + 1);
    auto model = collectionNameModel.substr(0, maxModelLength);
    auto numPercentSign = std::count(model.begin(), model.end(), '%');
    if (numPercentSign == 0) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Cannot generate collection name for temporary collection: "
                                       "model for collection name "
                                    << collectionNameModel
                                    << " must contain at least one percent sign within first "
                                    << maxModelLength
                                    << " characters.");
    }

    if (!_uniqueCollectionNamespacePseudoRandom) {
        Timestamp ts;
        _uniqueCollectionNamespacePseudoRandom =
            stdx::make_unique<PseudoRandom>(Date_t::now().asInt64());
    }

    const auto charsToChooseFrom =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"_sd;
    invariant((10U + 26U * 2) == charsToChooseFrom.size());

    auto replacePercentSign = [&, this](const auto& c) {
        if (c != '%') {
            return c;
        }
        auto i = _uniqueCollectionNamespacePseudoRandom->nextInt32(charsToChooseFrom.size());
        return charsToChooseFrom[i];
    };

    auto numGenerationAttempts = numPercentSign * charsToChooseFrom.size() * 100U;
    for (decltype(numGenerationAttempts) i = 0; i < numGenerationAttempts; ++i) {
        auto collectionName = model.toString();
        std::transform(collectionName.begin(),
                       collectionName.end(),
                       collectionName.begin(),
                       replacePercentSign);

        NamespaceString nss(_name, collectionName);
        if (!getCollection(opCtx, nss)) {
            return nss;
        }
    }

    return Status(
        ErrorCodes::NamespaceExists,
        str::stream() << "Cannot generate collection name for temporary collection with model "
                      << collectionNameModel
                      << " after "
                      << numGenerationAttempts
                      << " attempts due to namespace conflicts with existing collections.");
}

namespace {
MONGO_INITIALIZER(InitializeDropDatabaseImpl)(InitializerContext* const) {
    Database::registerDropDatabaseImpl(DatabaseImpl::dropDatabase);
    return Status::OK();
}
MONGO_INITIALIZER(InitializeUserCreateNSImpl)(InitializerContext* const) {
    registerUserCreateNSImpl(userCreateNSImpl);
    return Status::OK();
}

MONGO_INITIALIZER(InitializeDropAllDatabasesExceptLocalImpl)(InitializerContext* const) {
    registerDropAllDatabasesExceptLocalImpl(dropAllDatabasesExceptLocalImpl);
    return Status::OK();
}
}  // namespace
}  // namespace mongo

//mongo::dropAllDatabasesExceptLocal调用
void mongo::dropAllDatabasesExceptLocalImpl(OperationContext* opCtx) {
    Lock::GlobalWrite lk(opCtx);

    vector<string> n;
    StorageEngine* storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
    storageEngine->listDatabases(&n);

    if (n.size() == 0)
        return;
    log() << "dropAllDatabasesExceptLocal " << n.size();

    repl::ReplicationCoordinator::get(opCtx)->dropAllSnapshots();

    for (const auto& dbName : n) {
        if (dbName != "local") {
            writeConflictRetry(opCtx, "dropAllDatabasesExceptLocal", dbName, [&opCtx, &dbName] {
                Database* db = dbHolder().get(opCtx, dbName);

                // This is needed since dropDatabase can't be rolled back.
                // This is safe be replaced by "invariant(db);dropDatabase(opCtx, db);" once fixed
                if (db == nullptr) {
                    log() << "database disappeared after listDatabases but before drop: " << dbName;
                } else {
                    DatabaseImpl::dropDatabase(opCtx, db);
                }
            });
        }
    }
}

//手动建表流程：CmdCreate::run->createCollection->mongo::userCreateNSImpl->DatabaseImpl::createCollection
//直接写入数据的时候会建表流程：insertBatchAndHandleErrors->makeCollection->mongo::userCreateNS
//    ->mongo::userCreateNSImpl->DatabaseImpl::createCollection


//insertBatchAndHandleErrors->makeCollection->mongo::userCreateNS->mongo::userCreateNSImpl
//mongo::userCreateNS中执行，创建集合，这里主要是一些建集合相关的参数有效性检查，真正创建集合
//通过调用DatabaseImpl::createCollection实现
auto mongo::userCreateNSImpl(OperationContext* opCtx,
                             Database* db,
                             StringData ns,
                             BSONObj options,
                             CollectionOptions::ParseKind parseKind,
                             bool createDefaultIndexes,
                             const BSONObj& idIndex) -> Status {
    invariant(db);

    LOG(1) << "create collection " << ns << ' ' << options;

    if (!NamespaceString::validCollectionComponent(ns))
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "invalid ns: " << ns);
	
	//获取对应CollectionImpl  DatabaseImpl::getCollection
    Collection* collection = db->getCollection(opCtx, ns); //DatabaseImpl::getCollection
	//表已经存在
    if (collection)
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a collection '" << ns.toString() << "' already exists");

	//ViewCatalog先跳过，以后分析
    if (db->getViewCatalog()->lookup(opCtx, ns))
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a view '" << ns.toString() << "' already exists");

    CollectionOptions collectionOptions;
	//创建集合相关参数检查判断
    Status status = collectionOptions.parse(options, parseKind);

    if (!status.isOK())
        return status;

    // Validate the collation, if there is one.
    //collator先跳过，以后分析
    std::unique_ptr<CollatorInterface> collator;
    if (!collectionOptions.collation.isEmpty()) {
        auto collatorWithStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(collectionOptions.collation);

        if (!collatorWithStatus.isOK()) {
            return collatorWithStatus.getStatus();
        }

        collator = std::move(collatorWithStatus.getValue());

        // If the collator factory returned a non-null collator, set the collation option to the
        // result of serializing the collator's spec back into BSON. We do this in order to fill in
        // all options that the user omitted.
        //
        // If the collator factory returned a null collator (representing the "simple" collation),
        // we simply unset the "collation" from the collection options. This ensures that
        // collections created on versions which do not support the collation feature have the same
        // format for representing the simple collation as collections created on this version.
        collectionOptions.collation = collator ? collator->getSpec().toBSON() : BSONObj();
    }

    if (!collectionOptions.validator.isEmpty()) {
        // Pre-parse the validator document to make sure there are no extensions that are not
        // permitted in collection validators.
        MatchExpressionParser::AllowedFeatureSet allowedFeatures =
            MatchExpressionParser::kBanAllSpecialFeatures;
        if (!serverGlobalParams.validateFeaturesAsMaster.load() ||
            (serverGlobalParams.featureCompatibility.getVersion() ==
             ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36)) {
            // Note that we don't enforce this feature compatibility check when we are on
            // the secondary or on a backup instance, as indicated by !validateFeaturesAsMaster.
            allowedFeatures |= MatchExpressionParser::kJSONSchema;
            allowedFeatures |= MatchExpressionParser::kExpr;
        }
        boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(opCtx, collator.get()));
        auto statusWithMatcher = MatchExpressionParser::parse(collectionOptions.validator,
                                                              std::move(expCtx),
                                                              ExtensionsCallbackNoop(),
                                                              allowedFeatures);

        // We check the status of the parse to see if there are any banned features, but we don't
        // actually need the result for now.
        if (!statusWithMatcher.isOK()) {
            if (statusWithMatcher.getStatus().code() == ErrorCodes::QueryFeatureNotAllowed) {
                // The default error message for disallowed $jsonSchema and $expr is not descriptive
                // enough, so we rewrite it here.
                return {ErrorCodes::QueryFeatureNotAllowed,
                        str::stream() << "The featureCompatibilityVersion must be 3.6 to create a "
                                         "collection validator using 3.6 query features. See "
                                      << feature_compatibility_version::kDochubLink
                                      << "."};
            } else {
                return statusWithMatcher.getStatus();
            }
        }
    }

    status =
        validateStorageOptions(collectionOptions.storageEngine,
                               stdx::bind(&StorageEngine::Factory::validateCollectionStorageOptions,
                                          stdx::placeholders::_1,
                                          stdx::placeholders::_2));

	//存储引擎相关参数不对，直接报错，建表的时候可以指定存储引擎参数信息
    if (!status.isOK())
        return status;

    if (auto indexOptions = collectionOptions.indexOptionDefaults["storageEngine"]) {
        status =
			//存储引擎索引参数检测
            validateStorageOptions(indexOptions.Obj(),
                                   stdx::bind(&StorageEngine::Factory::validateIndexStorageOptions,
                                              stdx::placeholders::_1,
                                              stdx::placeholders::_2));

        if (!status.isOK()) {
            return status;
        }
    }

    if (collectionOptions.isView()) {
        invariant(parseKind == CollectionOptions::parseForCommand);
        uassertStatusOK(db->createView(opCtx, ns, collectionOptions));
    } else { //DatabaseImpl::createCollection 创建集合
        invariant(
            db->createCollection(opCtx, ns, collectionOptions, createDefaultIndexes, idIndex));
    }

    return Status::OK();
}

