/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/lock_state.h"

#include <vector>

#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/new.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

/**
 * Partitioned global lock statistics, so we don't hit the same bucket.
 */ 

//注意db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) 三个的区别

/*  LockStats<>::_report 中获取相关信息
featdoc:PRIMARY> 
featdoc:PRIMARY> db.serverStatus().locks
{
        "Global" : {
                "acquireCount" : {
                        "r" : NumberLong(1447),
                        "w" : NumberLong(40),
                        "W" : NumberLong(9)
                },
                "acquireWaitCount" : {
                        "w" : NumberLong(1),
                        "W" : NumberLong(2)
                },
                "timeAcquiringMicros" : {
                        "w" : NumberLong(8569),
                        "W" : NumberLong(268)
                }
        },
        "Database" : {
                "acquireCount" : {
                        "r" : NumberLong(689),
                        "w" : NumberLong(18),
                        "R" : NumberLong(7),
                        "W" : NumberLong(16)
                }
        },
        "Collection" : {
                "acquireCount" : {
                        "r" : NumberLong(358),
                        "w" : NumberLong(8)
                }
        },
        "oplog" : {
                "acquireCount" : {
                        "r" : NumberLong(331),
                        "w" : NumberLong(12)
                }
        }
}
featdoc:PRIMARY> db.serverStatus().globalLock
{
        "totalTime" : NumberLong(170653000),
        "currentQueue" : {
                "total" : 0,
                "readers" : 0,
                "writers" : 0
        },
        "activeClients" : {
                "total" : 29,
                "readers" : 0,
                "writers" : 0
        }
}
featdoc:PRIMARY> 
featdoc:PRIMARY> 
*/

//注意db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) 三个的区别

//单次请求对应线程的锁统计在LockerImpl._stats中存储，全局锁统计在全局变量globalStats中存储

//PartitionedInstanceWideLockStats globalStats;    全局锁统计
//LockStats<>::_report(db.serverStatus().locks查看)中获取相关信息,这里面是总的锁相关的统计
class PartitionedInstanceWideLockStats {
    MONGO_DISALLOW_COPYING(PartitionedInstanceWideLockStats);

public:
    PartitionedInstanceWideLockStats() {}

	//LockerImpl<>::lockBegin->PartitionedInstanceWideLockStats::recordAcquisition->LockStats::recordAcquisition
	//LockerImpl<IsForMMAPV1>::lockBegin中执行
    void recordAcquisition(LockerId id, ResourceId resId, LockMode mode) {
        _get(id).recordAcquisition(resId, mode);
    }

	
	//LockerImpl<>::lockBegin->PartitionedInstanceWideLockStats::recordWait->LockStats::recordWait
	//LockerImpl<>::lockBegin中执行
    void recordWait(LockerId id, ResourceId resId, LockMode mode) {
        _get(id).recordWait(resId, mode);
    }

	//LockerImpl<>::lockComplete->PartitionedInstanceWideLockStats::recordWaitTime->LockStats::recordWaitTime
    void recordWaitTime(LockerId id, ResourceId resId, LockMode mode, uint64_t waitMicros) {
        _get(id).recordWaitTime(resId, mode, waitMicros);
    }

	//LockerImpl<>::lockComplete->PartitionedInstanceWideLockStats::recordDeadlock->LockStats::recordDeadlock
    void recordDeadlock(ResourceId resId, LockMode mode) {
        _get(resId).recordDeadlock(resId, mode);
    }

	//全局监控打印db.serverstats().locks  reportGlobalLockingStats->PartitionedInstanceWideLockStats::report
	//用户请求线程，例如写一条数据对应线程，会记录慢日志，OpDebug::report中打印到慢日志中
	void report(SingleThreadedLockStats* outStats) const {
        for (int i = 0; i < NumPartitions; i++) {
			//LockStats::append
            outStats->append(_partitions[i].stats);
        }
    }

    void reset() {
        for (int i = 0; i < NumPartitions; i++) {
            _partitions[i].stats.reset();
        }
    }

private:
    // This alignment is a best effort approach to ensure that each partition falls on a
    // separate page/cache line in order to avoid false sharing.
    //c++ alignas改变一个数据类型的对齐属性
    struct alignas(stdx::hardware_destructive_interference_size) AlignedLockStats {
        AtomicLockStats stats;
    };

	//AlignedLockStats _partitions[NumPartitions];
    enum { NumPartitions = 8 };

	//前面的recordAcquisition等接口会用到该类
    AtomicLockStats& _get(LockerId id) {
        return _partitions[id % NumPartitions].stats;
    }

	//也就是AtomicLockStats结构 
    AlignedLockStats _partitions[NumPartitions];
};

// Global lock manager instance.
LockManager globalLockManager;

// Global lock. Every server operation, which uses the Locker must acquire this lock at least
// once. See comments in the header file (begin/endTransaction) for more information.
const ResourceId resourceIdGlobal = ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);

// Flush lock. This is only used for the MMAP V1 storage engine and synchronizes journal writes
// to the shared view and remaps. See the comments in the header for information on how MMAP V1
// concurrency control works.
const ResourceId resourceIdMMAPV1Flush =
    ResourceId(RESOURCE_MMAPV1_FLUSH, ResourceId::SINGLETON_MMAPV1_FLUSH);

// How often (in millis) to check for deadlock if a lock has not been granted for some time
//超过这么多时间都没有获取到锁，则判断为死锁
const Milliseconds DeadlockTimeout = Milliseconds(500);

// Dispenses unique LockerId identifiers
AtomicUInt64 idCounter(0);

// Partitioned global lock statistics, so we don't hit the same bucket

//单次请求对应线程的锁统计在LockerImpl._stats中存储，全局锁统计在全局变量globalStats中存储
PartitionedInstanceWideLockStats globalStats; //全局lock统计


