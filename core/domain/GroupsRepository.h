#pragma once

#include "domain/RelayRequestPlan.h"
#include "net/GroupsClient.h" // GroupsFetchResult -- crosses the thread hop by value
#include "net/RelayAuth.h"    // RelayEndpoint

#include <QVector>
#include <optional>

class GroupDao;
class HttpClient;
class PairingStore;
struct Group;

// Refreshes GroupDao's local name-cache from GroupsClient, once per contact
// sync cycle. Deliberately a separate class from ContactSyncRepository
// (rather than adding GroupsClient/GroupDao as two more constructor
// dependencies there) so ContactSyncRepository -- and its large existing
// test suite, none of which expects a second outbound HTTP call per sync()
// -- stays untouched by this feature; see task-2-report.md for the
// tradeoff. ContactsController::sync() calls refresh() alongside (after a
// successful) ContactSyncRepository::sync(), duplicating the small
// pairing -> RelayAuth/serverUrl computation ContactSyncRepository::sync()
// already does internally, rather than threading it through.
//
// Small full-replace cache, no delta cursor -- matches the "small list,
// full replace" simplicity task-2-brief.md calls for (same call the source
// doc makes for Android's own GroupEntity cache).
class GroupsRepository
{
public:
    GroupsRepository(GroupsClient& client, GroupDao& groupDao, PairingStore& pairingStore);

    QVector<Group> groups() const; // groupDao.findAll()

    // No-op when not paired. On any fetch error (401, transport failure,
    // decode failure, ...) this gives up and leaves the existing cache
    // untouched -- never crashes -- matching this feature's "GroupsClient
    // must degrade gracefully on 401/error" Global Constraint. The next sync
    // cycle simply retries; no delta cursor means a retry is always a full,
    // correct refresh.
    //
    // Returns whether the cache now matches a response: false for not
    // paired, a fetch error, a re-pair mid-flight, OR a failed cache write.
    // That last one used to be unreportable -- the DAO's result was dropped
    // -- so a half-written name-cache and a correct one looked identical from
    // here. Callers are not required to show it to the user (a stale name
    // cache is not data loss) but they ARE required to stop calling it a
    // refresh that happened.
    bool refresh();

    // Three-phase form, same shape as every other repository here. Phase 1 is
    // just the pairing read; there is no cursor, but the plan does carry the
    // identity that authorised the request so phase 3 can tell whether the
    // reply still belongs here.
    std::optional<RelayRequestPlan> planRefresh() const;
    static GroupsFetchResult fetchWith(HttpClient& httpClient, const RelayEndpoint& endpoint);

    // Writes nothing when the device has been re-paired since the request went
    // out. Group names are the previous account's data, the group table has no
    // subscriber column, and pairing a replacement account has just emptied it
    // -- see PairingIdentity in DevicePairing.h.
    bool applyRefresh(const RelayRequestPlan& plan, const GroupsFetchResult& result);

private:
    GroupsClient& m_client;
    GroupDao& m_groupDao;
    PairingStore& m_pairingStore;
};
