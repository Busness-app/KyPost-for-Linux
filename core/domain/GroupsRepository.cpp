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

std::optional<RelayEndpoint> GroupsRepository::planRefresh() const
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return std::nullopt;
    return RelayEndpoint{ QUrl(pairing->serverBaseUrl),
                          RelayAuth{ pairing->deviceId, pairing->deviceSecret } };
}

GroupsFetchResult GroupsRepository::fetchWith(HttpClient& httpClient, const RelayEndpoint& endpoint)
{
    GroupsClient client(httpClient);
    return client.fetch(endpoint.serverBaseUrl, endpoint.auth);
}

void GroupsRepository::applyRefresh(const GroupsFetchResult& result)
{
    if (result.error.has_value())
        return; // degrade gracefully -- next sync cycle retries, no crash

    for (const Group& group : result.groups)
        m_groupDao.insertOrReplace(group);
}

// The synchronous form, kept as the composition of the three phases above.
void GroupsRepository::refresh()
{
    const std::optional<RelayEndpoint> endpoint = planRefresh();
    if (!endpoint.has_value())
        return;
    applyRefresh(m_client.fetch(endpoint->serverBaseUrl, endpoint->auth));
}
