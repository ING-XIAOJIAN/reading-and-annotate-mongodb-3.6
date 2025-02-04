/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batch_write_op.h"

#include <numeric>

#include "mongo/base/error_codes.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {

using std::unique_ptr;
using std::make_pair;
using std::set;
using std::stringstream;
using std::vector;

namespace {

// Conservative overhead per element contained in the write batch. This value was calculated as 1
// byte (element type) + 5 bytes (max string encoding of the array index encoded as string and the
// maximum key is 99999) + 1 byte (zero terminator) = 7 bytes
const int kBSONArrayPerElementOverheadBytes = 7;

//BatchWriteOp::targetBatch中使用
struct BatchSize {
    int numOps{0};
    int sizeBytes{0};
};

//BatchWriteOp::targetBatch
typedef std::map<const ShardEndpoint*, BatchSize, EndpointComp> TargetedBatchSizeMap;

struct WriteErrorDetailComp {
    bool operator()(const WriteErrorDetail* errorA, const WriteErrorDetail* errorB) const {
        return errorA->getIndex() < errorB->getIndex();
    }
};

// MAGIC NUMBERS
//
// Before serializing updates/deletes, we don't know how big their fields would be, but we break
// batches before serializing.
//
// TODO: Revisit when we revisit command limits in general
const int kEstUpdateOverheadBytes = (BSONObjMaxInternalSize - BSONObjMaxUserSize) / 100;
const int kEstDeleteOverheadBytes = (BSONObjMaxInternalSize - BSONObjMaxUserSize) / 100;

/**
 * Returns a new write concern that has the copy of every field from the original
 * document but with a w set to 1. This is intended for upgrading { w: 0 } write
 * concern to { w: 1 }.
 */
BSONObj upgradeWriteConcern(const BSONObj& origWriteConcern) {
    BSONObjIterator iter(origWriteConcern);
    BSONObjBuilder newWriteConcern;

    while (iter.more()) {
        BSONElement elem(iter.next());

        if (strncmp(elem.fieldName(), "w", 2) == 0) {
            newWriteConcern.append("w", 1);
        } else {
            newWriteConcern.append(elem);
        }
    }

    return newWriteConcern.obj();
}

void buildTargetError(const Status& errStatus, WriteErrorDetail* details) {
    details->setErrCode(errStatus.code());
    details->setErrMessage(errStatus.reason());
}

/**
 * Helper to determine whether a number of targeted writes require a new targeted batch.
 */
bool isNewBatchRequired(const std::vector<TargetedWrite*>& writes,
                        const TargetedBatchMap& batchMap) {
    for (vector<TargetedWrite*>::const_iterator it = writes.begin(); it != writes.end(); ++it) {
        TargetedWrite* write = *it;
        if (batchMap.find(&write->endpoint) == batchMap.end()) {
            return true;
        }
    }

    return false;
}

/**
 * Helper to determine whether a number of targeted writes require a new targeted batch.
 */ //文档长度检查wouldMakeBatchesTooBig
bool wouldMakeBatchesTooBig(const std::vector<TargetedWrite*>& writes,
                            int writeSizeBytes,
                            const TargetedBatchSizeMap& batchSizes) {
    for (vector<TargetedWrite*>::const_iterator it = writes.begin(); it != writes.end(); ++it) {
        const TargetedWrite* write = *it;
        TargetedBatchSizeMap::const_iterator seenIt = batchSizes.find(&write->endpoint);

        if (seenIt == batchSizes.end()) {
            // If this is the first item in the batch, it can't be too big
            continue;
        }

        const BatchSize& batchSize = seenIt->second;

        if (batchSize.numOps >= static_cast<int>(write_ops::kMaxWriteBatchSize)) {
            // Too many items in batch
            return true;
        }

        if (batchSize.sizeBytes + writeSizeBytes > BSONObjMaxUserSize) {
            // Batch would be too big
            return true;
        }
    }

    return false;
}

/**
 * Gets an estimated size of how much the particular write operation would add to the size of the
 * batch.
 */
//BatchWriteOp::targetBatch调用
//增删改流量经过代理，需要记录对应流量大小，这个计算过程就是该函数
int getWriteSizeBytes(const WriteOp& writeOp) {
    const BatchItemRef& item = writeOp.getWriteItem();
    const BatchedCommandRequest::BatchType batchType = item.getOpType();

    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        return item.getDocument().objsize();
    } else if (batchType == BatchedCommandRequest::BatchType_Update) {
        // Note: Be conservative here - it's okay if we send slightly too many batches
        auto collationSize =
            item.getUpdate().getCollation() ? item.getUpdate().getCollation()->objsize() : 0;
        auto estSize = item.getUpdate().getQ().objsize() + item.getUpdate().getU().objsize() +
            collationSize + kEstUpdateOverheadBytes;
        dassert(estSize >= item.getUpdate().toBSON().objsize());
        return estSize;
    } else if (batchType == BatchedCommandRequest::BatchType_Delete) {
        // Note: Be conservative here - it's okay if we send slightly too many batches
        auto collationSize =
            item.getDelete().getCollation() ? item.getDelete().getCollation()->objsize() : 0;
        auto estSize = item.getDelete().getQ().objsize() + collationSize + kEstDeleteOverheadBytes;
        dassert(estSize >= item.getDelete().toBSON().objsize());
        return estSize;
    }

