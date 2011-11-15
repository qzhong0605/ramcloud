/* Copyright (c) 2011 Stanford University
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

#include "BackupSelector.h"
#include "Segment.h"
#include "ShortMacros.h"

namespace RAMCloud {

namespace {
/**
 * Packs and unpacks the user_data field of BackupSelector.hosts.
 * Used in BackupSelector.
 */
struct AbuserData{
  private:
    union X {
        struct {
            /**
             * Disk bandwidth of the host in MB/s
             */
            uint32_t bandwidth;
            /**
             * Number of primary segments this master has stored on the backup.
             */
            uint32_t numSegments;
        };
        /**
         * Raw user_data field.
         */
        uint64_t user_data;
    } x;
  public:
    explicit AbuserData(const ProtoBuf::ServerList::Entry* host)
        : x()
    {
        x.user_data = host->user_data();
    }
    X* operator*() { return &x; }
    X* operator->() { return &x; }
    /**
     * Return the expected number of milliseconds the backup would take to read
     * from its disk all of the primary segments this master has stored on it
     * plus an additional segment.
     */
    uint32_t getMs() {
        // unit tests, etc default to 100 MB/s
        uint32_t bandwidth = x.bandwidth ?: 100;
        if (bandwidth == 1u)
            return 1u;
        return downCast<uint32_t>((x.numSegments + 1) * 1000UL *
                                  Segment::SEGMENT_SIZE /
                                  1024 / 1024 / bandwidth);
    }
};

} // anonymous namespace

// --- BackupSelector ---

/**
 * Constructor.
 * \param coordinator
 *      See #coordinator.
 */
BackupSelector::BackupSelector(CoordinatorClient* coordinator)
    : updateHostListThrower()
    , coordinator(coordinator)
    , hosts()
    , hostsOrder()
    , numUsedHosts(0)
{
}

/**
 * Choose backups for a segment.
 * \param[in] numBackups
 *      The number of backups to choose.
 * \param[out] backups
 *      An array of numBackups entries in which to return the chosen backups.
 *      The first entry should store the primary replica.
 */
void
BackupSelector::select(uint32_t numBackups, Backup* backups[])
{
    if (numBackups == 0)
        return;
    while (hosts.server_size() == 0)
        updateHostListFromCoordinator();

    // Select primary (the least loaded of 5 random backups):
    auto& primary = backups[0];
    primary = getRandomHost();
    for (uint32_t i = 0; i < 5 - 1; ++i) {
        auto candidate = getRandomHost();
        if (AbuserData(primary).getMs() > AbuserData(candidate).getMs())
            primary = candidate;
    }
    AbuserData h(primary);
    LOG(DEBUG, "Chose backup with %u segments and %u MB/s disk bandwidth "
        "(expected time to read on recovery is %u ms)",
        h->numSegments, h->bandwidth, h.getMs());
    ++h->numSegments;
    primary->set_user_data(h->user_data);

    // Select secondaries:
    for (uint32_t i = 1; i < numBackups; ++i)
        backups[i] = selectAdditional(i, backups);
}

/**
 * Choose a random backup that does not conflict with an existing set of
 * backups.
 * \param[in] numBackups
 *      The number of entries in the \a backups array.
 * \param[in] backups
 *      An array of numBackups entries, none of which may conflict with the
 *      returned backup.
 */
BackupSelector::Backup*
BackupSelector::selectAdditional(uint32_t numBackups,
                                 const Backup* const backups[])
{
    while (true) {
        for (uint32_t i = 0; i < uint32_t(hosts.server_size()) * 2; ++i) {
            auto host = getRandomHost();
            if (!conflictWithAny(host, numBackups, backups))
                return host;
        }
        // The constraints must be unsatisfiable with the current backup list.
        LOG(NOTICE, "Current list of backups is insufficient, refreshing");
        updateHostListFromCoordinator();
    }
}

/**
 * Return a random backup.
 * Guaranteed to return all backups at least once after
 * any hosts.server_size() * 2 consecutive calls.
 * \pre
 *      The backup list is not empty.
 * \return
 *      A random backup.
 *
 * Conceptually, the algorithm operates as follows:
 * A set of candidate backups is initialized with the entire list of backups,
 * and a set of used backups starts off empty. With each call to getRandomHost,
 * one backup is chosen from the set of candidates and is moved into the set of
 * used backups. This is the backup that is returned. Once the set of
 * candidates is exhausted, start over.
 *
 * In practice, the algorithm is implemented efficiently:
 * Every index into the list of hosts is stored in the #hostsOrder array. The
 * backups referred to by hostsOrder[0] through hostsOrder[numUsedHosts - 1]
 * make up the set of used hosts, while the backups referred to by the
 * remainder of the array make up the candidate backups.
 */
BackupSelector::Backup*
BackupSelector::getRandomHost()
{
    assert(hosts.server_size() > 0);
    if (numUsedHosts >= hostsOrder.size())
        numUsedHosts = 0;
    uint32_t i = numUsedHosts;
    ++numUsedHosts;
    uint32_t j = i + downCast<uint32_t>(generateRandom() %
                                        (hostsOrder.size() - i));
    std::swap(hostsOrder[i], hostsOrder[j]);
    return hosts.mutable_server(hostsOrder[i]);
}

/**
 * Return whether it is unwise to place a replica on backup 'a' given that a
 * replica exists on backup 'b'. For example, it is unwise to place two
 * replicas on the same backup or on backups that share a common power source.
 */
bool
BackupSelector::conflict(const Backup* a, const Backup* b) const
{
    if (a == b)
        return true;
    // TODO(ongaro): Add other notions of conflicts, such as same rack.
    return false;
}

/**
 * Return whether it is unwise to place a replica on backup 'a' given that
 * replica exists on 'backups'. See #conflict.
 */
bool
BackupSelector::conflictWithAny(const Backup* a,
                                           uint32_t numBackups,
                                           const Backup* const backups[]) const
{
    for (uint32_t i = 0; i < numBackups; ++i) {
        if (conflict(a, backups[i]))
            return true;
    }
    return false;
}

/**
 * Populate the host list by fetching a list of hosts from the coordinator.
 */
void
BackupSelector::updateHostListFromCoordinator()
{
    updateHostListThrower();
    if (!coordinator)
        DIE("No coordinator given, replication requirements can't be met.");
    // TODO(ongaro): This forgets about the number of primaries
    //               stored on each backup.
    coordinator->getBackupList(hosts);

    hostsOrder.clear();
    hostsOrder.reserve(hosts.server_size());
    for (uint32_t i = 0; i < uint32_t(hosts.server_size()); ++i)
        hostsOrder.push_back(i);
    numUsedHosts = 0;
}

} // namespace RAMCloud
