/**
 *    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_executor.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/pipeline_proxy.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

const OperationContext::Decoration<bool> shouldWaitForInserts =
    OperationContext::declareDecoration<bool>();
const OperationContext::Decoration<repl::OpTime> clientsLastKnownCommittedOpTime =
    OperationContext::declareDecoration<repl::OpTime>();

struct CappedInsertNotifierData {
    shared_ptr<CappedInsertNotifier> notifier;
    uint64_t lastEOFVersion = ~0;
};

namespace {

MONGO_FP_DECLARE(planExecutorAlwaysFails);

/**
 * Constructs a PlanYieldPolicy based on 'policy'.
 */
std::unique_ptr<PlanYieldPolicy> makeYieldPolicy(PlanExecutor* exec,
                                                 PlanExecutor::YieldPolicy policy) {
    switch (policy) {
        case PlanExecutor::YieldPolicy::YIELD_AUTO:
        case PlanExecutor::YieldPolicy::YIELD_MANUAL:
        case PlanExecutor::YieldPolicy::NO_YIELD:
        case PlanExecutor::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY: {
            return stdx::make_unique<PlanYieldPolicy>(exec, policy);
        }
        case PlanExecutor::YieldPolicy::ALWAYS_TIME_OUT: {
            return stdx::make_unique<AlwaysTimeOutYieldPolicy>(exec);
        }
        case PlanExecutor::YieldPolicy::ALWAYS_MARK_KILLED: {
            return stdx::make_unique<AlwaysPlanKilledYieldPolicy>(exec);
        }
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Retrieves the first stage of a given type from the plan tree, or NULL
 * if no such stage is found.
 */
PlanStage* getStageByType(PlanStage* root, StageType type) {
    if (root->stageType() == type) {
        return root;
    }

    const auto& children = root->getChildren();
    for (size_t i = 0; i < children.size(); i++) {
        PlanStage* result = getStageByType(children[i].get(), type);
        if (result) {
            return result;
        }
    }

    return NULL;
}
}  // namespace

// static
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    const Collection* collection,
    YieldPolicy yieldPolicy) {
    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(rt), nullptr, nullptr, collection, {}, yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    NamespaceString nss,
    YieldPolicy yieldPolicy) {
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(rt),
                              nullptr,
                              nullptr,
                              nullptr,
                              std::move(nss),
                              yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    unique_ptr<CanonicalQuery> cq,
    const Collection* collection,
    YieldPolicy yieldPolicy) {
    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(rt), nullptr, std::move(cq), collection, {}, yieldPolicy);
}

// static   getExecutor中执行
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    unique_ptr<QuerySolution> qs,
    unique_ptr<CanonicalQuery> cq,
    const Collection* collection,
    YieldPolicy yieldPolicy) {
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(rt),
                              std::move(qs),
                              std::move(cq),
                              collection,
                              {},
                              yieldPolicy);
}

// static   getExecutor getExecutorUpdate getExecutorDelete中执行  这里面会pickBestPlan选取最优的plan
//初始化PlanExecutor类型,并且调用pickBestPlan选取最优的Plan.里面包含了很多不同类型的PlanStage
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> 
   PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    unique_ptr<QuerySolution> qs,
    unique_ptr<CanonicalQuery> cq,
    const Collection* collection,
    NamespaceString nss,
    YieldPolicy yieldPolicy) {

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec(
        new PlanExecutor(opCtx,
                         std::move(ws),
                         std::move(rt),
                         std::move(qs),
                         std::move(cq),
                         collection,
                         std::move(nss),
                         yieldPolicy),
        PlanExecutor::Deleter(opCtx, collection));

    // Perform plan selection, if necessary.
    //PlanExecutor::pickBestPlan  调用pickBestPlan选取最优的Plan.里面包含了很多不同类型的PlanStage
    Status status = exec->pickBestPlan(collection);
    if (!status.isOK()) {
        return status;
    }

    return std::move(exec);
}

