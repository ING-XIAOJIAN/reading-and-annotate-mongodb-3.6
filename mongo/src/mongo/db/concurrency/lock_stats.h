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

#pragma once

#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

class BSONObjBuilder;


/**
 * Operations for manipulating the lock statistics abstracting whether they are atomic or not.
 */
struct CounterOps {
    static int64_t get(const int64_t& counter) {
        return counter;
    }

    static int64_t get(const AtomicInt64& counter) {
        return counter.load();
    }

    static void set(int64_t& counter, int64_t value) {
        counter = value;
    }

    static void set(AtomicInt64& counter, int64_t value) {
        counter.store(value);
    }
    
    static void add(int64_t& counter, int64_t value) {
        counter += value;
    }

    static void add(int64_t& counter, const AtomicInt64& value) {
        counter += value.load();
    }

    static void add(AtomicInt64& counter, int64_t value) {
        counter.addAndFetch(value);
    }
};


/**
 * Bundle of locking statistics values.
 */ 
//LockStats


//统计分为两层，第一层代表资源类型：数组全局锁  库锁  表锁等，对应统计类型PerModeLockStatCounters
//  第二层代表锁模式：读锁、写锁、意向读锁、意向写锁，也就是MODE_X  MODE_S MODE_IX等，对应统计类型
//     第二层对应统计类型: LockStatCountersType(PerModeLockStatCounters.modeStats成员)

//LockStats._stats  
//LockStats._stats统计展示在LockStats<>::_report  慢日志中锁相关计数就在这里
template <typename CounterType>
struct LockStatCounters { 
    template <typename OtherType>
    void append(const LockStatCounters<OtherType>& other) {
        CounterOps::add(numAcquisitions, other.numAcquisitions);
        CounterOps::add(numWaits, other.numWaits);
        CounterOps::add(combinedWaitTimeMicros, other.combinedWaitTimeMicros);
        CounterOps::add(numDeadlocks, other.numDeadlocks);
    }

    //LockerImpl<>::getLockerInfo  慢日志打印的时候调用
    void reset() {
        CounterOps::set(numAcquisitions, 0);
        CounterOps::set(numWaits, 0);
        CounterOps::set(combinedWaitTimeMicros, 0);
        CounterOps::set(numDeadlocks, 0);
    }

//慢日志中的:
//locks:{ Global: { acquireCount: { r: 11814 }, acquireWaitCount: { r: 18 }, timeAcquiringMicros: { r: 12365 } }, 
// Database: { acquireCount: { r: 5907 } }, Collection: { acquireCount: { r: 5907 } } }

    //LockerImpl<>::lockBegin->PartitionedInstanceWideLockStats::recordAcquisition->LockStats::recordAcquisition
    CounterType numAcquisitions;
    //LockerImpl<>::lockBegin->PartitionedInstanceWideLockStats::recordWait->LockStats::recordWait
    CounterType numWaits;
    //LockerImpl<>::lockComplete->PartitionedInstanceWideLockStats::recordWaitTime->LockStats::recordWaitTime
    CounterType combinedWaitTimeMicros;
    //LockerImpl<>::lockComplete->PartitionedInstanceWideLockStats::recordDeadlock->LockStats::recordDeadlock
    CounterType numDeadlocks;
};


/**
 * Templatized lock statistics management class, which can be specialized with atomic integers
 * for the global stats and with regular integers for the per-locker stats.
 */
/*
参考
typedef LockStats<int64_t> SingleThreadedLockStats;
typedef LockStats<AtomicInt64> AtomicLockStats;
*/ 
//慢日志打印会用到，见OpDebug::report

//单次请求对应线程的锁统计在LockerImpl._stats中存储，全局锁统计在全局变量globalStats中存储

//注意db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) 三个的区别

//PartitionedInstanceWideLockStats._partitions[]为该类型
template <typename CounterType>
class LockStats {
public:
    // Declare the type for the lock counters bundle
    
//统计分为两层，第一层代表资源类型：数组全局锁  库锁  表锁等，对应统计类型PerModeLockStatCounters
//  第二层代表锁模式：读锁、写锁、意向读锁、意向写锁，也就是MODE_X  MODE_S MODE_IX等，对应统计类型
//     第二层对应统计类型: LockStatCountersType(PerModeLockStatCounters.modeStats成员)


    //真正的计数操作就是在LockStatCounters中实现
    typedef LockStatCounters<CounterType> LockStatCountersType;

    /**
     * Initializes the locking statistics with zeroes (calls reset).
     */
    LockStats();

    //LockerImpl<>::lockBegin->PartitionedInstanceWideLockStats::recordAcquisition->LockStats::recordAcquisition
    //LockerImpl<>::lockBegin
    void recordAcquisition(ResourceId resId, LockMode mode) {
        CounterOps::add(get(resId, mode).numAcquisitions, 1);
    }

