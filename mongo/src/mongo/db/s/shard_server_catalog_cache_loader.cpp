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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_server_catalog_cache_loader.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_group.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using namespace shardmetadatautil;

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

namespace {

AtomicUInt64 taskIdGenerator{0};

/**
 * Constructs the options for the loader thread pool.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "ShardServerCatalogCacheLoader";
    options.minThreads = 0;
    options.maxThreads = 6;

    // Ensure all threads have a client.
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return options;
}

/**
 * Takes a CollectionAndChangedChunks object and persists the changes to the shard's metadata
 * collections.
 *
 * Returns ConflictingOperationInProgress if a chunk is found with a new epoch.
 */
Status persistCollectionAndChangedChunks(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const CollectionAndChangedChunks& collAndChunks) {
    // Update the collections collection entry for 'nss' in case there are any new updates.
    ShardCollectionType update = ShardCollectionType(nss,
                                                     collAndChunks.uuid,
                                                     collAndChunks.epoch,
                                                     collAndChunks.shardKeyPattern,
                                                     collAndChunks.defaultCollation,
                                                     collAndChunks.shardKeyIsUnique);
    Status status = updateShardCollectionsEntry(opCtx,
                                                BSON(ShardCollectionType::ns() << nss.ns()),
                                                update.toBSON(),
                                                BSONObj(),
                                                true /*upsert*/);
    if (!status.isOK()) {
        return status;
    }

    // Mark the chunk metadata as refreshing, so that secondaries are aware of refresh.
    status = setPersistedRefreshFlags(opCtx, nss);
    if (!status.isOK()) {
        return status;
    }

    // Update the chunks.
    status = updateShardChunks(opCtx, nss, collAndChunks.changedChunks, collAndChunks.epoch);
    if (!status.isOK()) {
        return status;
    }

    // Mark the chunk metadata as done refreshing.
    status =
        unsetPersistedRefreshFlags(opCtx, nss, collAndChunks.changedChunks.back().getVersion());
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

/**
 * This function will throw on error!
 *
 * Retrieves the persisted max chunk version for 'nss', if there are any persisted chunks. If there
 * are none -- meaning there's no persisted metadata for 'nss' --, returns a
 * ChunkVersion::UNSHARDED() version.
 *
 * It is unsafe to call this when a task for 'nss' is running concurrently because the collection
 * could be dropped and recreated between reading the collection epoch and retrieving the chunk,
 * which would make the returned ChunkVersion corrupt.
 */
ChunkVersion getPersistedMaxVersion(OperationContext* opCtx, const NamespaceString& nss) {
    // Must read the collections entry to get the epoch to pass into ChunkType for shard's chunk
    // collection.
    auto statusWithCollection = readShardCollectionsEntry(opCtx, nss);
    if (statusWithCollection == ErrorCodes::NamespaceNotFound) {
        // There is no persisted metadata.
        return ChunkVersion::UNSHARDED();
    }
    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Failed to read persisted collections entry for collection '"
                          << nss.ns()
                          << "' due to '"
                          << statusWithCollection.getStatus().toString()
                          << "'.",
            statusWithCollection.isOK());

    auto statusWithChunk =
        shardmetadatautil::readShardChunks(opCtx,
                                           nss,
                                           BSONObj(),
                                           BSON(ChunkType::lastmod() << -1),
                                           1LL,
                                           statusWithCollection.getValue().getEpoch());
    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Failed to read highest version persisted chunk for collection '"
                          << nss.ns()
                          << "' due to '"
                          << statusWithChunk.getStatus().toString()
                          << "'.",
            statusWithChunk.isOK());

    return statusWithChunk.getValue().empty() ? ChunkVersion::UNSHARDED()
                                              : statusWithChunk.getValue().front().getVersion();
}

/**
 * This function will throw on error!
 *
 * Tries to find persisted chunk metadata with chunk versions GTE to 'version'.
 *
 * If 'version's epoch matches persisted metadata, returns persisted metadata GTE 'version'.
 * If 'version's epoch doesn't match persisted metadata, returns all persisted metadata.
 * If collections entry does not exist, throws NamespaceNotFound error. Can return an empty
 * chunks vector in CollectionAndChangedChunks without erroring, if collections entry IS found.
 */