    MONGO_UNREACHABLE;
}

void cloneCommandErrorTo(const BatchedCommandResponse& batchResp, WriteErrorDetail* details) {
    details->setErrCode(batchResp.getErrCode());
    details->setErrMessage(batchResp.getErrMessage());
}

void toWriteErrorResponse(const WriteErrorDetail& error,
                          bool ordered,
                          int numWrites,
                          BatchedCommandResponse* writeErrResponse) {
    writeErrResponse->setOk(true);
    writeErrResponse->setN(0);

    int numErrors = ordered ? 1 : numWrites;
    for (int i = 0; i < numErrors; i++) {
        unique_ptr<WriteErrorDetail> errorClone(new WriteErrorDetail);
        error.cloneTo(errorClone.get());
        errorClone->setIndex(i);
        writeErrResponse->addToErrDetails(errorClone.release());
    }

    dassert(writeErrResponse->isValid(NULL));
}

/**
 * Given *either* a batch error or an array of per-item errors, copies errors we're interested in
 * into a TrackedErrorMap
 */
void trackErrors(const ShardEndpoint& endpoint,
                 const vector<WriteErrorDetail*> itemErrors,
                 TrackedErrors* trackedErrors) {
    for (vector<WriteErrorDetail*>::const_iterator it = itemErrors.begin(); it != itemErrors.end();
         ++it) {
        const WriteErrorDetail* error = *it;

        if (trackedErrors->isTracking(error->getErrCode())) {
            trackedErrors->addError(new ShardError(endpoint, *error));
        }
    }
}

}  // namespace

//BatchWriteExec::executeBatch
//ClusterWriter::write->BatchWriteExec::executeBatch
BatchWriteOp::BatchWriteOp(OperationContext* opCtx, const BatchedCommandRequest& clientRequest)
    : _opCtx(opCtx), _clientRequest(clientRequest), _batchTxnNum(_opCtx->getTxnNumber()) {
    _writeOps.reserve(_clientRequest.sizeWriteOps());

    for (size_t i = 0; i < _clientRequest.sizeWriteOps(); ++i) {
		//代表这个批量操作中的具体第几个写操作
        _writeOps.emplace_back(BatchItemRef(&_clientRequest, i));
    }
}

BatchWriteOp::~BatchWriteOp() {
    // Caller's responsibility to dispose of TargetedBatches
    invariant(_targeted.empty());
}