//PlanExecutor::make中调用执行
PlanExecutor::PlanExecutor(OperationContext* opCtx,
                           unique_ptr<WorkingSet> ws,
                           unique_ptr<PlanStage> rt,
                           unique_ptr<QuerySolution> qs,
                           unique_ptr<CanonicalQuery> cq,
                           const Collection* collection,
                           NamespaceString nss,
                           YieldPolicy yieldPolicy)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _workingSet(std::move(ws)),
      _qs(std::move(qs)),
      _root(std::move(rt)),
      _nss(std::move(nss)),
      // There's no point in yielding if the collection doesn't exist.
      _yieldPolicy(makeYieldPolicy(this, collection ? yieldPolicy : NO_YIELD)) {
    // We may still need to initialize _nss from either collection or _cq.
    if (!_nss.isEmpty()) {
        return;  // We already have an _nss set, so there's nothing more to do.
    }

    if (collection) {
        _nss = collection->ns();
        if (_yieldPolicy->canReleaseLocksDuringExecution()) {
            _registrationToken = collection->getCursorManager()->registerExecutor(this);
        }
    } else {
        invariant(_cq);
        _nss = _cq->getQueryRequest().nss();
    }
}

//PlanExecutor::make中调用  调用pickBestPlan选取最优的Plan.里面包含了很多不同类型的PlanStage
Status PlanExecutor::pickBestPlan(const Collection* collection) {
    invariant(_currentState == kUsable);

	//以下几种情况和prepareExecution中是对应的

    // First check if we need to do subplanning.
    PlanStage* foundStage = getStageByType(_root.get(), STAGE_SUBPLAN);
    if (foundStage) {
        SubplanStage* subplan = static_cast<SubplanStage*>(foundStage);
		//SubplanStage::pickBestPlan,这里面可能形成递归调用
        return subplan->pickBestPlan(_yieldPolicy.get());
    }

    // If we didn't have to do subplanning, we might still have to do regular
    // multi plan selection...
    //对应MultiPlanStage，如果一个查询有多个候选索引满足条件，则会有多个solution
    //所以需要选择最优的plan，配合prepareExecution阅读
    foundStage = getStageByType(_root.get(), STAGE_MULTI_PLAN); //multi plan
    if (foundStage) {
        MultiPlanStage* mps = static_cast<MultiPlanStage*>(foundStage);
		//MultiPlanStage::pickBestPlan 
        return mps->pickBestPlan(_yieldPolicy.get());
    }

    // ...or, we might have to run a plan from the cache for a trial period, falling back on
    // regular planning if the cached plan performs poorly.
    foundStage = getStageByType(_root.get(), STAGE_CACHED_PLAN); //cache plan
    if (foundStage) {
        CachedPlanStage* cachedPlan = static_cast<CachedPlanStage*>(foundStage);
		//CachedPlanStage::pickBestPlan
        return cachedPlan->pickBestPlan(_yieldPolicy.get());
    }

	//只有一个满足条件的PlanStage，则这里直接返回
    // Either we chose a plan, or no plan selection was required. In both cases,
    // our work has been successfully completed.
    return Status::OK();
}

PlanExecutor::~PlanExecutor() {
    invariant(_currentState == kDisposed);
}

// static
string PlanExecutor::statestr(ExecState s) {
    if (PlanExecutor::ADVANCED == s) {
        return "ADVANCED";
    } else if (PlanExecutor::IS_EOF == s) {
        return "IS_EOF";
    } else if (PlanExecutor::DEAD == s) {
        return "DEAD";
    } else {
        verify(PlanExecutor::FAILURE == s);
        return "FAILURE";
    }
}

WorkingSet* PlanExecutor::getWorkingSet() const {
    return _workingSet.get();
}

PlanStage* PlanExecutor::getRootStage() const {
    return _root.get();
}

CanonicalQuery* PlanExecutor::getCanonicalQuery() const {
    return _cq.get();
}

unique_ptr<PlanStageStats> PlanExecutor::getStats() const {
    return _root->getStats();
}

BSONObjSet PlanExecutor::getOutputSorts() const {
    if (_qs && _qs->root) {
        _qs->root->computeProperties();
        return _qs->root->getSort();
    }

    if (_root->stageType() == STAGE_MULTI_PLAN) {
        // If we needed a MultiPlanStage, the PlanExecutor does not own the QuerySolution. We
        // must go through the MultiPlanStage to access the output sort.
        auto multiPlanStage = static_cast<MultiPlanStage*>(_root.get());
        if (multiPlanStage->bestSolution()) {
            multiPlanStage->bestSolution()->root->computeProperties();
            return multiPlanStage->bestSolution()->root->getSort();
        }
    } else if (_root->stageType() == STAGE_SUBPLAN) {
        auto subplanStage = static_cast<SubplanStage*>(_root.get());
        if (subplanStage->compositeSolution()) {
            subplanStage->compositeSolution()->root->computeProperties();
            return subplanStage->compositeSolution()->root->getSort();
        }
    }

    return SimpleBSONObjComparator::kInstance.makeBSONObjSet();
}

