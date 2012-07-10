/* Copyright (c) 2009-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "CoordinatorServerManager.h"
#include "CoordinatorService.h"
#include "MembershipClient.h"
#include "PingClient.h"
#include "ShortMacros.h"

namespace RAMCloud {

CoordinatorServerManager::CoordinatorServerManager(
        CoordinatorService& coordinatorService)
    : service(coordinatorService)
    , nextReplicationId(1)
    , forceServerDownForTesting(false)
{
}

CoordinatorServerManager::~CoordinatorServerManager()
{
}

/**
 * Assign a new replicationId to a backup, and inform the backup which nodes
 * are in its replication group.
 *
 * \param replicationId
 *      New replication group Id that is assigned to backup.
 *
 * \param replicationGroupIds
 *      Includes the ServerId's of all the members of the replication group.
 *
 * \return
 *      False if one of the servers is dead, true if all of them are alive.
 */
bool
CoordinatorServerManager::assignReplicationGroup(
    uint64_t replicationId, const vector<ServerId>& replicationGroupIds)
{
    foreach (ServerId backupId, replicationGroupIds) {
        if (!service.serverList.contains(backupId)) {
            return false;
        }
        service.serverList.setReplicationId(backupId, replicationId);
        // Try to send an assignReplicationId Rpc to a backup. If the Rpc
        // fails, hintServerDown. If hintServerDown is true, the function
        // aborts. If it fails, keep trying to send the Rpc to the backup.
        // Note that this is an optimization. Even if we didn't abort in case
        // of a failed Rpc, masters would still not use the failed replication
        // group, since it would not accept their Rpcs.
        while (true) {
            try {
                const char* locator =
                    service.serverList[backupId].serviceLocator.c_str();
                BackupClient backupClient(
                    service.context.transportManager->getSession(locator));
                backupClient.assignGroup(
                    replicationId,
                    static_cast<uint32_t>(replicationGroupIds.size()),
                    &replicationGroupIds[0]);
            } catch (TransportException& e) {
                if (hintServerDown(backupId)) {
                    return false;
                } else {
                    continue;
                }
            } catch (TimeoutException& e) {
                if (hintServerDown(backupId)) {
                    return false;
                } else {
                    continue;
                }
            }
            break;
        }
    }
    return true;
}

/**
 * Try to create a new replication group. Look for groups of backups that
 * are not assigned a replication group and are up.
 * If there are not enough available candidates for a new group, the function
 * returns without sending out any Rpcs. If there are enough group members
 * to form a new group, but one of the servers is down, hintServerDown will
 * reset the replication group of that server.
 */
void
CoordinatorServerManager::createReplicationGroup()
{
    // Create a list of all servers who do not belong to a replication group
    // and are up. Note that this is a performance optimization and is not
    // required for correctness.
    vector<ServerId> freeBackups;
    for (size_t i = 0; i < service.serverList.size(); i++) {
        if (service.serverList[i] &&
            service.serverList[i]->isBackup() &&
            service.serverList[i]->replicationId == 0) {
            freeBackups.push_back(service.serverList[i]->serverId);
        }
    }

    // TODO(cidon): The coordinator currently has no knowledge of the
    // replication factor, so we manually set the replication group size to 3.
    // We should make this parameter configurable.
    const uint32_t numReplicas = 3;
    vector<ServerId> group;
    while (freeBackups.size() >= numReplicas) {
        group.clear();
        // Update the replicationId on serverList.
        for (uint32_t i = 0; i < numReplicas; i++) {
            const ServerId& backupId = freeBackups.back();
            group.push_back(backupId);
            service.serverList.setReplicationId(backupId, nextReplicationId);
            freeBackups.pop_back();
        }
        // Assign a new replication group. AssignReplicationGroup handles
        // Rpc failures.
        assignReplicationGroup(nextReplicationId, group);
        nextReplicationId++;
    }
}

/**
 * Implements the first part to handle enlistServer.
 *
 * \param replacesId
 *      Server id of the server that the enlisting server is replacing.
 * \param replacedEntry
 *      Keeps track of the details of the server id that is being forced out
 *      of the cluster by the enlister so we can start recovery.
 * \param serviceMask
 *      Services supported by the enlisting server.
 * \param readSpeed
 *      Read speed of the enlisting server.
 * \param writeSpeed
 *      Write speed of the enlisting server.
 * \param serviceLocator
 *      Service Locator of the enlisting server.
 * \param serverListUpdate
 *      Keeps track of the server list updates to be sent to the cluster.
 *
 * \return
 *      Server id assigned to the enlisting server.
 */