//BatchWriteExec::executeBatch  获取该BatchWriteOp对应的targetedBatches，也就是确定BatchWriteOp对应的文档应该发送到后端那些mongod分片
//批量写入，把需要写入到同一个shard的数据组装到一起，通过targetedBatches返回
Status BatchWriteOp::targetBatch(const NSTargeter& targeter, //ChunkManagerTargeter
                                 bool recordTargetErrors,
                                 std::map<ShardId, TargetedWriteBatch*>* targetedBatches) {
    //
    // Targeting of unordered batches is fairly simple - each remaining write op is targeted,
    // and each of those targeted writes are grouped into a batch for a particular shard
    // endpoint.
    //
    // Targeting of ordered batches is a bit more complex - to respect the ordering of the
    // batch, we can only send:
    // A) a single targeted batch to one shard endpoint
    // B) multiple targeted batches, but only containing targeted writes for a single write op
    //
    // This means that any multi-shard write operation must be targeted and sent one-by-one.
    // Subsequent single-shard write operations can be batched together if they go to the same
    // place.
    //
    // Ex: ShardA : { skey : a->k }, ShardB : { skey : k->z }
    //
    // Ordered insert batch of: [{ skey : a }, { skey : b }, { skey : x }]
    // broken into:
    //  [{ skey : a }, { skey : b }],
    //  [{ skey : x }]
    //
    // Ordered update Batch of :
    //  [{ skey : a }{ $push },
    //   { skey : b }{ $push },
    //   { skey : [c, x] }{ $push },
    //   { skey : y }{ $push },
    //   { skey : z }{ $push }]
    // broken into:
    //  [{ skey : a }, { skey : b }],
    //  [{ skey : [c,x] }],
    //  [{ skey : y }, { skey : z }]
    //

	//获取ordered参数，也就是一批数据中间写入成功，后续数据是否需要继续写入
    const bool ordered = _clientRequest.getWriteCommandBase().getOrdered();

	//using TargetedBatchMap = std::map<const ShardEndpoint*, TargetedWriteBatch*, EndpointComp>;
	//批量写入，把需要写入到同一个shard的数据组装到同一个TargetedWriteBatch中，map格式: <endpoint: TargetedWriteBatch>
    TargetedBatchMap batchMap;
	//批量写入，把需要写入到同一个shard的数据进行计数，map格式: <endpoint: BatchSize()>
    TargetedBatchSizeMap batchSizes;

    int numTargetErrors = 0;

	//BatchedCommandRequest::sizeWriteOps
	//总文档数
    const size_t numWriteOps = _clientRequest.sizeWriteOps(); //总文档数

    for (size_t i = 0; i < numWriteOps; ++i) { 
		//获取批量写入的第i个操作
        WriteOp& writeOp = _writeOps[i];

        // Only target _Ready ops
        if (writeOp.getWriteState() != WriteOpState_Ready)
            continue;

        //
        // Get TargetedWrites from the targeter for the write operation
        //

        // TargetedWrites need to be owned once returned
        OwnedPointerVector<TargetedWrite> writesOwned;
		//对应目标,该操作应该被转发到那些shard，一个TargetedWrite实际上对应一个shard
        vector<TargetedWrite*>& writes = writesOwned.mutableVector();

		//获取该op对应的后端mongod节点TargetedWrite   应该发送该op对应的文档到后端那些mongod
		//WriteOp::targetWrites也就是对应目标后端mongod实例信息

		//WriteOp::targetWrites
		
		//根据请求中解析出的shardkey信息，获取对应ShardEndpoint信息，也就是应该转发到那个chunk和那个shard信息
		//该批量写操作中的一个writeOp应该转发到指定的targetedWrites对应shard中，这里为数组原因是删除 更新可能对应多个shard
		Status targetStatus = writeOp.targetWrites(_opCtx, targeter, &writes);
		//异常处理
        if (!targetStatus.isOK()) {
            WriteErrorDetail targetError;
            buildTargetError(targetStatus, &targetError);

            if (!recordTargetErrors) {
                // Cancel current batch state with an error
                _cancelBatches(targetError, std::move(batchMap));
                return targetStatus;
            } else if (!ordered || batchMap.empty()) {
                // Record an error for this batch

                writeOp.setOpError(targetError);
                ++numTargetErrors;

                if (ordered)
                    return Status::OK();

                continue;
            } else {
                dassert(ordered && !batchMap.empty());

                // Send out what we have, but don't record an error yet, since there may be an error
                // in the writes before this point.
                writeOp.cancelWrites(&targetError);
                break;
            }
        }

        //
        // If ordered and we have a previous endpoint, make sure we don't need to send these
        // targeted writes to any other endpoints.
        //

        if (ordered && !batchMap.empty()) {
            dassert(batchMap.size() == 1u);
            if (isNewBatchRequired(writes, batchMap)) {
                writeOp.cancelWrites(NULL);
                break;
            }
        }

        // Account the array overhead once for the actual updates array and once for the statement
        // ids array, if retryable writes are used
        const int writeSizeBytes = getWriteSizeBytes(writeOp) + kBSONArrayPerElementOverheadBytes +
            (_batchTxnNum ? kBSONArrayPerElementOverheadBytes + 4 : 0);

        // If this write will push us over some sort of size limit, stop targeting
        if (wouldMakeBatchesTooBig(writes, writeSizeBytes, batchSizes)) {//文档长度检查wouldMakeBatchesTooBig
            invariant(!batchMap.empty());
            writeOp.cancelWrites(NULL);
            break;
        }

        //
        // Targeting went ok, add to appropriate TargetedBatch
        //
		//把需要转发到同一个shard的TargetedWrite组装到同一个TargetedWriteBatch
		//同时按照shard 转发纬度，进行内容计数
        for (vector<TargetedWrite*>::iterator it = writes.begin(); it != writes.end(); ++it) {
            TargetedWrite* write = *it;

			//using TargetedBatchMap = std::map<const ShardEndpoint*, TargetedWriteBatch*, EndpointComp>;
			//typedef std::map<const ShardEndpoint*, BatchSize, EndpointComp> TargetedBatchSizeMap;
            TargetedBatchMap::iterator batchIt = batchMap.find(&write->endpoint);
            TargetedBatchSizeMap::iterator batchSizeIt = batchSizes.find(&write->endpoint);

			//可以去重，保证相同的endpoint不会在batchMap中重复
            if (batchIt == batchMap.end()) { //没找到，则根据endpoint获取一个新的 TargetedWriteBatch  
            
                TargetedWriteBatch* newBatch = new TargetedWriteBatch(write->endpoint);
                batchIt = batchMap.insert(make_pair(&newBatch->getEndpoint(), newBatch)).first;
                batchSizeIt =
                    batchSizes.insert(make_pair(&newBatch->getEndpoint(), BatchSize())).first;
            }

			//
            TargetedWriteBatch* batch = batchIt->second;
            BatchSize& batchSize = batchSizeIt->second;

            ++batchSize.numOps;

            // If the request contains transaction number, this means the end result will contain a
            // statement ids array, so we need to account for that overhead.
            batchSize.sizeBytes += writeSizeBytes;
			//TargetedWriteBatch::addWrite
            batch->addWrite(write); 
        }

        // Relinquish ownership of TargetedWrites, now the TargetedBatches own them
        writesOwned.mutableVector().clear();

        //
        // Break if we're ordered and we have more than one endpoint - later writes cannot be
        // enforced as ordered across multiple shard endpoints.
        //

		//如果请求中ordered为ture，并且这批数据会写入到多个分片，这时候则无法保证有序，
		//因为以下原因: 假设一批数据100条，insert操作，2个分片，如果一个分片中间数据写入异常，
		//  这时候另一个分片无法回滚，保证数据一定会全部成功写入
        if (ordered && batchMap.size() > 1u)
            break;
    }

    //
    // Send back our targeted batches
    //

    for (TargetedBatchMap::iterator it = batchMap.begin(); it != batchMap.end(); ++it) {
        TargetedWriteBatch* batch = it->second;

        if (batch->getWrites().empty())
            continue;

        // Remember targeted batch for reporting
        _targeted.insert(batch);

        // Send the handle back to caller
        invariant(targetedBatches->find(batch->getEndpoint().shardName) == targetedBatches->end());
		//该batch对应的文档信息应该转发到shardName节点
        targetedBatches->insert(std::make_pair(batch->getEndpoint().shardName, batch)); //向targetedBatches添加map成员
    }

    return Status::OK();
}