CollectionAndChangedChunks getPersistedMetadataSinceVersion(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            ChunkVersion version,
                                                            const bool okToReadWhileRefreshing) {
    ShardCollectionType shardCollectionEntry =
        uassertStatusOK(readShardCollectionsEntry(opCtx, nss));

    // If the persisted epoch doesn't match what the CatalogCache requested, read everything.
    ChunkVersion startingVersion = (shardCollectionEntry.getEpoch() == version.epoch())
        ? version
        : ChunkVersion(0, 0, shardCollectionEntry.getEpoch());

    QueryAndSort diff = createShardChunkDiffQuery(startingVersion);

    auto changedChunks = uassertStatusOK(
        readShardChunks(opCtx, nss, diff.query, diff.sort, boost::none, startingVersion.epoch()));

    return CollectionAndChangedChunks{shardCollectionEntry.getUUID(),
                                      shardCollectionEntry.getEpoch(),
                                      shardCollectionEntry.getKeyPattern().toBSON(),
                                      shardCollectionEntry.getDefaultCollation(),
                                      shardCollectionEntry.getUnique(),
                                      std::move(changedChunks)};
}

/**
 * Attempts to read the collection and chunk metadata. May not read a complete diff if the metadata
 * for the collection is being updated concurrently. This is safe if those updates are appended.
 *
 * If the epoch changes while reading the chunks, returns an empty object.
 */
StatusWith<CollectionAndChangedChunks> getIncompletePersistedMetadataSinceVersion(
    OperationContext* opCtx, const NamespaceString& nss, ChunkVersion version) {

    try {
        CollectionAndChangedChunks collAndChunks =
            getPersistedMetadataSinceVersion(opCtx, nss, version, false);
        if (collAndChunks.changedChunks.empty()) {
            // Found a collections entry, but the chunks are being updated.
            return CollectionAndChangedChunks();
        }

        // Make sure the collections entry epoch has not changed since we began reading chunks --
        // an epoch change between reading the collections entry and reading the chunk metadata
        // would invalidate the chunks.

        auto afterShardCollectionsEntry = uassertStatusOK(readShardCollectionsEntry(opCtx, nss));
        if (collAndChunks.epoch != afterShardCollectionsEntry.getEpoch()) {
            // The collection was dropped and recreated since we began. Return empty results.
            return CollectionAndChangedChunks();
        }

        return collAndChunks;
    } catch (const DBException& ex) {
        Status status = ex.toStatus();
        if (status == ErrorCodes::NamespaceNotFound) {
            return CollectionAndChangedChunks();
        }
        return Status(ErrorCodes::OperationFailed,
                      str::stream() << "Failed to load local metadata due to '" << status.toString()
                                    << "'.");
    }
}

/**
 * Sends _flushRoutingTableCacheUpdates to the primary to force it to refresh its routing table for
 * collection 'nss' and then waits for the refresh to replicate to this node.
 */
//forcePrimaryRefreshAndWaitForReplication：
//  mongos向mongod shard server主节点发送forceRoutingTableRefresh命令，shard server主节点收到后，
//  在FlushRoutingTableCacheUpdates::run中处理，注意这个是内部命令
//还有一个外部强制路由刷新db.adminCommand({"flushRouterConfig":1})

//ShardServerCatalogCacheLoader::getChunksSince->ShardServerCatalogCacheLoader::_runSecondaryGetChunksSince调用
void forcePrimaryRefreshAndWaitForReplication(OperationContext* opCtx, const NamespaceString& nss) {
    auto const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->enabled());

    auto selfShard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardingState->getShardName()));

    auto cmdResponse = uassertStatusOK(selfShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        BSON("forceRoutingTableRefresh" << nss.ns()),
        Seconds{30},
        Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(cmdResponse.commandStatus);

    uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->waitUntilOpTimeForRead(
        opCtx, {LogicalTime::fromOperationTime(cmdResponse.response), boost::none}));
}

/**
 * Reads the local chunk metadata to obtain the current ChunkVersion. If there is no local
 * metadata for the namespace, returns ChunkVersion::UNSHARDED(), since only metadata for sharded
 * collections is persisted.
 */