OperationContext* PlanExecutor::getOpCtx() const {
    return _opCtx;
}

void PlanExecutor::saveState() {
    invariant(_currentState == kUsable || _currentState == kSaved);

    // The query stages inside this stage tree might buffer record ids (e.g. text, geoNear,
    // mergeSort, sort) which are no longer protected by the storage engine's transactional
    // boundaries.
    WorkingSetCommon::prepareForSnapshotChange(_workingSet.get());

    if (!isMarkedAsKilled()) {
        _root->saveState();
    }
    _currentState = kSaved;
}

//加索引的时候，会通过MultiIndexBlockImpl::insertAllDocumentsInCollection走到这里
Status PlanExecutor::restoreState() {
    try {
        return restoreStateWithoutRetrying();
    } catch (const WriteConflictException&) {
        if (!_yieldPolicy->canAutoYield())
            throw;

        // Handles retries by calling restoreStateWithoutRetrying() in a loop.
        return _yieldPolicy->yield();
    }
}

Status PlanExecutor::restoreStateWithoutRetrying() {
    invariant(_currentState == kSaved);

    if (!isMarkedAsKilled()) {
        _root->restoreState();
    }

    _currentState = kUsable;
    return isMarkedAsKilled()
        ? Status{ErrorCodes::QueryPlanKilled, "query killed during yield: " + *_killReason}
        : Status::OK();
}

void PlanExecutor::detachFromOperationContext() {
    invariant(_currentState == kSaved);
    _opCtx = nullptr;
    _root->detachFromOperationContext();
    _currentState = kDetached;
    _everDetachedFromOperationContext = true;
}

void PlanExecutor::reattachToOperationContext(OperationContext* opCtx) {
    invariant(_currentState == kDetached);

    // We're reattaching for a getMore now.  Reset the yield timer in order to prevent from
    // yielding again right away.
    _yieldPolicy->resetTimer();

    _opCtx = opCtx;
    _root->reattachToOperationContext(opCtx);
    _currentState = kSaved;
}

void PlanExecutor::invalidate(OperationContext* opCtx, const RecordId& dl, InvalidationType type) {
    if (!isMarkedAsKilled()) {
        _root->invalidate(opCtx, dl, type);
    }
}