//BatchWriteExec::executeBatch
//根据targetedBatch生成到后端的BatchedCommandRequest
BatchedCommandRequest BatchWriteOp::buildBatchRequest(
	//记录需要转发到指定分片的数据
    const TargetedWriteBatch& targetedBatch) const {
    //是增删改中的那种操作类型
    const auto batchType = _clientRequest.getBatchType();

    boost::optional<std::vector<int32_t>> stmtIdsForOp;
    if (_batchTxnNum) {
        stmtIdsForOp.emplace();
    }

    boost::optional<std::vector<BSONObj>> insertDocs;
    boost::optional<std::vector<write_ops::UpdateOpEntry>> updates;
    boost::optional<std::vector<write_ops::DeleteOpEntry>> deletes;

	//遍历targetedBatch，把所有增、删、改操作添加到对应容器中
    for (const auto& targetedWrite : targetedBatch.getWrites()) {
        const WriteOpRef& writeOpRef = targetedWrite->writeOpRef;

        switch (batchType) {
            case BatchedCommandRequest::BatchType_Insert:
                if (!insertDocs)
                    insertDocs.emplace();
                insertDocs->emplace_back(
                    _clientRequest.getInsertRequest().getDocuments().at(writeOpRef.first));
                break;
            case BatchedCommandRequest::BatchType_Update:
                if (!updates)
                    updates.emplace();
                updates->emplace_back(
                    _clientRequest.getUpdateRequest().getUpdates().at(writeOpRef.first));
                break;
            case BatchedCommandRequest::BatchType_Delete:
                if (!deletes)
                    deletes.emplace();
                deletes->emplace_back(
                    _clientRequest.getDeleteRequest().getDeletes().at(writeOpRef.first));
                break;
            default:
                MONGO_UNREACHABLE;
        }

		//为每个操作生成一个stmid
        if (stmtIdsForOp) {
            stmtIdsForOp->push_back(write_ops::getStmtIdForWriteAt(
                _clientRequest.getWriteCommandBase(), writeOpRef.first));
        }
    }

	//根据上面获取的增删改操作，生成对应的BatchedCommandRequest
	//到同一个shard的多条操作都封装到该BatchedCommandRequest中
    BatchedCommandRequest request([&] {
        switch (batchType) {
            case BatchedCommandRequest::BatchType_Insert:
                return BatchedCommandRequest([&] {
                    write_ops::Insert insertOp(_clientRequest.getNS());
                    insertOp.setDocuments(std::move(*insertDocs));
                    return insertOp;
                }());
            case BatchedCommandRequest::BatchType_Update:
                return BatchedCommandRequest([&] {
                    write_ops::Update updateOp(_clientRequest.getNS());
                    updateOp.setUpdates(std::move(*updates));
                    return updateOp;
                }());
            case BatchedCommandRequest::BatchType_Delete:
                return BatchedCommandRequest([&] {
                    write_ops::Delete deleteOp(_clientRequest.getNS());
                    deleteOp.setDeletes(std::move(*deletes));
                    return deleteOp;
                }());
        }
        MONGO_UNREACHABLE;
    }());

	//设置bypass  order参数  TxnNum等
    request.setWriteCommandBase([&] {
        write_ops::WriteCommandBase wcb;

        wcb.setBypassDocumentValidation(
            _clientRequest.getWriteCommandBase().getBypassDocumentValidation());
        wcb.setOrdered(_clientRequest.getWriteCommandBase().getOrdered());

        if (_batchTxnNum) {
            wcb.setStmtIds(std::move(stmtIdsForOp));
        }

        return wcb;
    }());

	//设置shardVersion信息
	//BatchedCommandRequest::setShardVersion
    request.setShardVersion(targetedBatch.getEndpoint().shardVersion);

	//WriteConcern设置
    if (_clientRequest.hasWriteConcern()) {
        if (_clientRequest.isVerboseWC()) {
            request.setWriteConcern(_clientRequest.getWriteConcern());
        } else {
            // Mongos needs to send to the shard with w > 0 so it will be able to see the
            // writeErrors
            request.setWriteConcern(upgradeWriteConcern(_clientRequest.getWriteConcern()));
        }
    }

    return request;
}