    //LockerImpl<>::lockBegin->PartitionedInstanceWideLockStats::recordWait->LockStats::recordWait
    //LockerImpl<>::lockBegin
    void recordWait(ResourceId resId, LockMode mode) {
        CounterOps::add(get(resId, mode).numWaits, 1);
    }

    //LockerImpl<>::lockComplete->PartitionedInstanceWideLockStats::recordWaitTime->LockStats::recordWaitTime
    void recordWaitTime(ResourceId resId, LockMode mode, int64_t waitMicros) {
        CounterOps::add(get(resId, mode).combinedWaitTimeMicros, waitMicros);
    }

    //LockerImpl<>::lockComplete->PartitionedInstanceWideLockStats::recordDeadlock->LockStats::recordDeadlock
    void recordDeadlock(ResourceId resId, LockMode mode) {
        CounterOps::add(get(resId, mode).numDeadlocks, 1);
    }

    //上面的各种统计中会调用
    LockStatCountersType& get(ResourceId resId, LockMode mode) {
        if (resId == resourceIdOplog) {
            return _oplogStats.modeStats[mode];
        }
        //资源类型(全局、库、表等).锁模式(读锁、写锁、意向读锁、意向写锁等)
        return _stats[resId.getType()].modeStats[mode];
    }

    template <typename OtherType>
    //db.serverstats().locks  reportGlobalLockingStats->PartitionedInstanceWideLockStats::report
    void append(const LockStats<OtherType>& other) {
        typedef LockStatCounters<OtherType> OtherLockStatCountersType;

        // Append all lock stats
        for (int i = 0; i < ResourceTypesCount; i++) {
            for (int mode = 0; mode < LockModesCount; mode++) {
                const OtherLockStatCountersType& otherStats = other._stats[i].modeStats[mode];
                LockStatCountersType& thisStats = _stats[i].modeStats[mode];
                thisStats.append(otherStats);
            }
        }

        // Append the oplog stats
        for (int mode = 0; mode < LockModesCount; mode++) {
            const OtherLockStatCountersType& otherStats = other._oplogStats.modeStats[mode];
            LockStatCountersType& thisStats = _oplogStats.modeStats[mode];
            thisStats.append(otherStats);
        }
    }

    void report(BSONObjBuilder* builder) const;
    void reset();

private:
    // Necessary for the append call, which accepts argument of type different than our
    // template parameter.
    template <typename T>
    friend class LockStats;


    // Keep the per-mode lock stats next to each other in case we want to do fancy operations
    // such as atomic operations on 128-bit values.
    struct PerModeLockStatCounters { //下面的_stats和_oplogStats为该类型
        LockStatCountersType modeStats[LockModesCount]; //对应不同类型的锁 MODE_X IX S IS等
    };


    void _report(BSONObjBuilder* builder,
                 const char* sectionName,
                 const PerModeLockStatCounters& stat) const;


    // Split the lock stats per resource type and special-case the oplog so we can collect
    // more detailed stats for it.

//统计分为两层，第一层代表资源类型：数组全局锁  库锁  表锁等，对应统计类型PerModeLockStatCounters
//  第二层代表锁模式：读锁、写锁、意向读锁、意向写锁，也就是MODE_X  MODE_S MODE_IX等，对应统计类型
//     第二层对应统计类型: LockStatCountersType(PerModeLockStatCounters.modeStats成员)

    //资源类型：数组全局锁  库锁  表锁   LockStats<>::report中打印输出
    PerModeLockStatCounters _stats[ResourceTypesCount]; 
    //polog相关统计   LockStats<CounterType>::report中打印输出
    PerModeLockStatCounters _oplogStats;
};

//单次请求对应线程的锁统计在LockerImpl._stats中存储，全局锁统计在全局变量globalStats中存储

//LockerImpl._stats为该类型  代表的是本次请求相关的锁统计  慢日志中体现 (ServiceEntryPointMongod::handleRequest)
//慢日志打印会用到，见OpDebug::report
typedef LockStats<int64_t> SingleThreadedLockStats;
//PartitionedInstanceWideLockStats._partitions[]数组为该类型  
//LockStats<>::_report(db.serverStatus().locks查看)中获取相关信息,这里面是总的锁相关的统计
typedef LockStats<AtomicInt64> AtomicLockStats;


/**
 * Reports instance-wide locking statistics, which can then be converted to BSON or logged.
 */
void reportGlobalLockingStats(SingleThreadedLockStats* outStats);

/**
 * Currently used for testing only.
 */
void resetGlobalLockStats();

}  // namespace mongo