/*
(gdb) bt
#0  mongo::IndexScan::initIndexScan (this=this@entry=0x7f88328f8000) at src/mongo/db/exec/index_scan.cpp:102
#1  0x00007f882ae8172f in mongo::IndexScan::doWork (this=0x7f88328f8000, out=0x7f8829bcb918) at src/mongo/db/exec/index_scan.cpp:138
#2  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f88328f8000, out=out@entry=0x7f8829bcb918) at src/mongo/db/exec/plan_stage.cpp:46
#3  0x00007f882ae70855 in mongo::FetchStage::doWork (this=0x7f8832110500, out=0x7f8829bcb9e0) at src/mongo/db/exec/fetch.cpp:86
#4  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f8832110500, out=out@entry=0x7f8829bcb9e0) at src/mongo/db/exec/plan_stage.cpp:46
#5  0x00007f882ab6a823 in mongo::PlanExecutor::getNextImpl (this=0x7f8832362000, objOut=objOut@entry=0x7f8829bcba70, dlOut=dlOut@entry=0x0) at src/mongo/db/query/plan_executor.cpp:546
#6  0x00007f882ab6b16b in mongo::PlanExecutor::getNext (this=<optimized out>, objOut=objOut@entry=0x7f8829bcbb80, dlOut=dlOut@entry=0x0) at src/mongo/db/query/plan_executor.cpp:406
#7  0x00007f882a7cfc3d in mongo::(anonymous namespace)::FindCmd::run (this=this@entry=0x7f882caac740 <mongo::(anonymous namespace)::findCmd>, opCtx=opCtx@entry=0x7f883216fdc0, dbname=..., cmdObj=..., result=...)
    at src/mongo/db/commands/find_cmd.cpp:366

(gdb) bt
#0  mongo::IndexScan::initIndexScan (this=this@entry=0x7f8832913800) at src/mongo/db/exec/index_scan.cpp:102
#1  0x00007f882ae8172f in mongo::IndexScan::doWork (this=0x7f8832913800, out=0x7f8820d0dc18) at src/mongo/db/exec/index_scan.cpp:138
#2  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f8832913800, out=out@entry=0x7f8820d0dc18) at src/mongo/db/exec/plan_stage.cpp:46
#3  0x00007f882ae70855 in mongo::FetchStage::doWork (this=0x7f8832110880, out=0x7f8820d0dcf8) at src/mongo/db/exec/fetch.cpp:86
#4  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f8832110880, out=out@entry=0x7f8820d0dcf8) at src/mongo/db/exec/plan_stage.cpp:46
#5  0x00007f882ae6c318 in mongo::DeleteStage::doWork (this=0x7f8832363400, out=0x7f8820d0de40) at src/mongo/db/exec/delete.cpp:125
#6  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f8832363400, out=out@entry=0x7f8820d0de40) at src/mongo/db/exec/plan_stage.cpp:46
#7  0x00007f882ab6a823 in mongo::PlanExecutor::getNextImpl (this=this@entry=0x7f8832363500, objOut=objOut@entry=0x7f8820d0ded0, dlOut=dlOut@entry=0x0) at src/mongo/db/query/plan_executor.cpp:546
#8  0x00007f882ab6b16b in mongo::PlanExecutor::getNext (this=this@entry=0x7f8832363500, objOut=objOut@entry=0x7f8820d0df20, dlOut=dlOut@entry=0x0) at src/mongo/db/query/plan_executor.cpp:406
#9  0x00007f882ab6b26d in mongo::PlanExecutor::executePlan (this=0x7f8832363500) at src/mongo/db/query/plan_executor.cpp:665
#10 0x00007f882a76e92c in mongo::TTLMonitor::doTTLForIndex (this=this@entry=0x7f882e8cdfc0, opCtx=opCtx@entry=0x7f8832170180, idx=...) at src/mongo/db/ttl.cpp:263
#11 0x00007f882a76f5e0 in mongo::TTLMonitor::doTTLPass (this=this@entry=0x7f882e8cdfc0) at src/mongo/db/ttl.cpp:158
#12 0x00007f882a76fc08 in mongo::TTLMonitor::run (this=0x7f882e8cdfc0) at src/mongo/db/ttl.cpp:111
#13 0x00007f882bc3b221 in mongo::BackgroundJob::jobBody (this=0x7f882e8cdfc0) at src/mongo/util/background.cpp:150
*/
////FindCmd::run循环调用PlanExecutor的getNext函数获得查询结果.
PlanExecutor::ExecState PlanExecutor::getNext(BSONObj* objOut, RecordId* dlOut) {
    Snapshotted<BSONObj> snapshotted;
    ExecState state = getNextImpl(objOut ? &snapshotted : NULL, dlOut);

    if (objOut) {
        *objOut = snapshotted.value();
    }

    return state;
}

//建索引 MultiIndexBlockImpl::insertAllDocumentsInCollection中调用
//loc对应数据的key, objToIndex对应数据value
PlanExecutor::ExecState PlanExecutor::getNextSnapshotted(Snapshotted<BSONObj>* objOut,
                                                         RecordId* dlOut) {
    // Detaching from the OperationContext means that the returned snapshot ids could be invalid.
    invariant(!_everDetachedFromOperationContext);
    return getNextImpl(objOut, dlOut);
}

bool PlanExecutor::shouldWaitForInserts() {
    // If this is an awaitData-respecting operation and we have time left and we're not interrupted,
    // we should wait for inserts.
    if (_cq && _cq->getQueryRequest().isTailableAndAwaitData() &&
        mongo::shouldWaitForInserts(_opCtx) && _opCtx->checkForInterruptNoAssert().isOK() &&
        _opCtx->getRemainingMaxTimeMicros() > Microseconds::zero()) {
        // We expect awaitData cursors to be yielding.
        invariant(_yieldPolicy->canReleaseLocksDuringExecution());

        // For operations with a last committed opTime, we should not wait if the replication
        // coordinator's lastCommittedOpTime has changed.
        if (!clientsLastKnownCommittedOpTime(_opCtx).isNull()) {
            auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
            return clientsLastKnownCommittedOpTime(_opCtx) == replCoord->getLastCommittedOpTime();
        }
        return true;
    }
    return false;
}