//BatchWriteExec::executeBatch  BatchWriteOp::noteBatchError  BatchWriteExec::executeBatch执行
//写后端的状态记录，如果后端应答路由版本比本地缓存高，则从新刷新路由获取最新路由后，重定向到新的分片
void BatchWriteOp::noteBatchResponse(const TargetedWriteBatch& targetedBatch,
                                     const BatchedCommandResponse& response,
                                     TrackedErrors* trackedErrors) {
    //BatchedCommandResponse::getOk
    if (!response.getOk()) {
        WriteErrorDetail error;
        cloneCommandErrorTo(response, &error);

        // Treat command errors exactly like other failures of the batch
        // Note that no errors will be tracked from these failures - as-designed
        noteBatchError(targetedBatch, error);
        return;
    }

    dassert(response.getOk());

    // Stop tracking targeted batch
    _targeted.erase(&targetedBatch);

    // Increment stats for this batch
    _incBatchStats(response);//计数统计

    //
    // Assign errors to particular items.
    // Write Concern errors are stored and handled later.
    //

    // Special handling for write concern errors, save for later
    if (response.isWriteConcernErrorSet()) {
        _wcErrors.emplace_back(targetedBatch.getEndpoint(), *response.getWriteConcernError());
    }

    vector<WriteErrorDetail*> itemErrors;

    // Handle batch and per-item errors
    if (response.isErrDetailsSet()) {
        // Per-item errors were set
        itemErrors.insert(
            itemErrors.begin(), response.getErrDetails().begin(), response.getErrDetails().end());

        // Sort per-item errors by index
        std::sort(itemErrors.begin(), itemErrors.end(), WriteErrorDetailComp());
    }

    //
    // Go through all pending responses of the op and sorted remote reponses, populate errors
    // This will either set all errors to the batch error or apply per-item errors as-needed
    //
    // If the batch is ordered, cancel all writes after the first error for retargeting.
    //

    const bool ordered = _clientRequest.getWriteCommandBase().getOrdered();

    vector<WriteErrorDetail*>::iterator itemErrorIt = itemErrors.begin();
    int index = 0;
    WriteErrorDetail* lastError = NULL;
    for (vector<TargetedWrite *>::const_iterator it = targetedBatch.getWrites().begin();
         it != targetedBatch.getWrites().end();
         ++it, ++index) {
        const TargetedWrite* write = *it;
        WriteOp& writeOp = _writeOps[write->writeOpRef.first];

        dassert(writeOp.getWriteState() == WriteOpState_Pending);

        // See if we have an error for the write
        WriteErrorDetail* writeError = NULL;

        if (itemErrorIt != itemErrors.end() && (*itemErrorIt)->getIndex() == index) {
            // We have an per-item error for this write op's index
            writeError = *itemErrorIt;
            ++itemErrorIt;
        }

        // Finish the response (with error, if needed)
        if (NULL == writeError) {
            if (!ordered || !lastError) {
				//转发完成状态记录
                writeOp.noteWriteComplete(*write);
            } else {
                // We didn't actually apply this write - cancel so we can retarget
                //例如mongos路由信息版本过低，后端应答高版本信息，代理刷新路由后重试回走这里
                dassert(writeOp.getNumTargeted() == 1u);
                writeOp.cancelWrites(lastError);
            }
        } else {
        	//后端返回异常，说明到该后端的一批数据转发异常了，无需重试
            writeOp.noteWriteError(*write, *writeError);
            lastError = writeError;
        }
    }

    // Track errors we care about, whether batch or individual errors
    if (NULL != trackedErrors) {
        trackErrors(targetedBatch.getEndpoint(), itemErrors, trackedErrors);
    }

    // Track upserted ids if we need to
    if (response.isUpsertDetailsSet()) {
        const vector<BatchedUpsertDetail*>& upsertedIds = response.getUpsertDetails();
        for (vector<BatchedUpsertDetail*>::const_iterator it = upsertedIds.begin();
             it != upsertedIds.end();
             ++it) {
            // The child upserted details don't have the correct index for the full batch
            const BatchedUpsertDetail* childUpsertedId = *it;

            // Work backward from the child batch item index to the batch item index
            int childBatchIndex = childUpsertedId->getIndex();
            int batchIndex = targetedBatch.getWrites()[childBatchIndex]->writeOpRef.first;

            // Push the upserted id with the correct index into the batch upserted ids
            auto upsertedId = stdx::make_unique<BatchedUpsertDetail>();
            upsertedId->setIndex(batchIndex);
            upsertedId->setUpsertedID(childUpsertedId->getUpsertedID());
            _upsertedIds.push_back(std::move(upsertedId));
        }
    }
}