ChunkVersion getLocalVersion(OperationContext* opCtx, const NamespaceString& nss) {
    auto swRefreshState = getPersistedRefreshFlags(opCtx, nss);
    if (swRefreshState == ErrorCodes::NamespaceNotFound)
        return ChunkVersion::UNSHARDED();
    return uassertStatusOK(std::move(swRefreshState)).lastRefreshedCollectionVersion;
}

}  // namespace

ShardServerCatalogCacheLoader::ShardServerCatalogCacheLoader(
    std::unique_ptr<CatalogCacheLoader> configServerLoader)
    : _configServerLoader(std::move(configServerLoader)),
      _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

ShardServerCatalogCacheLoader::~ShardServerCatalogCacheLoader() {
    // Prevent further scheduling, then interrupt ongoing tasks.
    _threadPool.shutdown();
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _contexts.interrupt(ErrorCodes::InterruptedAtShutdown);
        ++_term;
    }

    _threadPool.join();
    invariant(_contexts.isEmpty());
}

void ShardServerCatalogCacheLoader::notifyOfCollectionVersionUpdate(const NamespaceString& nss) {
    _namespaceNotifications.notifyChange(nss);
}

//ShardingState::initializeFromShardIdentity中调用
void ShardServerCatalogCacheLoader::initializeReplicaSetRole(bool isPrimary) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role == ReplicaSetRole::None);

    if (isPrimary) {
        _role = ReplicaSetRole::Primary;
    } else {
        _role = ReplicaSetRole::Secondary;
    }
}

void ShardServerCatalogCacheLoader::onStepDown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role != ReplicaSetRole::None);
    _contexts.interrupt(ErrorCodes::PrimarySteppedDown);
    ++_term;
    _role = ReplicaSetRole::Secondary;
}

void ShardServerCatalogCacheLoader::onStepUp() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role != ReplicaSetRole::None);
    ++_term;
    _role = ReplicaSetRole::Primary;
}

//CatalogCache::_scheduleCollectionRefresh调用 
std::shared_ptr<Notification<void>> 
  ShardServerCatalogCacheLoader::getChunksSince(
    const NamespaceString& nss,
    ChunkVersion version,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn) {
    long long currentTerm;
    bool isPrimary;
    {
        // Take the mutex so that we can discern whether we're primary or secondary and schedule a
        // task with the corresponding _term value.
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_role != ReplicaSetRole::None);

        currentTerm = _term;
        isPrimary = (_role == ReplicaSetRole::Primary);
    }

    auto notify = std::make_shared<Notification<void>>();

    uassertStatusOK(_threadPool.schedule(
        [ this, nss, version, callbackFn, notify, isPrimary, currentTerm ]() noexcept {
            auto context = _contexts.makeOperationContext(*Client::getCurrent());

            {
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                // We may have missed an OperationContextGroup interrupt since this operation began
                // but before the OperationContext was added to the group. So we'll check that
                // we're still in the same _term.
                if (_term != currentTerm) {
                    callbackFn(context.opCtx(),
                               Status{ErrorCodes::Interrupted,
                                      "Unable to refresh routing table because replica set state "
                                      "changed or node is shutting down."});
                    notify->set();
                    return;
                }
            }

            try {
                if (isPrimary) {
                    _schedulePrimaryGetChunksSince(
                        context.opCtx(), nss, version, currentTerm, callbackFn, notify);
                } else {
                    _runSecondaryGetChunksSince(context.opCtx(), nss, version, callbackFn);
                }
            } catch (const DBException& ex) {
                callbackFn(context.opCtx(), ex.toStatus());
                notify->set();
            }
        }));

    return notify;
}