std::shared_ptr<CappedInsertNotifier> PlanExecutor::getCappedInsertNotifier() {
    // We don't expect to need a capped insert notifier for non-yielding plans.
    invariant(_yieldPolicy->canReleaseLocksDuringExecution());

    // We can only wait if we have a collection; otherwise we should retry immediately when
    // we hit EOF.
    dassert(_opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IS));
    auto db = dbHolder().get(_opCtx, _nss.db());
    invariant(db);
    auto collection = db->getCollection(_opCtx, _nss);
    invariant(collection);

    return collection->getCappedInsertNotifier();
}

PlanExecutor::ExecState PlanExecutor::waitForInserts(CappedInsertNotifierData* notifierData,
                                                     Snapshotted<BSONObj>* errorObj) {
    invariant(notifierData->notifier);

    // The notifier wait() method will not wait unless the version passed to it matches the
    // current version of the notifier.  Since the version passed to it is the current version
    // of the notifier at the time of the previous EOF, we require two EOFs in a row with no
    // notifier version change in order to wait.  This is sufficient to ensure we never wait
    // when data is available.
    auto curOp = CurOp::get(_opCtx);
    curOp->pauseTimer();
    ON_BLOCK_EXIT([curOp] { curOp->resumeTimer(); });
    auto opCtx = _opCtx;
    uint64_t currentNotifierVersion = notifierData->notifier->getVersion();
    auto yieldResult = _yieldPolicy->yield(nullptr, [opCtx, notifierData] {
        const auto timeout = opCtx->getRemainingMaxTimeMicros();
        notifierData->notifier->wait(notifierData->lastEOFVersion, timeout);
    });
    notifierData->lastEOFVersion = currentNotifierVersion;
    if (yieldResult.isOK()) {
        // There may be more results, try to get more data.
        return ADVANCED;
    }
    return swallowTimeoutIfAwaitData(yieldResult, errorObj);
}