//BatchWriteOp::noteBatchResponse  BatchWriteExec::executeBatch
void BatchWriteOp::noteBatchError(const TargetedWriteBatch& targetedBatch,
                                  const WriteErrorDetail& error) {
    // Treat errors to get a batch response as failures of the contained writes
    BatchedCommandResponse emulatedResponse;
    toWriteErrorResponse(error,
                         _clientRequest.getWriteCommandBase().getOrdered(),
                         targetedBatch.getWrites().size(),
                         &emulatedResponse);

    noteBatchResponse(targetedBatch, emulatedResponse, NULL);
}

void BatchWriteOp::abortBatch(const WriteErrorDetail& error) {
    dassert(!isFinished());
    dassert(numWriteOpsIn(WriteOpState_Pending) == 0);

    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    const bool orderedOps = _clientRequest.getWriteCommandBase().getOrdered();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
        // Can only be called with no outstanding batches
        dassert(writeOp.getWriteState() != WriteOpState_Pending);

        if (writeOp.getWriteState() < WriteOpState_Completed) {
            writeOp.setOpError(error);

            // Only one error if we're ordered
            if (orderedOps)
                break;
        }
    }

    dassert(isFinished());
}

//BatchWriteExec::executeBatch中调用
//写相关的一批数据是否转发到后端全部完成
bool BatchWriteOp::isFinished() {
    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    const bool orderedOps = _clientRequest.getWriteCommandBase().getOrdered();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
		//例如后端返回版本不匹配，这时候不算完成，获取最新路由后重新执行
        if (writeOp.getWriteState() < WriteOpState_Completed) //该文档写入到后端成功，赋值见BatchWriteOp::noteBatchResponse
            return false;
		//如果order参数为ture，则一批数据中间任何一条写入失败，都不进行后续数据写入
		//
        else if (orderedOps && writeOp.getWriteState() == WriteOpState_Error)
            return true;
    }

    return true;
}