void ShardServerCatalogCacheLoader::waitForCollectionFlush(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    stdx::unique_lock<stdx::mutex> lg(_mutex);
    const auto initialTerm = _term;

    boost::optional<uint64_t> taskNumToWait;

    while (true) {
        uassert(ErrorCodes::NotMaster,
                str::stream() << "Unable to wait for collection metadata flush for " << nss.ns()
                              << " because the node's replication role changed.",
                _role == ReplicaSetRole::Primary && _term == initialTerm);

        auto it = _taskLists.find(nss);

        // If there are no tasks for the specified namespace, everything must have been completed
        if (it == _taskLists.end())
            return;

        auto& taskList = it->second;

        if (!taskNumToWait) {
            const auto& lastTask = taskList.back();
            taskNumToWait = lastTask.taskNum;
        } else {
            const auto& activeTask = taskList.front();

            if (activeTask.taskNum > *taskNumToWait) {
                auto secondTaskIt = std::next(taskList.begin());

                // Because of an optimization where a namespace drop clears all tasks except the
                // active it is possible that the task number we are waiting on will never actually
                // be written. Because of this we move the task number to the drop which can only be
                // in the active task or in the one after the active.
                if (activeTask.dropped) {
                    taskNumToWait = activeTask.taskNum;
                } else if (secondTaskIt != taskList.end() && secondTaskIt->dropped) {
                    taskNumToWait = secondTaskIt->taskNum;
                } else {
                    return;
                }
            }
        }

        // It is not safe to use taskList after this call, because it will unlock and lock the tasks
        // mutex, so we just loop around.
        taskList.waitForActiveTaskCompletion(lg);
    }
}

//primary调用_schedulePrimaryGetChunksSince，secondary调用_runSecondaryGetChunksSince
//ShardServerCatalogCacheLoader::getChunksSince调用
void ShardServerCatalogCacheLoader::_runSecondaryGetChunksSince(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn) {
    forcePrimaryRefreshAndWaitForReplication(opCtx, nss);

    // Read the local metadata.
    auto swCollAndChunks =
        _getCompletePersistedMetadataForSecondarySinceVersion(opCtx, nss, catalogCacheSinceVersion);
    callbackFn(opCtx, std::move(swCollAndChunks));
}

//primary调用_schedulePrimaryGetChunksSince，secondary调用_runSecondaryGetChunksSince
void ShardServerCatalogCacheLoader::_schedulePrimaryGetChunksSince(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    long long termScheduled,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn,
    std::shared_ptr<Notification<void>> notify) {

    // Get the max version the loader has.
    const ChunkVersion maxLoaderVersion = [&] {
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            auto taskListIt = _taskLists.find(nss);

            if (taskListIt != _taskLists.end() &&
                taskListIt->second.hasTasksFromThisTerm(termScheduled)) {
                // Enqueued tasks have the latest metadata
                return taskListIt->second.getHighestVersionEnqueued();
            }
        }

        // If there are no enqueued tasks, get the max persisted
        return getPersistedMaxVersion(opCtx, nss);
    }();

    auto remoteRefreshCallbackFn = [this,
                                    nss,
                                    catalogCacheSinceVersion,
                                    maxLoaderVersion,
                                    termScheduled,
                                    callbackFn,
                                    notify](
        OperationContext* opCtx,
        StatusWith<CollectionAndChangedChunks> swCollectionAndChangedChunks) {

        if (swCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound) {
            Status scheduleStatus = _ensureMajorityPrimaryAndScheduleTask(
                opCtx, nss, Task{swCollectionAndChangedChunks, maxLoaderVersion, termScheduled});
            if (!scheduleStatus.isOK()) {
                callbackFn(opCtx, scheduleStatus);
                notify->set();
                return;
            }
			
            log() << "Cache loader remotely refreshed for collection " << nss << " from version "
                  << maxLoaderVersion << " and no metadata was found.";
        } else if (swCollectionAndChangedChunks.isOK()) {
            auto& collAndChunks = swCollectionAndChangedChunks.getValue();

            if (collAndChunks.changedChunks.back().getVersion().epoch() != collAndChunks.epoch) {
                swCollectionAndChangedChunks =
                    Status{ErrorCodes::ConflictingOperationInProgress,
                           str::stream()
                               << "Invalid chunks found when reloading '"
                               << nss.toString()
                               << "' Previous collection epoch was '"
                               << collAndChunks.epoch.toString()
                               << "', but found a new epoch '"
                               << collAndChunks.changedChunks.back().getVersion().epoch().toString()
                               << "'. Collection was dropped and recreated."};
            } else {
                if ((collAndChunks.epoch != maxLoaderVersion.epoch()) ||
                    (collAndChunks.changedChunks.back().getVersion() > maxLoaderVersion)) {
                    Status scheduleStatus = _ensureMajorityPrimaryAndScheduleTask(
                        opCtx,
                        nss,
                        Task{swCollectionAndChangedChunks, maxLoaderVersion, termScheduled});
                    if (!scheduleStatus.isOK()) {
                        callbackFn(opCtx, scheduleStatus);
                        notify->set();
                        return;
                    }
                }

                log() << "Cache loader remotely refreshed for collection " << nss
                      << " from collection version " << maxLoaderVersion
                      << " and found collection version "
                      << collAndChunks.changedChunks.back().getVersion();

                // Metadata was found remotely -- otherwise would have received NamespaceNotFound
                // rather than Status::OK(). Return metadata for CatalogCache that's GTE
                // catalogCacheSinceVersion, from the loader's persisted and enqueued metadata.

                swCollectionAndChangedChunks =
                    _getLoaderMetadata(opCtx, nss, catalogCacheSinceVersion, termScheduled);
                if (swCollectionAndChangedChunks.isOK()) {
                    // After finding metadata remotely, we must have found metadata locally.
                    invariant(!collAndChunks.changedChunks.empty());
                }
            }
        }

        // Complete the callbackFn work.
        callbackFn(opCtx, std::move(swCollectionAndChangedChunks));
        notify->set();
    };

    // Refresh the loader's metadata from the config server. The caller's request will
    // then be serviced from the loader's up-to-date metadata.
    _configServerLoader->getChunksSince(nss, maxLoaderVersion, remoteRefreshCallbackFn);
}