/*
(gdb) bt
#0  mongo::IndexScan::initIndexScan (this=this@entry=0x7f88328f8000) at src/mongo/db/exec/index_scan.cpp:102
#1  0x00007f882ae8172f in mongo::IndexScan::doWork (this=0x7f88328f8000, out=0x7f8829bcb918) at src/mongo/db/exec/index_scan.cpp:138
#2  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f88328f8000, out=out@entry=0x7f8829bcb918) at src/mongo/db/exec/plan_stage.cpp:46
#3  0x00007f882ae70855 in mongo::FetchStage::doWork (this=0x7f8832110500, out=0x7f8829bcb9e0) at src/mongo/db/exec/fetch.cpp:86
#4  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f8832110500, out=out@entry=0x7f8829bcb9e0) at src/mongo/db/exec/plan_stage.cpp:46
#5  0x00007f882ab6a823 in mongo::PlanExecutor::getNextImpl (this=0x7f8832362000, objOut=objOut@entry=0x7f8829bcba70, dlOut=dlOut@entry=0x0) at src/mongo/db/query/plan_executor.cpp:546
#6  0x00007f882ab6b16b in mongo::PlanExecutor::getNext (this=<optimized out>, objOut=objOut@entry=0x7f8829bcbb80, dlOut=dlOut@entry=0x0) at src/mongo/db/query/plan_executor.cpp:406
#7  0x00007f882a7cfc3d in mongo::(anonymous namespace)::FindCmd::run (this=this@entry=0x7f882caac740 <mongo::(anonymous namespace)::findCmd>, opCtx=opCtx@entry=0x7f883216fdc0, dbname=..., cmdObj=..., result=...)
    at src/mongo/db/commands/find_cmd.cpp:366

(gdb) bt
#0  mongo::IndexScan::initIndexScan (this=this@entry=0x7f8832913800) at src/mongo/db/exec/index_scan.cpp:102
#1  0x00007f882ae8172f in mongo::IndexScan::doWork (this=0x7f8832913800, out=0x7f8820d0dc18) at src/mongo/db/exec/index_scan.cpp:138
#2  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f8832913800, out=out@entry=0x7f8820d0dc18) at src/mongo/db/exec/plan_stage.cpp:46
#3  0x00007f882ae70855 in mongo::FetchStage::doWork (this=0x7f8832110880, out=0x7f8820d0dcf8) at src/mongo/db/exec/fetch.cpp:86
#4  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f8832110880, out=out@entry=0x7f8820d0dcf8) at src/mongo/db/exec/plan_stage.cpp:46
#5  0x00007f882ae6c318 in mongo::DeleteStage::doWork (this=0x7f8832363400, out=0x7f8820d0de40) at src/mongo/db/exec/delete.cpp:125
#6  0x00007f882ae952cb in mongo::PlanStage::work (this=0x7f8832363400, out=out@entry=0x7f8820d0de40) at src/mongo/db/exec/plan_stage.cpp:46
#7  0x00007f882ab6a823 in mongo::PlanExecutor::getNextImpl (this=this@entry=0x7f8832363500, objOut=objOut@entry=0x7f8820d0ded0, dlOut=dlOut@entry=0x0) at src/mongo/db/query/plan_executor.cpp:546
#8  0x00007f882ab6b16b in mongo::PlanExecutor::getNext (this=this@entry=0x7f8832363500, objOut=objOut@entry=0x7f8820d0df20, dlOut=dlOut@entry=0x0) at src/mongo/db/query/plan_executor.cpp:406
#9  0x00007f882ab6b26d in mongo::PlanExecutor::executePlan (this=0x7f8832363500) at src/mongo/db/query/plan_executor.cpp:665
#10 0x00007f882a76e92c in mongo::TTLMonitor::doTTLForIndex (this=this@entry=0x7f882e8cdfc0, opCtx=opCtx@entry=0x7f8832170180, idx=...) at src/mongo/db/ttl.cpp:263
#11 0x00007f882a76f5e0 in mongo::TTLMonitor::doTTLPass (this=this@entry=0x7f882e8cdfc0) at src/mongo/db/ttl.cpp:158
#12 0x00007f882a76fc08 in mongo::TTLMonitor::run (this=0x7f882e8cdfc0) at src/mongo/db/ttl.cpp:111
#13 0x00007f882bc3b221 in mongo::BackgroundJob::jobBody (this=0x7f882e8cdfc0) at src/mongo/util/background.cpp:150
*/
//FindCmd::run循环调用PlanExecutor的getNext函数获得查询结果.
//PlanExecutor::getNext中调用      执行查询计划  dlOut对应数据的key, objOut对应数据value
PlanExecutor::ExecState PlanExecutor::getNextImpl(Snapshotted<BSONObj>* objOut, RecordId* dlOut) {
    if (MONGO_FAIL_POINT(planExecutorAlwaysFails)) {
        Status status(ErrorCodes::OperationFailed,
                      str::stream() << "PlanExecutor hit planExecutorAlwaysFails fail point");
        *objOut =
            Snapshotted<BSONObj>(SnapshotId(), WorkingSetCommon::buildMemberStatusObject(status));

        return PlanExecutor::FAILURE;
    }

    invariant(_currentState == kUsable); 
    if (isMarkedAsKilled()) {
        if (NULL != objOut) {
            Status status(ErrorCodes::OperationFailed,
                          str::stream() << "Operation aborted because: " << *_killReason);
            *objOut = Snapshotted<BSONObj>(SnapshotId(),
                                           WorkingSetCommon::buildMemberStatusObject(status));
        }
        return PlanExecutor::DEAD;
    }

    if (!_stash.empty()) {
        invariant(objOut && !dlOut);
        *objOut = {SnapshotId(), _stash.front()};
        _stash.pop();
        return PlanExecutor::ADVANCED;
    }

    // When a stage requests a yield for document fetch, it gives us back a RecordFetcher*
    // to use to pull the record into memory. We take ownership of the RecordFetcher here,
    // deleting it after we've had a chance to do the fetch. For timing-based yields, we
    // just pass a NULL fetcher.
    unique_ptr<RecordFetcher> fetcher;

    // Incremented on every writeConflict, reset to 0 on any successful call to _root->work.
    size_t writeConflictsInARow = 0;

    // Capped insert data; declared outside the loop so we hold a shared pointer to the capped
    // insert notifier the entire time we are in the loop.  Holding a shared pointer to the capped
    // insert notifier is necessary for the notifierVersion to advance.
    CappedInsertNotifierData cappedInsertNotifierData;
    if (shouldWaitForInserts()) {
        cappedInsertNotifierData.notifier = getCappedInsertNotifier();
    }

	//这里面获取查下到的结果
    for (;;) {
        // These are the conditions which can cause us to yield:
        //   1) The yield policy's timer elapsed, or
        //   2) some stage requested a yield due to a document fetch, or
        //   3) we need to yield and retry due to a WriteConflictException.
        // In all cases, the actual yielding happens here.
        //判断是否需要让出CPU，例如检查kill  让出CPU给其他操作等
        if (_yieldPolicy->shouldYield()) {
            auto yieldStatus = _yieldPolicy->yield(fetcher.get());
            if (!yieldStatus.isOK()) {
                return swallowTimeoutIfAwaitData(yieldStatus, objOut);
            }
        }

        // We're done using the fetcher, so it should be freed. We don't want to
        // use the same RecordFetcher twice.
        fetcher.reset();

        WorkingSetID id = WorkingSet::INVALID_ID;
		//PlanStage::work
		//如果满足条件的索引有多个，一般执行MultiPlanStage::doWork
		//如果满足条件的索引只有一个，一般就是FetchStage::doWork
        PlanStage::StageState code = _root->work(&id); //PlanStage::work  执行查询计划

        if (code != PlanStage::NEED_YIELD)
            writeConflictsInARow = 0;

		//log() << "yang test PlanExecutor::getNextImpl:" << (int)code;
        if (PlanStage::ADVANCED == code) {//0
            WorkingSetMember* member = _workingSet->get(id);  //数据都存入WorkingSetMember这里面
            bool hasRequestedData = true;

            if (NULL != objOut) {
                if (WorkingSetMember::RID_AND_IDX == member->getState()) { //只需要获取索引信息
                    if (1 != member->keyData.size()) {
                        _workingSet->free(id);
                        hasRequestedData = false;
                    } else {
                        // TODO: currently snapshot ids are only associated with documents, and
                        // not with index keys.
                        *objOut = Snapshotted<BSONObj>(SnapshotId(), member->keyData[0].keyData);
                    }
                } else if (member->hasObj()) {
                    *objOut = member->obj;
                } else {
                    _workingSet->free(id);
                    hasRequestedData = false;
                }
            }

            if (NULL != dlOut) {
                if (member->hasRecordId()) {
                    *dlOut = member->recordId;
                } else {
                    _workingSet->free(id);
                    hasRequestedData = false;
                }
            }

            if (hasRequestedData) {
                _workingSet->free(id);
                return PlanExecutor::ADVANCED;
            }
            // This result didn't have the data the caller wanted, try again.
        } else if (PlanStage::NEED_YIELD == code) {
            if (id == WorkingSet::INVALID_ID) {
                if (!_yieldPolicy->canAutoYield())
                    throw WriteConflictException();
                CurOp::get(_opCtx)->debug().writeConflicts++;
                writeConflictsInARow++;
                WriteConflictException::logAndBackoff(
                    writeConflictsInARow, "plan execution", _nss.ns());

            } else {
                WorkingSetMember* member = _workingSet->get(id);
                invariant(member->hasFetcher());
                // Transfer ownership of the fetcher. Next time around the loop a yield will
                // happen.
                fetcher.reset(member->releaseFetcher());
            }

            // If we're allowed to, we will yield next time through the loop.
            if (_yieldPolicy->canAutoYield())
                _yieldPolicy->forceYield();
        } else if (PlanStage::NEED_TIME == code) { //NEED_TIME这里不退出，继续循环执行
            // Fall through to yield check at end of large conditional.
        } else if (PlanStage::IS_EOF == code) {
            if (!shouldWaitForInserts()) {
                return PlanExecutor::IS_EOF;
            }
            const ExecState waitResult = waitForInserts(&cappedInsertNotifierData, objOut);
            if (waitResult == PlanExecutor::ADVANCED) {
                // There may be more results, keep going.
                continue;
            }
            return waitResult;
        } else {
            invariant(PlanStage::DEAD == code || PlanStage::FAILURE == code);

            if (NULL != objOut) {
                BSONObj statusObj;
                WorkingSetCommon::getStatusMemberObject(*_workingSet, id, &statusObj);
                *objOut = Snapshotted<BSONObj>(SnapshotId(), statusObj);
            }

            return (PlanStage::DEAD == code) ? PlanExecutor::DEAD : PlanExecutor::FAILURE;
        }
    }
}