//BatchWriteExec::executeBatch
void BatchWriteOp::buildClientResponse(BatchedCommandResponse* batchResp) {
    dassert(isFinished());

    // Result is OK
    batchResp->setOk(true);

    // For non-verbose, it's all we need.
    if (!_clientRequest.isVerboseWC()) {
        dassert(batchResp->isValid(NULL));
        return;
    }

    //
    // Find all the errors in the batch
    //

    vector<WriteOp*> errOps;

    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];

        if (writeOp.getWriteState() == WriteOpState_Error) {
            errOps.push_back(&writeOp);
        }
    }

    //
    // Build the per-item errors.
    //

    if (!errOps.empty()) {
        for (vector<WriteOp*>::iterator it = errOps.begin(); it != errOps.end(); ++it) {
            WriteOp& writeOp = **it;
            WriteErrorDetail* error = new WriteErrorDetail();
            writeOp.getOpError().cloneTo(error);
            batchResp->addToErrDetails(error);
        }
    }

    // Only return a write concern error if everything succeeded (unordered or ordered)
    // OR if something succeeded and we're unordered
    const bool orderedOps = _clientRequest.getWriteCommandBase().getOrdered();
    const bool reportWCError =
        errOps.empty() || (!orderedOps && errOps.size() < _clientRequest.sizeWriteOps());
    if (!_wcErrors.empty() && reportWCError) {
        WriteConcernErrorDetail* error = new WriteConcernErrorDetail;

        // Generate the multi-error message below
        StringBuilder msg;
        if (_wcErrors.size() > 1) {
            msg << "multiple errors reported : ";
            error->setErrCode(ErrorCodes::WriteConcernFailed);
        } else {
            error->setErrCode(_wcErrors.begin()->error.getErrCode());
        }

        for (auto it = _wcErrors.begin(); it != _wcErrors.end(); ++it) {
            const auto& wcError = *it;
            if (it != _wcErrors.begin()) {
                msg << " :: and :: ";
            }
            msg << wcError.error.getErrMessage() << " at " << wcError.endpoint.shardName;
        }

        error->setErrMessage(msg.str());
        batchResp->setWriteConcernError(error);
    }

    //
    // Append the upserted ids, if required
    //

    if (_upsertedIds.size() != 0) {
        batchResp->setUpsertDetails(transitional_tools_do_not_use::unspool_vector(_upsertedIds));
    }

    // Stats
    const int nValue = _numInserted + _numUpserted + _numMatched + _numDeleted;
    batchResp->setN(nValue);
    if (_clientRequest.getBatchType() == BatchedCommandRequest::BatchType_Update &&
        _numModified >= 0) {
        batchResp->setNModified(_numModified);
    }

    dassert(batchResp->isValid(NULL));
}

