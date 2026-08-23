#include "domain/GroupsRepository.h"

#include "db/GroupDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "models/Group.h"
#include "net/GroupsClient.h"
#include "net/RelayAuth.h"

#include <QUrl>

GroupsRepository::GroupsRepository(GroupsClient& client, GroupDao& groupDao, PairingStore& pairingStore)
    : m_client(client)
    , m_groupDao(groupDao)
    , m_pairingStore(pairingStore)
{
}

QVector<Group> GroupsRepository::groups() const
{
    return m_groupDao.findAll();
}

std::optional<RelayRequestPlan> GroupsRepository::planRefresh() const
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return std::nullopt;
    return RelayRequestPlan{ RelayEndpoint{ QUrl(pairing->serverBaseUrl),
                                             RelayAuth{ pairing->deviceId, pairing->deviceSecret } },
                              identityOf(*pairing) };
}

GroupsFetchResult GroupsRepository::fetchWith(HttpClient& httpClient, const RelayEndpoint& endpoint)
{
    GroupsClient client(httpClient);
    return client.fetch(endpoint.serverBaseUrl, endpoint.auth);
}

bool GroupsRepository::applyRefresh(const RelayRequestPlan& plan, const GroupsFetchResult& result)
{
    if (result.error.has_value())
        return false; // degrade gracefully -- next sync cycle retries, no crash

    // Same give-up as an error, and for a stronger reason: these rows belong
    // to the account that asked for them, not to whatever account is paired
    // now.
    if (!m_pairingStore.stillCurrent(plan.identity))
        return false;

    // One transactional replace, not an upsert loop: /api/groups answers with
    // the whole list, so a group the user deleted on another device is
    // visible only as an absence from it -- and an upsert loop that half
    // applied left the name-cache in a state no response describes.
    return m_groupDao.replaceSnapshot(result.groups);
}

// The synchronous form, kept as the composition of the three phases above.
bool GroupsRepository::refresh()
{
    const std::optional<RelayRequestPlan> plan = planRefresh();
    if (!plan.has_value())
        return false;
    return applyRefresh(*plan, m_client.fetch(plan->endpoint.serverBaseUrl, plan->endpoint.auth));
}