ServerId
CoordinatorServerManager::enlistServerStart(
    ServerId replacesId, Tub<CoordinatorServerList::Entry>* replacedEntry,
    ServiceMask serviceMask,
    const uint32_t readSpeed, const uint32_t writeSpeed,
    const char* serviceLocator, ProtoBuf::ServerList* serverListUpdate)
{
    // The order of the updates in serverListUpdate is important: the remove
    // must be ordered before the add to ensure that as members apply the
    // update they will see the removal of the old server id before the
    // addition of the new, replacing server id.
    if (service.serverList.contains(replacesId)) {
        LOG(NOTICE, "%s is enlisting claiming to replace server id "
            "%lu, which is still in the server list, taking its word "
            "for it and assuming the old server has failed",
            serviceLocator, replacesId.getId());
        replacedEntry->construct(service.serverList[replacesId]);
        // Don't increment server list yet; done after the add below.
        // Note, if the server being replaced is already crashed this may
        // not append an update at all.
        service.serverList.crashed(replacesId, *serverListUpdate);
        // If the server being replaced did not have a master then there
        // will be no recovery.  That means it needs to transition to
        // removed status now (usually recoveries remove servers from the
        // list when they complete).
        if (!replacedEntry->get()->isMaster())
            service.serverList.remove(replacesId, *serverListUpdate);
    }

    ServerId newServerId = service.serverList.add(serviceLocator,
                                                   serviceMask,
                                                   readSpeed,
                                                   *serverListUpdate);
    service.serverList.incrementVersion(*serverListUpdate);
    CoordinatorServerList::Entry entry = service.serverList[newServerId];

    LOG(NOTICE, "Enlisting new server at %s (server id %lu) supporting "
        "services: %s",
        serviceLocator, newServerId.getId(),
        entry.services.toString().c_str());
    if (replacesId.isValid()) {
        LOG(NOTICE, "Newly enlisted server %lu replaces server %lu",
            newServerId.getId(), replacesId.getId());
    }

    if (entry.isBackup()) {
        LOG(DEBUG, "Backup at id %lu has %u MB/s read %u MB/s write",
            newServerId.getId(), readSpeed, writeSpeed);
        createReplicationGroup();
    }

    return newServerId;
}

/**
 * Implements the second part to handle enlistServer.
 *
 * \param replacedEntry
 *      Details of the server id that is being forced out
 *      of the cluster by the enlister so we can start recovery.
 * \param newServerId
 *      Server id of the enlisting server.
 * \param serverListUpdate
 *      The server list updates to be sent to the cluster.
 */
void
CoordinatorServerManager::enlistServerComplete(
    Tub<CoordinatorServerList::Entry>* replacedEntry,
    ServerId newServerId, ProtoBuf::ServerList* serverListUpdate)
{
    CoordinatorServerList::Entry entry = service.serverList[newServerId];

    if (entry.services.has(MEMBERSHIP_SERVICE))
        sendServerList(newServerId);
    service.serverList.sendMembershipUpdate(*serverListUpdate, newServerId);

    // Recovery on the replaced host is deferred until the replacing host
    // has been enlisted.
    if (*replacedEntry)
        service.recoveryManager.startMasterRecovery(
            replacedEntry->get()->serverId);
}

/**
 * Return the serialized server list filtered by the serviceMask.
 *
 * \param serviceMask
 *      Return servers that support this service.
 * \return
 *      Serialized server list filtered by the serviceMask.
 */
ProtoBuf::ServerList
CoordinatorServerManager::getServerList(ServiceMask serviceMask)
{
    ProtoBuf::ServerList serialServerList;
    service.serverList.serialize(serialServerList, serviceMask);
    return serialServerList;
}

/**
 * Returns true if server is down, false otherwise.
 *
 * \param serverId
 *      ServerId of the server that is suspected to be down.
 * \return
 *      True if server is down, false otherwise.
 */
bool
CoordinatorServerManager::hintServerDown(ServerId serverId)
{
    if (!service.serverList.contains(serverId) ||
        service.serverList[serverId].status != ServerStatus::UP) {
        LOG(NOTICE, "Spurious crash report on unknown server id %lu",
            serverId.getId());
        return true;
    }

    if (!verifyServerFailure(serverId))
        return false;
    LOG(NOTICE, "Server id %lu has crashed, notifying the cluster and "
        "starting recovery", serverId.getId());

    // If this machine has a backup and master on the same server it is best
    // to remove the dead backup before initiating recovery. Otherwise, other
    // servers may try to backup onto a dead machine which will cause delays.
    CoordinatorServerList::Entry entry = service.serverList[serverId];
    ProtoBuf::ServerList update;
    service.serverList.crashed(serverId, update);
    // If the server being replaced did not have a master then there
    // will be no recovery.  That means it needs to transition to
    // removed status now (usually recoveries remove servers from the
    // list when they complete).
    if (!entry.isMaster())
        service.serverList.remove(serverId, update);
    service.serverList.incrementVersion(update);

    // Update cluster membership information.
    // Backup recovery is kicked off via this update.
    // Deciding whether to place this before or after the start of master
    // recovery is difficult.
    service.serverList.sendMembershipUpdate(
            update, ServerId(/* invalid id */));

    service.recoveryManager.startMasterRecovery(entry.serverId);

    removeReplicationGroup(entry.replicationId);
    createReplicationGroup();

    return true;
}

