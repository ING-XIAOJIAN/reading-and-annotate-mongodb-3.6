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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class BSONObj;
class OperationContext;
class Timestamp;

namespace repl {

/**
 * A mock ReplicationConsistencyMarkers implementation that stores everything in memory.
 */
class ReplicationConsistencyMarkersMock : public ReplicationConsistencyMarkers {
    MONGO_DISALLOW_COPYING(ReplicationConsistencyMarkersMock);

public:
    ReplicationConsistencyMarkersMock() = default;

    void initializeMinValidDocument(OperationContext* opCtx) override;

    bool getInitialSyncFlag(OperationContext* opCtx) const override;
    void setInitialSyncFlag(OperationContext* opCtx) override;
    void clearInitialSyncFlag(OperationContext* opCtx) override;

    OpTime getMinValid(OperationContext* opCtx) const override;
    void setMinValid(OperationContext* opCtx, const OpTime& minValid) override;
    void setMinValidToAtLeast(OperationContext* opCtx, const OpTime& minValid) override;

    void setOplogTruncateAfterPoint(OperationContext* opCtx, const Timestamp& timestamp) override;
    Timestamp getOplogTruncateAfterPoint(OperationContext* opCtx) const override;

    void removeOldOplogDeleteFromPointField(OperationContext* opCtx) override;

    void setAppliedThrough(OperationContext* opCtx, const OpTime& optime) override;
    OpTime getAppliedThrough(OperationContext* opCtx) const override;

    void writeCheckpointTimestamp(OperationContext* opCtx, const Timestamp& timestamp) override;
    Timestamp getCheckpointTimestamp(OperationContext* opCtx) override;

private:
    mutable stdx::mutex _initialSyncFlagMutex;
    bool _initialSyncFlag = false;

    mutable stdx::mutex _minValidBoundariesMutex;
    OpTime _appliedThrough;
    /*
    对于回滚节点来说，导致状态被跳过的原因是进行了「refetch」，所以只需要记录每次「refetch」时同步源最新
    的 oplog 时间戳，「reapply」时拉取到最后一次「refetch」对应的这个同步源时间戳就可以保证状态的正确补
    齐，MongoDB 在实现中把这个时间戳称之为 minValid。
    参考https://mongoing.com/archives/77853
    */
    OpTime _minValid;
    Timestamp _oplogTruncateAfterPoint;
    Timestamp _checkpointTimestamp;
};

}  // namespace repl
}  // namespace mongo