StatusWith<CollectionAndChangedChunks> ShardServerCatalogCacheLoader::_getLoaderMetadata(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    const long long term) {

    // Get the enqueued metadata first. Otherwise we could miss data between reading persisted and
    // enqueued, if an enqueued task finished after the persisted read but before the enqueued read.

    auto enqueuedRes = _getEnqueuedMetadata(nss, catalogCacheSinceVersion, term);
    bool tasksAreEnqueued = std::move(enqueuedRes.first);
    CollectionAndChangedChunks enqueued = std::move(enqueuedRes.second);

    auto swPersisted =
        getIncompletePersistedMetadataSinceVersion(opCtx, nss, catalogCacheSinceVersion);
    CollectionAndChangedChunks persisted;
    if (swPersisted == ErrorCodes::NamespaceNotFound) {
        // No persisted metadata found, create an empty object.
        persisted = CollectionAndChangedChunks();
    } else if (!swPersisted.isOK()) {
        return swPersisted;
    } else {
        persisted = std::move(swPersisted.getValue());
    }

    log() << "Cache loader found "
          << (enqueued.changedChunks.empty()
                  ? (tasksAreEnqueued ? "a drop enqueued" : "no enqueued metadata")
                  : ("enqueued metadata from " +
                     enqueued.changedChunks.front().getVersion().toString() + " to " +
                     enqueued.changedChunks.back().getVersion().toString()))
          << " and " << (persisted.changedChunks.empty()
                             ? "no persisted metadata"
                             : ("persisted metadata from " +
                                persisted.changedChunks.front().getVersion().toString() + " to " +
                                persisted.changedChunks.back().getVersion().toString()))
          << ", GTE cache version " << catalogCacheSinceVersion;

    if (!tasksAreEnqueued) {
        // There are no tasks in the queue. Return the persisted metadata.
        return persisted;
    } else if (persisted.changedChunks.empty() || enqueued.changedChunks.empty() ||
               enqueued.epoch != persisted.epoch) {
        // There is a task queue and:
        // - nothing is persisted.
        // - nothing was returned from enqueued, which means the last task enqueued is a drop task.
        // - the epoch changed in the enqueued metadata, which means there's a drop operation
        //   enqueued somewhere.
        // Whichever the cause, the persisted metadata is out-dated/non-existent. Return enqueued
        // results.
        return enqueued;
    } else {
        // There can be overlap between persisted and enqueued metadata because enqueued work can
        // be applied while persisted was read. We must remove this overlap.

        const ChunkVersion minEnqueuedVersion = enqueued.changedChunks.front().getVersion();

        // Remove chunks from 'persisted' that are GTE the minimum in 'enqueued' -- this is
        // the overlap.
        auto persistedChangedChunksIt = persisted.changedChunks.begin();
        while (persistedChangedChunksIt != persisted.changedChunks.end() &&
               persistedChangedChunksIt->getVersion() < minEnqueuedVersion) {
            ++persistedChangedChunksIt;
        }
        persisted.changedChunks.erase(persistedChangedChunksIt, persisted.changedChunks.end());

        // Append 'enqueued's chunks to 'persisted', which no longer overlaps.
        persisted.changedChunks.insert(persisted.changedChunks.end(),
                                       enqueued.changedChunks.begin(),
                                       enqueued.changedChunks.end());

        return persisted;
    }
}

