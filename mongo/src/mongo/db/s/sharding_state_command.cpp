/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {
namespace {

//ShardingStateCmd::run调用
/*
xx:PRIMARY> db.runCommand({ shardingState: 1 })
"enabled" : true,
"configServer" : "xx/10.xx.xx.238:20014,10.xx.xx.234:20009,10.xx.xx.91:20016",
"shardName" : "xx_shard_1",
"clusterId" : ObjectId("5e4acb7a658f0a4a5029f452"),
"versions" : {
		"cloud_track.system.drop.1622998800i482t18.dailyCloudOperateInfo_01" : Timestamp(0, 0),
		"config.system.drop.1622826001i6304t18.cache.chunks.cloud_track.dailyCloudOperateInfo_30" : Timestamp(0, 0),
		"cloud_track.system.drop.1622826000i5598t18.dailyCloudOperateInfo_30" : Timestamp(0, 0),
		"config.system.drop.1622653201i5382t18.cache.chunks.cloud_track.dailyCloudOperateInfo_28" : Timestamp(0, 0),
		"config.system.drop.1622566801i4563t18.cache.chunks.cloud_track.dailyCloudOperateInfo_27" : Timestamp(0, 0),
		"config.system.drop.1622480401i6387t18.cache.chunks.cloud_track.dailyCloudOperateInfo_26" : Timestamp(0, 0),
		"cloud_track.system.drop.1622480400i723t18.dailyCloudOperateInfo_26" : Timestamp(0, 0),
		"cloud_track.system.drop.1622307600i100t18.dailyCloudOperateInfo_24" : Timestamp(0, 0),
		"cloud_track.system.drop.1622221200i533t18.dailyCloudOperateInfo_23" : Timestamp(0, 0),
		"config.system.drop.1621789201i5341t18.cache.chunks.cloud_track.dailyCloudOperateInfo_18" : Timestamp(0, 0),
		"config.system.drop.1621702801i5647t18.cache.chunks.cloud_track.dailyCloudOperateInfo_17" : Timestamp(0, 0),
		"config.system.drop.1621616401i7264t18.cache.chunks.cloud_track.dailyCloudOperateInfo_16" : Timestamp(0, 0),

*/ //只有mongod支持，获取某个分片上的表版本信息

class ShardingStateCmd : public BasicCommand {
public:
    ShardingStateCmd() : BasicCommand("shardingState") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        ActionSet actions;
        actions.addAction(ActionType::shardingState);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        ShardingState::get(opCtx)->appendInfo(opCtx, result);
        return true;
    }

} shardingStateCmd;

}  // namespace
}  // namespace mongo
