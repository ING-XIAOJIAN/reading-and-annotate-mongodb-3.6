/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <map>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

using std::string;
using std::stringstream;

/**
 * Admin command to display global lock information
 */
/*
featdoc:PRIMARY> use admin
switched to db admin
featdoc:PRIMARY> db.runCommand({lockInfo: 1})
{
        "lockInfo" : [
                {
                        "resourceId" : "{2305843009213693953: Global, 1}",
                        "granted" : [
                                {
                                        "mode" : "IS",
                                        "convertMode" : "NONE",
                                        "enqueueAtFront" : false,
                                        "compatibleFirst" : false,
                                        "desc" : "conn65",
                                        "connectionId" : 65,
                                        "client" : "127.0.0.1:52223",
                                        "opid" : 1414386
                                }
                        ],
                        "pending" : [ ]
                },
                {
                        "resourceId" : "{9695931499680953263: Collection, 472559462826177455}",
                        "granted" : [
                                {
                                        "mode" : "IS",
                                        "convertMode" : "NONE",
                                        "enqueueAtFront" : false,
                                        "compatibleFirst" : false,
                                        "desc" : "conn60",
                                        "connectionId" : 60,
                                        "client" : "172.xx.xx.29:43066",
                                        "opid" : 1412163
                                },
                                {
                                        "mode" : "IS",
                                        "convertMode" : "NONE",
                                        "enqueueAtFront" : false,
                                        "compatibleFirst" : false,
                                        "desc" : "conn59",
                                        "connectionId" : 59,
                                        "client" : "172.xx.xx.29:43019",
                                        "opid" : 1412143
                                }
                        ],
                        "pending" : [ ]
                },
                {
                        "resourceId" : "{8576409733318454219: Database, 1658880705677372363}",
                        "granted" : [
                                {
                                        "mode" : "IS",
                                        "convertMode" : "NONE",
                                        "enqueueAtFront" : false,
                                        "compatibleFirst" : false,
                                        "desc" : "conn60",
                                        "connectionId" : 60,
                                        "client" : "172.xx.xx.29:43066",
                                        "opid" : 1412163
                                },
                                {
                                        "mode" : "IS",
                                        "convertMode" : "NONE",
                                        "enqueueAtFront" : false,
                                        "compatibleFirst" : false,

                                        //client::reportState 
                                        "desc" : "conn59",
                                        "connectionId" : 59,
                                        "client" : "172.xx.xx.29:43019",
                                        "opid" : 1412143
                                }
                        ],
                        "pending" : [ ]
                }
        ],
        "ok" : 1
}
featdoc:PRIMARY>
//要有流量的时候才会有输出，才会有锁信息
*/ //use admin;  db.runCommand({lockInfo: 1})
class CmdLockInfo : public BasicCommand {
public:
    virtual bool slaveOk() const {
        return true;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const {
        return false;
    }

    virtual void help(stringstream& help) const {
        help << "show all lock info on the server";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::serverStatus);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    CmdLockInfo() : BasicCommand("lockInfo") {}

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        std::map<LockerId, BSONObj> lockToClientMap;

		//遍历获取当前所有的client信息
        for (ServiceContext::LockedClientsCursor cursor(opCtx->getClient()->getServiceContext());
             Client* client = cursor.next();) {
            invariant(client);

            stdx::lock_guard<Client> lk(*client);
            const OperationContext* clientOpCtx = client->getOperationContext();

            // Operation context specific information
            //获取客户端信息
            if (clientOpCtx) {
                BSONObjBuilder infoBuilder;
                // The client information   
                //client::reportState  
                client->reportState(infoBuilder);

                infoBuilder.append("opid", clientOpCtx->getOpID());
                LockerId lockerId = clientOpCtx->lockState()->getId();
                lockToClientMap.insert({lockerId, infoBuilder.obj()});
            }
        }

		//LockManager::getLockInfoBSON
        getGlobalLockManager()->getLockInfoBSON(lockToClientMap, &result);
        return true;
    }
} cmdLockInfo;
}