std::pair<bool, CollectionAndChangedChunks> ShardServerCatalogCacheLoader::_getEnqueuedMetadata(
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    const long long term) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    auto taskListIt = _taskLists.find(nss);

    if (taskListIt == _taskLists.end()) {
        return std::make_pair(false, CollectionAndChangedChunks());
    } else if (!taskListIt->second.hasTasksFromThisTerm(term)) {
        // If task list does not have a term that matches, there's no valid task data to collect.
        return std::make_pair(false, CollectionAndChangedChunks());
    }

    // Only return task data of tasks scheduled in the same term as the given 'term': older term
    // task data is no longer valid.
    CollectionAndChangedChunks collAndChunks = taskListIt->second.getEnqueuedMetadataForTerm(term);

    // Return all the results if 'catalogCacheSinceVersion's epoch does not match. Otherwise, trim
    // the results to be GTE to 'catalogCacheSinceVersion'.

    if (collAndChunks.epoch != catalogCacheSinceVersion.epoch()) {
        return std::make_pair(true, collAndChunks);
    }

    auto changedChunksIt = collAndChunks.changedChunks.begin();
    while (changedChunksIt != collAndChunks.changedChunks.end() &&
           changedChunksIt->getVersion() < catalogCacheSinceVersion) {
        ++changedChunksIt;
    }
    collAndChunks.changedChunks.erase(collAndChunks.changedChunks.begin(), changedChunksIt);

    return std::make_pair(true, collAndChunks);
}