bool PlanExecutor::isEOF() {
    invariant(_currentState == kUsable);
    return isMarkedAsKilled() || (_stash.empty() && _root->isEOF());
}

void PlanExecutor::markAsKilled(string reason) {
    _killReason = std::move(reason);
}

void PlanExecutor::dispose(OperationContext* opCtx, CursorManager* cursorManager) {
    if (_currentState == kDisposed) {
        return;
    }

    // If we are registered with the CursorManager we need to be sure to deregister ourselves.
    // However, if we have been killed we should not attempt to deregister ourselves, since the
    // caller of markAsKilled() will have done that already, and the CursorManager may no longer
    // exist. Note that the caller's collection lock prevents us from being marked as killed during
    // this method, since any interruption event requires a lock in at least MODE_IX.
    if (cursorManager && _registrationToken && !isMarkedAsKilled()) {
        dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IS));
        cursorManager->deregisterExecutor(this);
    }
    _root->dispose(opCtx);
    _currentState = kDisposed;
}

Status PlanExecutor::executePlan() {
    invariant(_currentState == kUsable);
    BSONObj obj;
    PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
    while (PlanExecutor::ADVANCED == state) {
        state = this->getNext(&obj, NULL);
    }

    if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
        if (isMarkedAsKilled()) {
            return Status(ErrorCodes::QueryPlanKilled,
                          str::stream() << "Operation aborted because: " << *_killReason);
        }

        auto errorStatus = WorkingSetCommon::getMemberObjectStatus(obj);
        invariant(!errorStatus.isOK());
        return errorStatus.withContext(str::stream() << "Exec error resulting in state "
                                                     << PlanExecutor::statestr(state));
    }

    invariant(!isMarkedAsKilled());
    invariant(PlanExecutor::IS_EOF == state);
    return Status::OK();
}