/**
 * Whether the particular lock's release should be held until the end of the operation. We
 * delay release of exclusive locks (locks that are for write operations) in order to ensure
 * that the data they protect is committed successfully.
 */
//LockerImpl<>::unlock中调用
bool shouldDelayUnlock(ResourceId resId, LockMode mode) {
    // Global and flush lock are not used to protect transactional resources and as such, they
    // need to be acquired and released when requested.
    //全局锁和刷新锁不用于保护事务资源，因此需要在请求时获取和释放这些资源。
    switch (resId.getType()) {
        case RESOURCE_GLOBAL:
        case RESOURCE_MMAPV1_FLUSH:
        case RESOURCE_MUTEX:
            return false;

        case RESOURCE_COLLECTION:
        case RESOURCE_DATABASE:
        case RESOURCE_METADATA:
            break;

        default:
            MONGO_UNREACHABLE;
    }

    switch (mode) {
		//写锁不应被释放，这样可以确保写事务正确提交
        case MODE_X:
        case MODE_IX:
            return true;

        case MODE_IS:
        case MODE_S:
            return false;

        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace

//写锁   //LockerImpl结构是OperationContext._locker成员, wiredtiger对应DefaultLockerImpl
template <bool IsForMMAPV1>
//当前resourceIdGlobal加的是MODE_X类型的锁
bool LockerImpl<IsForMMAPV1>::isW() const {
    return getLockMode(resourceIdGlobal) == MODE_X;
}

//读锁
template <bool IsForMMAPV1>
//当前resourceIdGlobal加的是MODE_S类型的锁
bool LockerImpl<IsForMMAPV1>::isR() const {
    return getLockMode(resourceIdGlobal) == MODE_S;
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isLocked() const {
    return getLockMode(resourceIdGlobal) != MODE_NONE;
}

template <bool IsForMMAPV1>
//当前resourceIdGlobal加的是MODE_IX类型的锁  写意向锁
bool LockerImpl<IsForMMAPV1>::isWriteLocked() const {
    return isLockHeldForMode(resourceIdGlobal, MODE_IX);
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isReadLocked() const {
//当前resourceIdGlobal加的是MODE_IS类型的锁   读意向锁
    return isLockHeldForMode(resourceIdGlobal, MODE_IS);
}

template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::dump() const {
    StringBuilder ss;
    ss << "Locker id " << _id << " status: ";

    _lock.lock();
    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        ss << it.key().toString() << " " << lockRequestStatusName(it->status) << " in "
           << modeName(it->mode) << "; ";
        it.next();
    }
    _lock.unlock();

    log() << ss.str();
}


//
// CondVarLockGrantNotification
//

CondVarLockGrantNotification::CondVarLockGrantNotification() {
    clear();
}

void CondVarLockGrantNotification::clear() {
    _result = LOCK_INVALID;
}

//条件变量等待唤醒或者超时自动唤醒返回     LockerImpl<>::lockComplete
LockResult CondVarLockGrantNotification::wait(Milliseconds timeout) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _cond.wait_for(
               lock, timeout.toSystemDuration(), [this] { return _result != LOCK_INVALID; })
        ? _result
        : LOCK_TIMEOUT;
}

//LockManager::_onLockModeChanged->CondVarLockGrantNotification::notify唤醒等待线程
void CondVarLockGrantNotification::notify(ResourceId resId, LockResult result) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    invariant(_result == LOCK_INVALID);
    _result = result;

	//条件变量，唤醒所有的等待(wait)线程。
    _cond.notify_all();
}


//ServiceContextMongoD::_newOpCtx中构造LockerImpl使用，每个请求对应一个DefaultLockerImpl(也就是一个LockerImpl)
//注意全局锁并发控制实际由全局变量ticketHolders[]负责维护，对每个请求的locker处理前需要判断全局锁是否需要等待



//注意db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) 三个的区别


//每一种mode对应一个TicketHolder，依赖linux的sem信号量实现
//通过db.serverStatus().globalLock获取
namespace { //赋值见setGlobalThrottling //WiredTigerKVEngine::WiredTigerKVEngine->Locker::setGlobalThrottling
TicketHolder* ticketHolders[LockModesCount] = {}; 
}  // namespace


//
// Locker
//
/**
 * Lock modes.
 *
 * Compatibility Matrix  相容性关系 +相容共存        +是兼容的   
 *                                          Granted mode
 *   ---------------.--------------------------------------------------------.
 *   Requested Mode | MODE_NONE  MODE_IS   MODE_IX  MODE_S   MODE_X  |
 *     MODE_IS      |      +        +         +        +        -    |
 *     MODE_IX      |      +        +         +        -        -    |
 *     MODE_S       |      +        +         -        +        -    |
 *     MODE_X       |      +        -         -        -        -    |  加了MODE_X锁后，读写都不相容
 * 官方文档https://docs.mongodb.com/manual/faq/concurrency/
 */

//TicketHolder openWriteTransaction(128); 
//TicketHolder openReadTransaction(128);

//WiredTigerKVEngine::WiredTigerKVEngine->Locker::setGlobalThrottling
/* static */  //读写锁默认由128的信号量实现，获取锁信号量-1，释放锁信号量加1
void Locker::setGlobalThrottling(class TicketHolder* reading, class TicketHolder* writing) {
    ticketHolders[MODE_S] = reading;
    ticketHolders[MODE_IS] = reading;
    ticketHolders[MODE_IX] = writing;

	//ticketHolders[MODE_X]为什么没赋值呢，在哪里赋值呢   从_lockGlobalBegin配合阅读
}

template <bool IsForMMAPV1>
LockerImpl<IsForMMAPV1>::LockerImpl()
    : _id(idCounter.addAndFetch(1)), _wuowNestingLevel(0), _threadId(stdx::this_thread::get_id()) {}

template <bool IsForMMAPV1>
stdx::thread::id LockerImpl<IsForMMAPV1>::getThreadId() const {
    return _threadId;
}

template <bool IsForMMAPV1>
LockerImpl<IsForMMAPV1>::~LockerImpl() {
    // Cannot delete the Locker while there are still outstanding requests, because the
    // LockManager may attempt to access deleted memory. Besides it is probably incorrect
    // to delete with unaccounted locks anyways.
    invariant(!inAWriteUnitOfWork());
    invariant(_resourcesToUnlockAtEndOfUnitOfWork.empty());
    invariant(_requests.empty());
    invariant(_modeForTicket == MODE_NONE);

    // Reset the locking statistics so the object can be reused
    _stats.reset();
}

//获取 Client 状态时，已经获取到wiredtiger ticket 的 Reader/Writer 如果在等锁，也会认为是 Queued 状态，这个之前忽略了。
//http://www.mongoing.com/archives/4768
//db.serverStatus().globalLock获取，
//注意db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) 三个的区别

////GlobalLockServerStatusSection::generateSection中调用
template <bool IsForMMAPV1>
Locker::ClientState LockerImpl<IsForMMAPV1>::getClientState() const {
    auto state = _clientState.load();
    if (state == kActiveReader && hasLockPending())
        state = kQueuedReader;
    if (state == kActiveWriter && hasLockPending())
        state = kQueuedWriter;

    return state;
}

//LockerImpl<>::restoreLockState
//全局锁获取实际上不是走的这里，而是Lock::GlobalLock::_enqueue->lock_state.h中的lockGlobalBegin->LockerImpl<>::_lockGlobalBegin


//全局锁加锁过程：Lock::GlobalLock::GlobalLock->Lock::GlobalLock::_enqueue->LockerImpl::lockGlobal
template <bool IsForMMAPV1>
LockResult LockerImpl<IsForMMAPV1>::lockGlobal(LockMode mode) {
    LockResult result = _lockGlobalBegin(mode, Milliseconds::max());

    if (result == LOCK_WAITING) { //一直等待，获取到锁或者超时才返回
        result = lockGlobalComplete(Milliseconds::max());
    }

    if (result == LOCK_OK) {
        lockMMAPV1Flush();
    }

    return result;
}
/*
db/concurrency/lock_state.cpp:const ResourceId resourceIdGlobal = ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
db/concurrency/lock_state.cpp:const ResourceId resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, StringData("local"));
db/concurrency/lock_state.cpp:const ResourceId resourceIdOplog = ResourceId(RESOURCE_COLLECTION, StringData("local.oplog.rs"));
db/concurrency/lock_state.cpp:const ResourceId resourceIdAdminDB = ResourceId(RESOURCE_DATABASE, StringData("admin"));
*/

//LockerImpl<IsForMMAPV1>::_lockGlobalBegin和Lock::GlobalLock::_unlock对应，一个上锁，一个解锁

//serverStatus.globalLock 或者 mongostat （qr|qw ar|aw指标）能查看mongod globalLock的各个指标情况。

//全局锁上锁过程LockerImpl<>::_lockGlobalBegin   
//RESOURCE_DATABASE RESOURCE_COLLECTION对应的上锁过程见LockResult LockerImpl<>::lock

//全局锁加锁过程：Lock::GlobalLock::GlobalLock->Lock::GlobalLock::_enqueue->LockerImpl::lockGlobal

//LockerImpl<>::lockGlobal  Lock::GlobalLock::_enqueue中调用  
template <bool IsForMMAPV1>  //等待获取全局锁成功见Lock::GlobalLock::GlobalLock->Lock::GlobalLock::waitForLock->LockerImpl<>::lockGlobalComplete
LockResult LockerImpl<IsForMMAPV1>::_lockGlobalBegin(LockMode mode, Milliseconds timeout) {
    dassert(isLocked() == (_modeForTicket != MODE_NONE));

	//全局锁队列并发控制在这里，这是实例级别的锁
    if (_modeForTicket == MODE_NONE) {
		//判断是否读锁或者读意向锁
        const bool reader = isSharedLockMode(mode);
		//该mode对应的ticketHolders，这是实例级别的锁
        auto holder = ticketHolders[mode]; 
		//该循环中等锁
        if (holder) { //如果mode为MODE_X， 这里ticketHolders[MODE_X]为NULL，见setGlobalThrottling
		//这里体现了S  IS  IX三种锁每种锁都需要受全局128信号量并发控制，也就是最多只有128个线程同时操作
		//这几种类型的锁
		
            _clientState.store(reader ? kQueuedReader : kQueuedWriter); 
			//等待锁期间为Queued状态，获取到锁后变为Active状态，获取超时变为inactive
            if (timeout == Milliseconds::max()) {
				//TicketHolder::waitForTicket一直等有信号锁锁可用
                holder->waitForTicket();  


			//最大等待锁时间，超过这个时间直接进入inactive
            } else if (!holder->waitForTicketUntil(Date_t::now() + timeout)) {
            	//没获取到锁，也就是信号量用完了，状态变为inactive
                _clientState.store(kInactive); 
				//获取锁超时
                return LOCK_TIMEOUT;
            }
        }

		//获取到锁后状态变为active, 通过db.serverStatus().globalLock获取
		//注意db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) 三个的区别
        _clientState.store(reader ? kActiveReader : kActiveWriter);
        _modeForTicket = mode;
    }

	//下面是意向锁相关
	//注意这里全都用的同一个资源resourceIdGlobal
    const LockResult result = lockBegin(resourceIdGlobal, mode);
    if (result == LOCK_OK)
        return LOCK_OK;

    // Currently, deadlock detection does not happen inline with lock acquisition so the only
    // unsuccessful result that the lock manager would return is LOCK_WAITING.
    invariant(result == LOCK_WAITING);

    return result;
}

//LockerImpl<>::lockGlobal    Lock::GlobalLock::GlobalLock->Lock::GlobalLock::waitForLock
template <bool IsForMMAPV1>
LockResult LockerImpl<IsForMMAPV1>::lockGlobalComplete(Milliseconds timeout) {
    return lockComplete(resourceIdGlobal, getLockMode(resourceIdGlobal), timeout, false);
}

template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::lockMMAPV1Flush() {
    if (!IsForMMAPV1)
        return;

    // The flush lock always has a reference count of 1, because it is dropped at the end of
    // each write unit of work in order to allow the flush thread to run. See the comments in
    // the header for information on how the MMAP V1 journaling system works.
    LockRequest* globalLockRequest = _requests.find(resourceIdGlobal).objAddr();
    if (globalLockRequest->recursiveCount == 1) {
        invariant(LOCK_OK == lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
    }

    dassert(getLockMode(resourceIdMMAPV1Flush) == _getModeForMMAPV1FlushLock());
}

template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::downgradeGlobalXtoSForMMAPV1() {
    invariant(!inAWriteUnitOfWork());

    LockRequest* globalLockRequest = _requests.find(resourceIdGlobal).objAddr();
    invariant(globalLockRequest->mode == MODE_X);
    invariant(globalLockRequest->recursiveCount == 1);
    invariant(_modeForTicket == MODE_X);
    // Note that this locker will not actually have a ticket (as MODE_X has no TicketHolder) or
    // acquire one now, but at most a single thread can be in this downgraded MODE_S situation,
    // so it's OK.

    // Making this call here will record lock downgrades as acquisitions, which is acceptable
    globalStats.recordAcquisition(_id, resourceIdGlobal, MODE_S);
    _stats.recordAcquisition(resourceIdGlobal, MODE_S);

    globalLockManager.downgrade(globalLockRequest, MODE_S);

    if (IsForMMAPV1) {
        invariant(unlock(resourceIdMMAPV1Flush));
    }
}

//LockerImpl<IsForMMAPV1>::_lockGlobalBegin和Lock::GlobalLock::_unlock对应，一个上锁，一个解锁
//Lock::GlobalLock::_unlock
template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::unlockGlobal() {
    if (!unlock(resourceIdGlobal)) {
        return false;
    }

    invariant(!inAWriteUnitOfWork());

    LockRequestsMap::Iterator it = _requests.begin();
    while (!it.finished()) {
        // If we're here we should only have one reference to any lock. It is a programming
        // error for any lock used with multi-granularity locking to have more references than
        // the global lock, because every scope starts by calling lockGlobal.
        if (it.key().getType() == RESOURCE_GLOBAL || it.key().getType() == RESOURCE_MUTEX) {
            it.next();
        } else {
            invariant(_unlockImpl(&it));
        }
    }

    return true;
}

//配合参考insertDocuments  makeCollection    事务封装
//WriteUnitOfWork中调用
template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::beginWriteUnitOfWork() {
    // Sanity check that write transactions under MMAP V1 have acquired the flush lock, so we
    // don't allow partial changes to be written.
    dassert(!IsForMMAPV1 || isLockHeldForMode(resourceIdMMAPV1Flush, MODE_IX));

    _wuowNestingLevel++;
}

//配合参考insertDocuments  makeCollection    事务封装
//~WriteUnitOfWork()  WriteUnitOfWork::commit调用
template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::endWriteUnitOfWork() {
    invariant(_wuowNestingLevel > 0);

    if (--_wuowNestingLevel > 0) {
        // Don't do anything unless leaving outermost WUOW.
        return;
    }
	
    while (!_resourcesToUnlockAtEndOfUnitOfWork.empty()) {
        unlock(_resourcesToUnlockAtEndOfUnitOfWork.front());
        _resourcesToUnlockAtEndOfUnitOfWork.pop();
    }

    // For MMAP V1, we need to yield the flush lock so that the flush thread can run
    if (IsForMMAPV1) {
        invariant(unlock(resourceIdMMAPV1Flush));
        invariant(LOCK_OK == lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
    }
}

//全局锁上锁过程LockerImpl<IsForMMAPV1>::_lockGlobalBegin   
//RESOURCE_DATABASE RESOURCE_COLLECTION对应的上锁过程见LockResult LockerImpl<>::lock

//LockerImpl<>::lock和LockerImpl<>::unlock对应

//Lock::ResourceLock::lock
//Lock::DBLock::DBLock  Lock::DBLock::relockWithMode
//Lock::CollectionLock::CollectionLock
//Lock::OplogIntentWriteLock::OplogIntentWriteLock

//LockerImpl<>::lockComplete
//LockerImpl<>::restoreLockState中可能引起递归调用
template <bool IsForMMAPV1>
LockResult LockerImpl<IsForMMAPV1>::lock(ResourceId resId,
                                         LockMode mode,
                                         //Locker类中默认赋值为Milliseconds::max()，不超时
                                         Milliseconds timeout,
                                         bool checkDeadlock) {
    const LockResult result = lockBegin(resId, mode);

    // Fast, uncontended path
    if (result == LOCK_OK)
        return LOCK_OK;

    // Currently, deadlock detection does not happen inline with lock acquisition so the only
    // unsuccessful result that the lock manager would return is LOCK_WAITING.
    invariant(result == LOCK_WAITING);

	//一直等待获取到锁或者超时才返回
    return lockComplete(resId, mode, timeout, checkDeadlock);
}

template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::downgrade(ResourceId resId, LockMode newMode) {
    LockRequestsMap::Iterator it = _requests.find(resId);
    globalLockManager.downgrade(it.objAddr(), newMode);
}

//LockerImpl<>::lock和LockerImpl<>::unlock对应
template <bool IsForMMAPV1>
//注意LockerImpl<>::unlock 和 LockerImpl<>::_unlockImpl的区别
//	LockerImpl<>::unlock相比_unlockImpl增加了锁释放前的事务处理判断，判断释放需要延迟等到事务
//	提交后释放，确认事务提交完成后释放锁，从而起一个保护作用
bool LockerImpl<IsForMMAPV1>::unlock(ResourceId resId) {
    LockRequestsMap::Iterator it = _requests.find(resId);
	//Lock_state.h中_wuowNestingLevel>0说明在事务处理中，这时候需要进一步确定是否需要延迟解锁
	//如果当前请求在事务处理中并且资源类型为库锁、表锁，并且对应的锁类型为X或者IX，
	//例如insert场景 insertDocuments中事务写，这类资源信息的unlock需要延迟到LockerImpl<>::endWriteUnitOfWork()
	// 事务提交后进行锁资源unlock释放
	if (inAWriteUnitOfWork() && shouldDelayUnlock(it.key(), (it->mode))) {
		//需要延迟unlock的锁添加到_resourcesToUnlockAtEndOfUnitOfWork队列
        _resourcesToUnlockAtEndOfUnitOfWork.push(it.key());
        return false;
    }

	
    return _unlockImpl(&it);
}

template <bool IsForMMAPV1>
LockMode LockerImpl<IsForMMAPV1>::getLockMode(ResourceId resId) const {
    scoped_spinlock scopedLock(_lock);

    const LockRequestsMap::ConstIterator it = _requests.find(resId);
    if (!it)
        return MODE_NONE;

    return it->mode;
}

template <bool IsForMMAPV1>

//也就是LockConflictsTable的getLockMode(resId)是否包含mode
//前者resId.mode是否包含后者mode
bool LockerImpl<IsForMMAPV1>::isLockHeldForMode(ResourceId resId, LockMode mode) const {
    return isModeCovered(mode, getLockMode(resId));
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isDbLockedForMode(StringData dbName, LockMode mode) const {
    invariant(nsIsDbOnly(dbName));

    if (isW())
        return true;
    if (isR() && isSharedLockMode(mode))
        return true;

    const ResourceId resIdDb(RESOURCE_DATABASE, dbName);
    return isLockHeldForMode(resIdDb, mode);
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isCollectionLockedForMode(StringData ns, LockMode mode) const {
    invariant(nsIsFull(ns));

    if (isW())
        return true;
    if (isR() && isSharedLockMode(mode))
        return true;

    const NamespaceString nss(ns);
    const ResourceId resIdDb(RESOURCE_DATABASE, nss.db());

    LockMode dbMode = getLockMode(resIdDb);
    if (!shouldConflictWithSecondaryBatchApplication())
        return true;

    switch (dbMode) {
        case MODE_NONE:
            return false;
        case MODE_X:
            return true;
        case MODE_S:
            return isSharedLockMode(mode);
        case MODE_IX:
        case MODE_IS: {
            const ResourceId resIdColl(RESOURCE_COLLECTION, ns);
            return isLockHeldForMode(resIdColl, mode);
        } break;
        case LockModesCount:
            break;
    }

    invariant(false);
    return false;
}

template <bool IsForMMAPV1>
//慢日志打印的时候调用LockerImpl<>::getLockerInfo，然后调用该接口
ResourceId LockerImpl<IsForMMAPV1>::getWaitingResource() const {
    scoped_spinlock scopedLock(_lock);

    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        if (it->status == LockRequest::STATUS_WAITING ||
            it->status == LockRequest::STATUS_CONVERTING) {
            return it.key();
        }

        it.next();
    }

    return ResourceId();
}

//慢日志记录参考ServiceEntryPointMongod::handleRequest   OpDebug::report中使用该lockerInfo打印到日志
template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::getLockerInfo(LockerInfo* lockerInfo) const {
    invariant(lockerInfo);

	//赋初值
    // Zero-out the contents
    lockerInfo->locks.clear();
    lockerInfo->waitingResource = ResourceId();
	//LockStatCounters::reset
    lockerInfo->stats.reset();

	//获取当前的实时统计信息
    _lock.lock();
    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        OneLock info;
        info.resourceId = it.key();
        info.mode = it->mode;

        lockerInfo->locks.push_back(info);
        it.next();
    }
    _lock.unlock();

    std::sort(lockerInfo->locks.begin(), lockerInfo->locks.end());

	// OpDebug::report中使用该lockerInfo打印到日志
    lockerInfo->waitingResource = getWaitingResource();
	//LockStats::append
    lockerInfo->stats.append(_stats);
}

template <bool IsForMMAPV1>
//QueryYield::yieldAllLocks  Lock::TempRelease::TempRelease调用
bool LockerImpl<IsForMMAPV1>::saveLockStateAndUnlock(Locker::LockSnapshot* stateOut) {
    // We shouldn't be saving and restoring lock state from inside a WriteUnitOfWork.
    invariant(!inAWriteUnitOfWork());

    // Clear out whatever is in stateOut.
    stateOut->locks.clear();
    stateOut->globalMode = MODE_NONE;

    // First, we look at the global lock.  There is special handling for this (as the flush
    // lock goes along with it) so we store it separately from the more pedestrian locks.
    LockRequestsMap::Iterator globalRequest = _requests.find(resourceIdGlobal);
    if (!globalRequest) {
        // If there's no global lock there isn't really anything to do. Check that.
        for (auto it = _requests.begin(); !it.finished(); it.next()) {
            invariant(it.key().getType() == RESOURCE_MUTEX);
        }
        return false;
    }

    // If the global lock has been acquired more than once, we're probably somewhere in a
    // DBDirectClient call.  It's not safe to release and reacquire locks -- the context using
    // the DBDirectClient is probably not prepared for lock release.
    if (globalRequest->recursiveCount > 1) {
        return false;
    }

    // The global lock must have been acquired just once
    stateOut->globalMode = globalRequest->mode;
    invariant(unlock(resourceIdGlobal));

    // Next, the non-global locks.
    for (LockRequestsMap::Iterator it = _requests.begin(); !it.finished(); it.next()) {
        const ResourceId resId = it.key();
        const ResourceType resType = resId.getType();
        if (resType == RESOURCE_MUTEX)
            continue;

        // We should never have to save and restore metadata locks.
        invariant((IsForMMAPV1 && (resourceIdMMAPV1Flush == resId)) ||
                  RESOURCE_DATABASE == resId.getType() || RESOURCE_COLLECTION == resId.getType() ||
                  (RESOURCE_GLOBAL == resId.getType() && isSharedLockMode(it->mode)));

        // And, stuff the info into the out parameter.
        OneLock info;
        info.resourceId = resId;
        info.mode = it->mode;

        stateOut->locks.push_back(info);

        invariant(unlock(resId));
    }
    invariant(!isLocked());

    // Sort locks by ResourceId. They'll later be acquired in this canonical locking order.
    std::sort(stateOut->locks.begin(), stateOut->locks.end());

    return true;
}

//Lock::TempRelease::~TempRelease    WiredTigerRecordStore::yieldAndAwaitOplogDeletionRequest
//QueryYield::yieldAllLocks   handleBatchHelper 
template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::restoreLockState(const Locker::LockSnapshot& state) {
    // We shouldn't be saving and restoring lock state from inside a WriteUnitOfWork.
    invariant(!inAWriteUnitOfWork());
    invariant(_modeForTicket == MODE_NONE);

    std::vector<OneLock>::const_iterator it = state.locks.begin();
    // If we locked the PBWM, it must be locked before the resourceIdGlobal resource.
    if (it != state.locks.end() && it->resourceId == resourceIdParallelBatchWriterMode) {
        invariant(LOCK_OK == lock(it->resourceId, it->mode));
        it++;
    }

    invariant(LOCK_OK == lockGlobal(state.globalMode));
    for (; it != state.locks.end(); it++) {
        // This is a sanity check that lockGlobal restored the MMAP V1 flush lock in the
        // expected mode.
        if (IsForMMAPV1 && (it->resourceId == resourceIdMMAPV1Flush)) {
            invariant(it->mode == _getModeForMMAPV1FlushLock());
        } else {
            invariant(LOCK_OK == lock(it->resourceId, it->mode));
        }
    }
    invariant(_modeForTicket != MODE_NONE);
}

//全局锁上锁过程LockerImpl<>::_lockGlobalBegin   
//RESOURCE_DATABASE RESOURCE_COLLECTION对应的上锁过程见LockResult LockerImpl<>::lock

//全局锁_lockGlobalBegin     库锁 表锁 LockResult LockerImpl<>::lock 都会执行该函数
template <bool IsForMMAPV1>  ////wiredtiger存储引擎LockerImpl对应DefaultLockerImpl
LockResult LockerImpl<IsForMMAPV1>::lockBegin(ResourceId resId, LockMode mode) {
    dassert(!getWaitingResource().isValid());

    LockRequest* request;
    bool isNew = true; //说明request是最新构造的，false说明是之前_requests已经存在的

	//log() << "yang test lockBegin, lockinfo: " << resId.toString();
	
//#include "mongo/util/stacktrace.h"
	//mallocFreeOStream << "lock test :" << ".\n";
    //printStackTrace(mallocFreeOStream);


    LockRequestsMap::Iterator it = _requests.find(resId); 
    if (!it) { //如果resId不在_requests map表中，则生成一个LockRequest添加进去
        scoped_spinlock scopedLock(_lock);
		//每个resId锁资源对应生成一个LockRequest，添加到_requests中
        LockRequestsMap::Iterator itNew = _requests.insert(resId);
		
		//初始化一个struct LockRequest结构	 一个ResourceId对应一个LockRequest类，LockRequest类有个链表结构可以让所有locker链接起来
        itNew->initNew(this, &_notify); //LockRequest::initNew
		
		//获取LockRequest 上面的LockRequest::initNew生成
        request = itNew.objAddr();//FastMapNoAlloc::IteratorImpl::objAddr
    } else { //resId已经存在于_requests，则获取该resId对应的LockRequest
    	//一般表示获取锁超时，然后递归重新获取，配合LockerImpl<>::lockComplete阅读
    	//只有MMAP引擎才会走到这个流程?
        request = it.objAddr();
        isNew = false;
    }

    // Making this call here will record lock re-acquisitions and conversions as well.
    //_id为标识本LockerImpl类的唯一ID
    //_id标识为LockerImpl的类，其记录的mode类型锁ResourceId resId, LockMode mode对应的计数增加
	//全局计数
	//PartitionedInstanceWideLockStats::recordAcquisition;
	globalStats.recordAcquisition(_id, resId, mode); 
	//LockStats::recordAcquisition;  
    _stats.recordAcquisition(resId, mode); //对本LockerImpl._stats做统计

    // Give priority to the full modes for global, parallel batch writer mode,
    // and flush lock so we don't stall global operations such as shutdown or flush.
    const ResourceType resType = resId.getType();
	//全局资源类型并且锁类型为非意向锁(MODE_S  MODE_X)则compatibleFirst为true
    if (resType == RESOURCE_GLOBAL || (IsForMMAPV1 && resId == resourceIdMMAPV1Flush)) {
        if (mode == MODE_S || mode == MODE_X) {
            request->enqueueAtFront = true;
            request->compatibleFirst = true;
        }
    } else if (resType != RESOURCE_MUTEX) { //库锁 表锁类型，进入这里面
        // This is all sanity checks that the global and flush locks are always be acquired
        // before any other lock has been acquired and they must be in sync with the nesting.
        DEV { //DEBUG开关打开才会进入
            const LockRequestsMap::Iterator itGlobal = _requests.find(resourceIdGlobal);
            invariant(itGlobal->recursiveCount > 0);
            invariant(itGlobal->mode != MODE_NONE);

            // Check the MMAP V1 flush lock is held in the appropriate mode
            invariant(!IsForMMAPV1 ||
                      isLockHeldForMode(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
        };
    }

    // The notification object must be cleared before we invoke the lock manager, because
    // otherwise we might reset state if the lock becomes granted very fast.
    _notify.clear();

	//是一个新的资源类型锁，则LockManager::lock，如果是_requests已有的则是LockManager::convert
    LockResult result = isNew ? globalLockManager.lock(resId, request, mode)  //LockManager::lock
                              : globalLockManager.convert(resId, request, mode); //LockManager::convert

    if (result == LOCK_WAITING) {
		//PartitionedInstanceWideLockStats::recordWait;
        globalStats.recordWait(_id, resId, mode);
		//LockStats::recordWait;
        _stats.recordWait(resId, mode);
    }

    return result;
}

//LockerImpl<>::lockGlobalComplete
//LockerImpl<>::lock

//循环等待lock结果，直到LOCK_OK或者LOCK_DEADLOCK或者超时，超时时间timeout ms超时
//LockerImpl<>::restoreLockState中可能引起递归调用
template <bool IsForMMAPV1>
LockResult LockerImpl<IsForMMAPV1>::lockComplete(ResourceId resId,
                                                 LockMode mode,
                                                 Milliseconds timeout,
                                                 bool checkDeadlock) {
    // Under MMAP V1 engine a deadlock can occur if a thread goes to sleep waiting on
    // DB lock, while holding the flush lock, so it has to be released. This is only
    // correct to do if not in a write unit of work.
    //yieldFlushLock只针对MMAP引擎
    const bool yieldFlushLock = IsForMMAPV1 && !inAWriteUnitOfWork() &&
        resId.getType() != RESOURCE_GLOBAL && resId.getType() != RESOURCE_MUTEX &&
        resId != resourceIdMMAPV1Flush;
    if (yieldFlushLock) {
        invariant(unlock(resourceIdMMAPV1Flush));
    }

    LockResult result;

    // Don't go sleeping without bound in order to be able to report long waits or wake up for
    // deadlock detection.
    Milliseconds waitTime = std::min(timeout, DeadlockTimeout);
    const uint64_t startOfTotalWaitTime = curTimeMicros64();
    uint64_t startOfCurrentWaitTime = startOfTotalWaitTime;

    while (true) {
        // It is OK if this call wakes up spuriously, because we re-evaluate the remaining
        // wait time anyways.
        //等待条件变量被唤醒，LockManager::_onLockModeChanged->CondVarLockGrantNotification::notify唤醒等待线程
        result = _notify.wait(waitTime); //CondVarLockGrantNotification::wait

        // Account for the time spent waiting on the notification object
        const uint64_t curTimeMicros = curTimeMicros64();
        const uint64_t elapsedTimeMicros = curTimeMicros - startOfCurrentWaitTime;
        startOfCurrentWaitTime = curTimeMicros;

		//等待锁的时间记录下来
        globalStats.recordWaitTime(_id, resId, mode, elapsedTimeMicros);
        _stats.recordWaitTime(resId, mode, elapsedTimeMicros);

        if (result == LOCK_OK)
            break;

        if (checkDeadlock) {
            DeadlockDetector wfg(globalLockManager, this);
            if (wfg.check().hasCycle()) {
                warning() << "Deadlock found: " << wfg.toString();

                globalStats.recordDeadlock(resId, mode);
                _stats.recordDeadlock(resId, mode);

                result = LOCK_DEADLOCK;
                break;
            }
        }

        // If infinite timeout was requested, just keep waiting
        if (timeout == Milliseconds::max()) {
            continue;
        }

        const auto totalBlockTime = duration_cast<Milliseconds>(
            Microseconds(int64_t(curTimeMicros - startOfTotalWaitTime)));
        waitTime = (totalBlockTime < timeout) ? std::min(timeout - totalBlockTime, DeadlockTimeout)
                                              : Milliseconds(0);

		//获取锁超时
        if (waitTime == Milliseconds(0)) {
            break;
        }
    }

    // Cleanup the state, since this is an unused lock now
    if (result != LOCK_OK) {
        LockRequestsMap::Iterator it = _requests.find(resId);
		//释放锁，下面重新上锁重试
        _unlockImpl(&it);
    }

	//只针对MMAP引擎
    if (yieldFlushLock) {
        // We cannot obey the timeout here, because it is not correct to return from the lock
        // request with the flush lock released.
        //注意这里重新上锁，类似递归过程
        invariant(LOCK_OK == lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
    }

    return result;
}

template <bool IsForMMAPV1>
//注意LockerImpl<>::unlock 和 LockerImpl<>::_unlockImpl的区别
//  LockerImpl<>::unlock相比_unlockImpl增加了锁释放前的事务处理判断，判断释放需要延迟等到事务
//  提交后释放，确认事务提交完成后释放锁，从而起一个保护作用
bool LockerImpl<IsForMMAPV1>::_unlockImpl(LockRequestsMap::Iterator* it) {
	//LockManager::unlock释放该资源信息对应的LockRequest
    if (globalLockManager.unlock(it->objAddr())) {
		//如果是全局资源信息锁，则需要做全局并发活跃性统计
        if (it->key() == resourceIdGlobal) {
            invariant(_modeForTicket != MODE_NONE);
            auto holder = ticketHolders[_modeForTicket];
            _modeForTicket = MODE_NONE;
            if (holder) {
                holder->release();
            }
            _clientState.store(kInactive);
        }

        scoped_spinlock scopedLock(_lock);
        it->remove();

        return true;
    }

    return false;
}

template <bool IsForMMAPV1>
LockMode LockerImpl<IsForMMAPV1>::_getModeForMMAPV1FlushLock() const {
    invariant(IsForMMAPV1);

    LockMode mode = getLockMode(resourceIdGlobal);
    switch (mode) {
        case MODE_X:
        case MODE_IX:
            return MODE_IX;
        case MODE_S:
        case MODE_IS:
            return MODE_IS;
        default:
            invariant(false);
            return MODE_NONE;
    }
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isGlobalLockedRecursively() {
	//获取resourceIdGlobal对应的LockRequest
    auto globalLockRequest = _requests.find(resourceIdGlobal);

	//配合LockerImpl<>::lockComplete阅读
	//找到对应的LockRequest，并且recursiveCount引用计数大于1，说明存在递归上锁过程，例如lock等待超时
    return !globalLockRequest.finished() && globalLockRequest->recursiveCount > 1;
}

//
// Auto classes
//

AutoYieldFlushLockForMMAPV1Commit::AutoYieldFlushLockForMMAPV1Commit(Locker* locker)
    : _locker(static_cast<MMAPV1LockerImpl*>(locker)) {
    // Explicit yielding of the flush lock should happen only at global synchronization points
    // such as database drop. There should not be any active writes at these points.
    invariant(!_locker->inAWriteUnitOfWork());

    if (isMMAPV1()) {
        invariant(_locker->unlock(resourceIdMMAPV1Flush));
    }
}

AutoYieldFlushLockForMMAPV1Commit::~AutoYieldFlushLockForMMAPV1Commit() {
    if (isMMAPV1()) {
        invariant(LOCK_OK ==
                  _locker->lock(resourceIdMMAPV1Flush, _locker->_getModeForMMAPV1FlushLock()));
    }
}


AutoAcquireFlushLockForMMAPV1Commit::AutoAcquireFlushLockForMMAPV1Commit(Locker* locker)
    : _locker(locker), _released(false) {
    // The journal thread acquiring the journal lock in S-mode opens opportunity for deadlock
    // involving operations which do not acquire and release the Oplog collection's X lock
    // inside a WUOW (see SERVER-17416 for the sequence of events), therefore acquire it with
    // check for deadlock and back-off if one is encountered.
    //
    // This exposes theoretical chance that we might starve the journaling system, but given
    // that these deadlocks happen extremely rarely and are usually due to incorrect locking
    // policy, and we have the deadlock counters as part of the locking statistics, this is a
    // reasonable handling.
    //
    // In the worst case, if we are to starve the journaling system, the server will shut down
    // due to too much uncommitted in-memory journal, but won't have corruption.

    while (true) {
        LockResult result = _locker->lock(resourceIdMMAPV1Flush, MODE_S, Milliseconds::max(), true);
        if (result == LOCK_OK) {
            break;
        }

        invariant(result == LOCK_DEADLOCK);

        warning() << "Delayed journaling in order to avoid deadlock during MMAP V1 journal "
                  << "lock acquisition. See the previous messages for information on the "
                  << "involved threads.";
    }
}

void AutoAcquireFlushLockForMMAPV1Commit::upgradeFlushLockToExclusive() {
    // This should not be able to deadlock, since we already hold the S journal lock, which
    // means all writers are kicked out. Readers always yield the journal lock if they block
    // waiting on any other lock.
    invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush, MODE_X, Milliseconds::max(), false));

    // Lock bumps the recursive count. Drop it back down so that the destructor doesn't
    // complain.
    invariant(!_locker->unlock(resourceIdMMAPV1Flush));
}

void AutoAcquireFlushLockForMMAPV1Commit::release() {
    if (!_released) {
        invariant(_locker->unlock(resourceIdMMAPV1Flush));
        _released = true;
    }
}

AutoAcquireFlushLockForMMAPV1Commit::~AutoAcquireFlushLockForMMAPV1Commit() {
    release();
}


namespace {
/**
 *  Periodically purges unused lock buckets. The first time the lock is used again after
 *  cleanup it needs to be allocated, and similarly, every first use by a client for an intent
 *  mode may need to create a partitioned lock head. Cleanup is done roughtly once a minute.
 */
class UnusedLockCleaner : PeriodicTask {
public:
    std::string taskName() const {
        return "UnusedLockCleaner";
    }

    void taskDoWork() {
        LOG(2) << "cleaning up unused lock buckets of the global lock manager";
        getGlobalLockManager()->cleanupUnusedLocks();
    }
} unusedLockCleaner;
}  // namespace


//
// Standalone functions
//

LockManager* getGlobalLockManager() {
    return &globalLockManager;
}

//db.serverstats().locks
//LockStatsServerStatusSection中调用
void reportGlobalLockingStats(SingleThreadedLockStats* outStats) {
	//PartitionedInstanceWideLockStats::report
    globalStats.report(outStats);
}

void resetGlobalLockStats() {
    globalStats.reset();
}


// Ensures that there are two instances compiled for LockerImpl for the two values of the
// template argument.
template class LockerImpl<true>;
template class LockerImpl<false>;

// Definition for the hardcoded localdb and oplog collection info
const ResourceId resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, StringData("local"));
const ResourceId resourceIdOplog = ResourceId(RESOURCE_COLLECTION, StringData("local.oplog.rs"));
const ResourceId resourceIdAdminDB = ResourceId(RESOURCE_DATABASE, StringData("admin"));

/*
db/concurrency/d_concurrency.cpp:      _pbwm(opCtx->lockState(), resourceIdParallelBatchWriterMode),
db/concurrency/d_concurrency.cpp:    : _pbwm(lockState, resourceIdParallelBatchWriterMode, MODE_X),
*/ //和同步相关，参考Lock::ParallelBatchWriterMode::ParallelBatchWriterMode
const ResourceId resourceIdParallelBatchWriterMode =
    ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_PARALLEL_BATCH_WRITER_MODE);

}  // namespace mongo