/**
 * Reset the replicationId for all backups with groupId.
 *
 * \param groupId
 *      Replication group that needs to be reset.
 */
void
CoordinatorServerManager::removeReplicationGroup(uint64_t groupId)
{
    // Cannot remove groupId 0, since it is the default groupId.
    if (groupId == 0) {
        return;
    }
    for (size_t i = 0; i < service.serverList.size(); i++) {
        if (service.serverList[i] &&
            service.serverList[i]->replicationId == groupId) {
            vector<ServerId> group;
            group.push_back(service.serverList[i]->serverId);
            service.serverList.setReplicationId(
                    service.serverList[i]->serverId, 0);
            // We check whether the server is up, in order to prevent
            // recursively calling removeReplicationGroup by hintServerDown.
            // If the backup is still up, we tell it to reset its
            // replicationId, so it will stop accepting Rpcs. This is an
            // optimization; even if we didn't reset its replicationId, Rpcs
            // sent to the server's group members would fail, because at least
            // one of the servers in the group is down.
            if (service.serverList[i]->isBackup()) {
                assignReplicationGroup(0, group);
            }
        }
    }
}

/**
 * Push the entire server list to the specified server. This is used to both
 * push the initial list when a server enlists, as well as to push the list
 * again if a server misses any updates and has gone out of sync.
 *
 * \param serverId
 *      ServerId of the server to send the list to.
 */
void
CoordinatorServerManager::sendServerList(ServerId serverId)
{
    if (!service.serverList.contains(serverId)) {
        LOG(WARNING, "Could not send list to unknown server %lu", *serverId);
        return;
    }

    if (service.serverList[serverId].status != ServerStatus::UP) {
        LOG(WARNING, "Could not send list to crashed server %lu", *serverId);
        return;
    }

    if (!service.serverList[serverId].services.has(MEMBERSHIP_SERVICE)) {
        LOG(WARNING, "Could not send list to server without membership "
            "service: %lu", *serverId);
        return;
    }

    LOG(DEBUG, "Sending server list to server id %lu as requested", *serverId);

    ProtoBuf::ServerList serializedServerList;
    service.serverList.serialize(serializedServerList);

    MembershipClient client(service.context);
    client.setServerList(
            service.serverList[serverId].serviceLocator.c_str(),
            serializedServerList);
}

/**
 * Set the minOpenSegmentId of the specified server to the specified segmentId.
 *
 * \param serverId
 *      ServerId of the server whose minOpenSegmentId will be set.
 * \param segmentId
 *      The minOpenSegmentId to be set.
 */
void
CoordinatorServerManager::setMinOpenSegmentId(ServerId serverId,
                                              uint64_t segmentId)
{
    service.serverList.setMinOpenSegmentId(serverId, segmentId);
}

/**
 * Investigate \a serverId and make a verdict about its whether it is alive.
 *
 * \param serverId
 *      Server to investigate.
 * \return
 *      True if the server is dead, false if it is alive.
 * \throw Exception
 *      If \a serverId is not in #serverList.
 */
bool
CoordinatorServerManager::verifyServerFailure(ServerId serverId) {
    // Skip the real ping if this is from a unit test
    if (forceServerDownForTesting)
        return true;

    const string& serviceLocator = service.serverList[serverId].serviceLocator;
    try {
        uint64_t nonce = generateRandom();
        PingClient pingClient(service.context);
        pingClient.ping(serviceLocator.c_str(), ServerId(),
                        nonce, TIMEOUT_USECS * 1000);
        LOG(NOTICE, "False positive for server id %lu (\"%s\")",
                    *serverId, serviceLocator.c_str());
        return false;
    } catch (TransportException& e) {
    } catch (TimeoutException& e) {
    }
    LOG(NOTICE, "Verified host failure: id %lu (\"%s\")",
        *serverId, serviceLocator.c_str());

    return true;
}

} // namespace RAMCloud