//FindCmd::run中入队
void PlanExecutor::enqueue(const BSONObj& obj) {
    _stash.push(obj.getOwned());
}

PlanExecutor::ExecState PlanExecutor::swallowTimeoutIfAwaitData(
    Status yieldError, Snapshotted<BSONObj>* errorObj) const {
    if (yieldError == ErrorCodes::ExceededTimeLimit) {
        if (_cq && _cq->getQueryRequest().isTailableAndAwaitData()) {
            // If the cursor is tailable then exceeding the time limit should not destroy this
            // PlanExecutor, we should just stop waiting for inserts.
            return PlanExecutor::IS_EOF;
        }
    }

    if (errorObj) {
        *errorObj = Snapshotted<BSONObj>(SnapshotId(),
                                         WorkingSetCommon::buildMemberStatusObject(yieldError));
    }
    return PlanExecutor::DEAD;
}

Timestamp PlanExecutor::getLatestOplogTimestamp() {
    if (auto pipelineProxy = getStageByType(_root.get(), STAGE_PIPELINE_PROXY))
        return static_cast<PipelineProxyStage*>(pipelineProxy)->getLatestOplogTimestamp();
    if (auto collectionScan = getStageByType(_root.get(), STAGE_COLLSCAN))
        return static_cast<CollectionScan*>(collectionScan)->getLatestOplogTimestamp();
    return Timestamp();
}

//
// PlanExecutor::Deleter
//

PlanExecutor::Deleter::Deleter(OperationContext* opCtx, const Collection* collection)
    : _opCtx(opCtx), _cursorManager(collection ? collection->getCursorManager() : nullptr) {}

void PlanExecutor::Deleter::operator()(PlanExecutor* execPtr) {
    try {
        invariant(_opCtx);  // It is illegal to invoke operator() on a default constructed Deleter.
        if (!_dismissed) {
            execPtr->dispose(_opCtx, _cursorManager);
        }
        delete execPtr;
    } catch (...) {
        std::terminate();
    }
}

}  // namespace mongo
