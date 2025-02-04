/**
 *    Copyright (C) 2017 10gen Inc.
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

#pragma once

#include "mongo/db/catalog/index_create.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_impl.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/record_id.h"

namespace mongo {

class BackgroundOperation;
class BSONObj;
class Collection;
class OperationContext;

/**
 * Builds one or more indexes.
 *
 * If any method other than insert() returns a not-ok Status, this MultiIndexBlock should be
 * considered failed and must be destroyed.
 *
 * If a MultiIndexBlock is destroyed before commit() or if commit() is rolled back, it will
 * clean up all traces of the indexes being constructed. MultiIndexBlocks should not be
 * destructed from inside of a WriteUnitOfWork as any cleanup needed should never be rolled back
 * (as it is itself essentially a form of rollback, you don't want to "rollback the rollback").
 */
/*
db.runCommand(
  {
    createIndexes: <collection>,
    indexes: [
        {
            key: {
                <key-value_pair>,
                <key-value_pair>,
                ...
            },
            name: <index_name>,
            <option1>,
            <option2>,
            ...
        },
        { ... },
        { ... }
    ],
    writeConcern: { <write concern> },
    commitQuorum: <int|string>,
    comment: <any>
  }
) 
一个命令可以同时创建多个索引
*/

class MultiIndexBlockImpl : public MultiIndexBlock::Impl {
    MONGO_DISALLOW_COPYING(MultiIndexBlockImpl);

public:
    /**
     * Neither pointer is owned.
     */
    MultiIndexBlockImpl(OperationContext* opCtx, Collection* collection);
    ~MultiIndexBlockImpl() override;

    /**
     * By default we ignore the 'background' flag in specs when building an index. If this is
     * called before init(), we will build the indexes in the background as long as *all* specs
     * call for background indexing. If any spec calls for foreground indexing all indexes will
     * be built in the foreground, as there is no concurrency benefit to building a subset of
     * indexes in the background, but there is a performance benefit to building all in the
     * foreground.
     */
    void allowBackgroundBuilding() override {
        _buildInBackground = true;
    }

    /**
     * Call this before init() to allow the index build to be interrupted.
     * This only affects builds using the insertAllDocumentsInCollection helper.
     */
    //CmdCreateIndex::errmsgRun中调用
    void allowInterruption() override {
        _allowInterruption = true;
    }

    /**
     * By default we enforce the 'unique' flag in specs when building an index by failing.
     * If this is called before init(), we will ignore unique violations. This has no effect if
     * no specs are unique.
     *
     * If this is called, any dupsOut sets passed in will never be filled.
     */
    void ignoreUniqueConstraint() override {
        _ignoreUnique = true;
    }

    /**
     * Removes pre-existing indexes from 'specs'. If this isn't done, init() may fail with
     * IndexAlreadyExists.
     */
    void removeExistingIndexes(std::vector<BSONObj>* specs) const override;

    /**
     * Prepares the index(es) for building and returns the canonicalized form of the requested index
     * specifications.
     *
     * Does not need to be called inside of a WriteUnitOfWork (but can be due to nesting).
     *
     * Requires holding an exclusive database lock.
     */
    StatusWith<std::vector<BSONObj>> init(const std::vector<BSONObj>& specs) override;
    StatusWith<std::vector<BSONObj>> init(const BSONObj& spec) override;

    /**
     * Inserts all documents in the Collection into the indexes and logs with timing info.
     *
     * This is a simplified replacement for insert and doneInserting. Do not call this if you
     * are calling either of them.
     *
     * If dupsOut is passed as non-NULL, violators of uniqueness constraints will be added to
     * the set rather than failing the build. Documents added to this set are not indexed, so
     * callers MUST either fail this index build or delete the documents from the collection.
     *
     * Can throw an exception if interrupted.
     *
     * Should not be called inside of a WriteUnitOfWork.
     */
    Status insertAllDocumentsInCollection(std::set<RecordId>* dupsOut = nullptr) override;

    /**
     * Call this after init() for each document in the collection.
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * Should be called inside of a WriteUnitOfWork.
     */
    Status insert(const BSONObj& wholeDocument, const RecordId& loc) override;

    /**
     * Call this after the last insert(). This gives the index builder a chance to do any
     * long-running operations in separate units of work from commit().
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * If dupsOut is passed as non-NULL, violators of uniqueness constraints will be added to
     * the set. Documents added to this set are not indexed, so callers MUST either fail this
     * index build or delete the documents from the collection.
     *
     * Should not be called inside of a WriteUnitOfWork.
     */
    Status doneInserting(std::set<RecordId>* dupsOut = nullptr) override;

    /**
     * Marks the index ready for use. Should only be called as the last method after
     * doneInserting() or insertAllDocumentsInCollection() return success.
     *
     * Should be called inside of a WriteUnitOfWork. If the index building is to be logOp'd,
     * logOp() should be called from the same unit of work as commit().
     *
     * Requires holding an exclusive database lock.
     */
    void commit() override;

    /**
     * May be called at any time after construction but before a successful commit(). Suppresses
     * the default behavior on destruction of removing all traces of uncommitted index builds.
     *
     * The most common use of this is if the indexes were already dropped via some other
     * mechanism such as the whole collection being dropped. In that case, it would be invalid
     * to try to remove the indexes again. Also, replication uses this to ensure that indexes
     * that are being built on shutdown are resumed on startup.
     *
     * Do not use this unless you are really sure you need to.
     *
     * Does not matter whether it is called inside of a WriteUnitOfWork. Will not be rolled
     * back.
     */
    void abortWithoutCleanup() override;

    bool getBuildInBackground() const override {
        return _buildInBackground;
    }

private:
    class SetNeedToCleanupOnRollback;
    class CleanupIndexesVectorOnRollback;

    //MultiIndexBlockImpl._indexes为该类型，下面的_indexes
    //MultiIndexBlockImpl::init初始化
    struct IndexToBuild {
        std::unique_ptr<IndexCatalogImpl::IndexBuildBlock> block;

        //索引方法  btree  text 2d等，btree对应Btree_access_method
        //MultiIndexBlockImpl::insert中真正使用，索引数据对应KV写入存储引擎
        //非阻塞加索引用这个   
        //MultiIndexBlockImpl::init中赋值,MultiIndexBlockImpl::insert中使用该real
        IndexAccessMethod* real = NULL;           // owned elsewhere
        //createindex中的partialFilterExpression参数信息
        const MatchExpression* filterExpression;  // might be NULL, owned elsewhere
        //MultiIndexBlockImpl::insert中真正使用，建索引不带backgroud阻塞加索引会使用这个
        //对应BulkBuilder,
        std::unique_ptr<IndexAccessMethod::BulkBuilder> bulk;

        InsertDeleteOptions options;
    };

    //一个索引对应一个IndexToBuild，一个命令可以创建多个索引，所以这里是一个数组
    std::vector<IndexToBuild> _indexes;
    

    std::unique_ptr<BackgroundOperation> _backgroundOperation;

    // Pointers not owned here and must outlive 'this'
    //通过这里和CollectionImpl联系起来，从而通过CollectionImpl._indexCatalog和IndexCatalogImpl关联起来
    Collection* _collection;
    OperationContext* _opCtx;

    //上面的allowBackgroundBuilding()赋值为true
    //MultiIndexBlockImpl::init中根据建索引是否指定了backgroud参数进行赋值
    bool _buildInBackground; //后台建索引
    //默认false,allowInterruption()赋值为true 
    //代表加索引过程释放可以被killop干掉
    bool _allowInterruption;
    bool _ignoreUnique;

    bool _needToCleanup;
};

}  // namespace mongo