int BatchWriteOp::numWriteOpsIn(WriteOpState opState) const {
    // TODO: This could be faster, if we tracked this info explicitly
    return std::accumulate(
        _writeOps.begin(), _writeOps.end(), 0, [opState](int sum, const WriteOp& writeOp) {
            return sum + (writeOp.getWriteState() == opState ? 1 : 0);
        });
}

//后端应答后，做统计   BatchWriteOp::noteBatchResponse
void BatchWriteOp::_incBatchStats(const BatchedCommandResponse& response) {
    const auto batchType = _clientRequest.getBatchType();

    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        _numInserted += response.getN(); //BatchedCommandResponse::getN
    } else if (batchType == BatchedCommandRequest::BatchType_Update) {
        int numUpserted = 0;
        if (response.isUpsertDetailsSet()) { //BatchedCommandResponse::isUpsertDetailsSet
            numUpserted = response.sizeUpsertDetails();
        }
        _numMatched += (response.getN() - numUpserted); //update成功，也就是确实有某条满足条件的数据，并且做了更新
        long long numModified = response.getNModified();//BatchedCommandResponse::getNModified

        if (numModified >= 0)
            _numModified += numModified;
        else
            _numModified = -1;  // sentinel used to indicate we omit the field downstream

        _numUpserted += numUpserted;
    } else {
        dassert(batchType == BatchedCommandRequest::BatchType_Delete);
        _numDeleted += response.getN();
    }
}

void BatchWriteOp::_cancelBatches(const WriteErrorDetail& why,
                                  TargetedBatchMap&& batchMapToCancel) {
    TargetedBatchMap batchMap(batchMapToCancel);

    // Collect all the writeOps that are currently targeted
    for (TargetedBatchMap::iterator it = batchMap.begin(); it != batchMap.end();) {
        TargetedWriteBatch* batch = it->second;
        const vector<TargetedWrite*>& writes = batch->getWrites();

        for (vector<TargetedWrite*>::const_iterator writeIt = writes.begin();
             writeIt != writes.end();
             ++writeIt) {
            TargetedWrite* write = *writeIt;

            // NOTE: We may repeatedly cancel a write op here, but that's fast and we want to cancel
            // before erasing the TargetedWrite* (which owns the cancelled targeting info) for
            // reporting reasons.
            _writeOps[write->writeOpRef.first].cancelWrites(&why);
        }

        // Note that we need to *erase* first, *then* delete, since the map keys are ptrs from
        // the values
        batchMap.erase(it++);
        delete batch;
    }
}

bool EndpointComp::operator()(const ShardEndpoint* endpointA,
                              const ShardEndpoint* endpointB) const {
    const int shardNameDiff = endpointA->shardName.compare(endpointB->shardName);
    if (shardNameDiff) {
        return shardNameDiff < 0;
    }

    const long shardVersionDiff =
        endpointA->shardVersion.toLong() - endpointB->shardVersion.toLong();
    if (shardVersionDiff) {
        return shardVersionDiff < 0;
    }

    return endpointA->shardVersion.epoch().compare(endpointB->shardVersion.epoch()) < 0;
}

void TrackedErrors::startTracking(int errCode) {
    dassert(!isTracking(errCode));
    _errorMap.insert(make_pair(errCode, vector<ShardError*>()));
}

bool TrackedErrors::isTracking(int errCode) const {
    return _errorMap.find(errCode) != _errorMap.end();
}

void TrackedErrors::addError(ShardError* error) {
    TrackedErrorMap::iterator seenIt = _errorMap.find(error->error.getErrCode());
    if (seenIt == _errorMap.end())
        return;
    seenIt->second.push_back(error);
}

const vector<ShardError*>& TrackedErrors::getErrors(int errCode) const {
    dassert(isTracking(errCode));
    return _errorMap.find(errCode)->second;
}

void TrackedErrors::clear() {
    for (TrackedErrorMap::iterator it = _errorMap.begin(); it != _errorMap.end(); ++it) {
        vector<ShardError*>& errors = it->second;

        for (vector<ShardError*>::iterator errIt = errors.begin(); errIt != errors.end(); ++errIt) {
            delete *errIt;
        }
        errors.clear();
    }
}

TrackedErrors::~TrackedErrors() {
    clear();
}

}  // namespace mongo