Status ShardServerCatalogCacheLoader::_ensureMajorityPrimaryAndScheduleTask(
    OperationContext* opCtx, const NamespaceString& nss, Task task) {
    Status linearizableReadStatus = waitForLinearizableReadConcern(opCtx);
    if (!linearizableReadStatus.isOK()) {
        return {linearizableReadStatus.code(),
                str::stream() << "Unable to schedule routing table update because this is not the"
                              << " majority primary and may not have the latest data. Error: "
                              << linearizableReadStatus.reason()};
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    const bool wasEmpty = _taskLists[nss].empty();
    _taskLists[nss].addTask(std::move(task));

    if (wasEmpty) {
        Status status = _threadPool.schedule([this, nss]() { _runTasks(nss); });
        if (!status.isOK()) {
            log() << "Cache loader failed to schedule persisted metadata update"
                  << " task for namespace '" << nss << "' due to '" << redact(status)
                  << "'. Clearing task list so that scheduling"
                  << " will be attempted by the next caller to refresh this namespace.";
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            _taskLists.erase(nss);
        }
        return status;
    }

    return Status::OK();
}

void ShardServerCatalogCacheLoader::_runTasks(const NamespaceString& nss) {
    auto context = _contexts.makeOperationContext(*Client::getCurrent());

    bool taskFinished = false;
    try {
        _updatePersistedMetadata(context.opCtx(), nss);
        taskFinished = true;
    } catch (const DBException& ex) {
        Status exceptionStatus = ex.toStatus();

        // This thread must stop if we are shutting down
        if (ErrorCodes::isShutdownError(exceptionStatus.code())) {
            log() << "Failed to persist chunk metadata update for collection '" << nss
                  << "' due to shutdown.";
            return;
        }

        log() << redact(exceptionStatus);
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    // If task completed successfully, remove it from work queue
    if (taskFinished) {
        _taskLists[nss].pop_front();
    }

    // Schedule more work if there is any
    if (!_taskLists[nss].empty()) {
        Status status = _threadPool.schedule([this, nss]() { _runTasks(nss); });
        if (!status.isOK()) {
            log() << "Cache loader failed to schedule a persisted metadata update"
                  << " task for namespace '" << nss << "' due to '" << redact(status)
                  << "'. Clearing task list so that scheduling will be attempted by the next"
                  << " caller to refresh this namespace.";
            _taskLists.erase(nss);
        }
    } else {
        _taskLists.erase(nss);
    }
}

void ShardServerCatalogCacheLoader::_updatePersistedMetadata(OperationContext* opCtx,
                                                             const NamespaceString& nss) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    const Task& task = _taskLists[nss].front();
    invariant(task.dropped || !task.collectionAndChangedChunks->changedChunks.empty());

    // If this task is from an old term and no longer valid, do not execute and return true so that
    // the task gets removed from the task list
    if (task.termCreated != _term) {
        return;
    }

    lock.unlock();

    // Check if this is a drop task
    if (task.dropped) {
        // The namespace was dropped. The persisted metadata for the collection must be cleared.
        Status status = dropChunksAndDeleteCollectionsEntry(opCtx, nss);
        uassert(status.code(),
                str::stream() << "Failed to clear persisted chunk metadata for collection '"
                              << nss.ns()
                              << "' due to '"
                              << status.reason()
                              << "'. Will be retried.",
                status.isOK());
        return;
    }

    Status status =
        persistCollectionAndChangedChunks(opCtx, nss, task.collectionAndChangedChunks.get());

    uassert(status.code(),
            str::stream() << "Failed to update the persisted chunk metadata for collection '"
                          << nss.ns()
                          << "' from '"
                          << task.minQueryVersion.toString()
                          << "' to '"
                          << task.maxQueryVersion.toString()
                          << "' due to '"
                          << status.reason()
                          << "'. Will be retried.",
            status.isOK());

    LOG(1) << "Successfully updated persisted chunk metadata for collection '" << nss << "' from '"
           << task.minQueryVersion << "' to collection version '" << task.maxQueryVersion << "'.";
}

CollectionAndChangedChunks
ShardServerCatalogCacheLoader::_getCompletePersistedMetadataForSecondarySinceVersion(
    OperationContext* opCtx, const NamespaceString& nss, const ChunkVersion& version) {
    // Keep trying to load the metadata until we get a complete view without updates being
    // concurrently applied.
    while (true) {
        const auto beginRefreshState = [&]() {
            while (true) {
                auto notif = _namespaceNotifications.createNotification(nss);

                auto refreshState = uassertStatusOK(getPersistedRefreshFlags(opCtx, nss));

                if (!refreshState.refreshing) {
                    return refreshState;
                }

                notif.get(opCtx);
            }
        }();

        // Load the metadata.
        CollectionAndChangedChunks collAndChangedChunks =
            getPersistedMetadataSinceVersion(opCtx, nss, version, true);

        // Check that no updates were concurrently applied while we were loading the metadata: this
        // could cause the loaded metadata to provide an incomplete view of the chunk ranges.
        const auto endRefreshState = uassertStatusOK(getPersistedRefreshFlags(opCtx, nss));

        if (beginRefreshState == endRefreshState) {
            return collAndChangedChunks;
        }

        LOG(1) << "Cache loader read meatadata while updates were being applied: this metadata may"
               << " be incomplete. Retrying. Refresh state before read: " << beginRefreshState
               << ". Current refresh state: '" << endRefreshState << "'.";
    }
}

ShardServerCatalogCacheLoader::Task::Task(
    StatusWith<CollectionAndChangedChunks> statusWithCollectionAndChangedChunks,
    ChunkVersion minimumQueryVersion,
    long long currentTerm)
    : taskNum(taskIdGenerator.fetchAndAdd(1)),
      minQueryVersion(minimumQueryVersion),
      termCreated(currentTerm) {
    if (statusWithCollectionAndChangedChunks.isOK()) {
        collectionAndChangedChunks = statusWithCollectionAndChangedChunks.getValue();
        invariant(!collectionAndChangedChunks->changedChunks.empty());
        maxQueryVersion = collectionAndChangedChunks->changedChunks.back().getVersion();
    } else {
        invariant(statusWithCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound);
        dropped = true;
        maxQueryVersion = ChunkVersion::UNSHARDED();
    }
}

ShardServerCatalogCacheLoader::TaskList::TaskList()
    : _activeTaskCompletedCondVar(std::make_shared<stdx::condition_variable>()) {}

void ShardServerCatalogCacheLoader::TaskList::addTask(Task task) {
    if (_tasks.empty()) {
        _tasks.emplace_back(std::move(task));
        return;
    }

    if (task.dropped) {
        invariant(_tasks.back().maxQueryVersion.equals(task.minQueryVersion));

        // As an optimization, on collection drop, clear any pending tasks in order to prevent any
        // throw-away work from executing. Because we have no way to differentiate whether the
        // active tasks is currently being operated on by a thread or not, we must leave the front
        // intact.
        _tasks.erase(std::next(_tasks.begin()), _tasks.end());

        // No need to schedule a drop if one is already currently active.
        if (!_tasks.front().dropped) {
            _tasks.emplace_back(std::move(task));
        }
    } else {
        // Tasks must have contiguous versions, unless a complete reload occurs.
        invariant(_tasks.back().maxQueryVersion.equals(task.minQueryVersion) ||
                  !task.minQueryVersion.isSet());

        _tasks.emplace_back(std::move(task));
    }
}

void ShardServerCatalogCacheLoader::TaskList::pop_front() {
    invariant(!_tasks.empty());
    _tasks.pop_front();
    _activeTaskCompletedCondVar->notify_all();
}

void ShardServerCatalogCacheLoader::TaskList::waitForActiveTaskCompletion(
    stdx::unique_lock<stdx::mutex>& lg) {
    // Increase the use_count of the condition variable shared pointer, because the entire task list
    // might get deleted during the unlocked interval
    auto condVar = _activeTaskCompletedCondVar;
    condVar->wait(lg);
}

bool ShardServerCatalogCacheLoader::TaskList::hasTasksFromThisTerm(long long term) const {
    invariant(!_tasks.empty());
    return _tasks.back().termCreated == term;
}

ChunkVersion ShardServerCatalogCacheLoader::TaskList::getHighestVersionEnqueued() const {
    invariant(!_tasks.empty());
    return _tasks.back().maxQueryVersion;
}

CollectionAndChangedChunks ShardServerCatalogCacheLoader::TaskList::getEnqueuedMetadataForTerm(
    const long long term) const {
    CollectionAndChangedChunks collAndChunks;
    for (const auto& task : _tasks) {
        if (task.termCreated != term) {
            // Task data is no longer valid. Go on to the next task in the list.
            continue;
        }

        if (task.dropped) {
            // A drop task should reset the metadata.
            collAndChunks = CollectionAndChangedChunks();
        } else {
            if (task.collectionAndChangedChunks->epoch != collAndChunks.epoch) {
                // An epoch change should reset the metadata and start from the new.
                collAndChunks = task.collectionAndChangedChunks.get();
            } else {
                // Epochs match, so the new results should be appended.

                // Make sure we do not append a duplicate chunk. The diff query is GTE, so there can
                // be duplicates of the same exact versioned chunk across tasks. This is no problem
                // for our diff application algorithms, but it can return unpredictable numbers of
                // chunks for testing purposes. Eliminate unpredicatable duplicates for testing
                // stability.
                auto taskCollectionAndChangedChunksIt =
                    task.collectionAndChangedChunks->changedChunks.begin();
                if (collAndChunks.changedChunks.back().getVersion() ==
                    task.collectionAndChangedChunks->changedChunks.front().getVersion()) {
                    ++taskCollectionAndChangedChunksIt;
                }

                collAndChunks.changedChunks.insert(
                    collAndChunks.changedChunks.end(),
                    taskCollectionAndChangedChunksIt,
                    task.collectionAndChangedChunks->changedChunks.end());
            }
        }
    }
    return collAndChunks;
}

}  // namespace mongo
