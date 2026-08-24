#include "domain/ContactPhotoRepository.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/RelayAuth.h"
#include "stores/ContactPhotoCache.h"

#include <QUrl>

ContactPhotoRepository::ContactPhotoRepository(ContactPhotoClient& client, ContactPhotoCache& cache,
                                                 PairingStore& pairingStore)
    : m_client(client)
    , m_cache(cache)
    , m_pairingStore(pairingStore)
{
}

QString ContactPhotoRepository::cachedPathFor(const QString& photoRef) const
{
    if (photoRef.isEmpty())
        return QString();
    return m_cache.cachedPathFor(photoRef);
}

std::optional<RelayRequestPlan> ContactPhotoRepository::planFetch() const
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return std::nullopt;
    return RelayRequestPlan{ RelayEndpoint{ QUrl(pairing->serverBaseUrl),
                                             RelayAuth{ pairing->deviceId, pairing->deviceSecret } },
                              identityOf(*pairing) };
}

ContactPhotoFetchResult ContactPhotoRepository::fetchWith(HttpClient& httpClient,
                                                           const RelayEndpoint& endpoint,
                                                           const QString& contactUid)
{
    ContactPhotoClient client(httpClient);
    return client.fetch(endpoint.serverBaseUrl, contactUid, endpoint.auth);
}

QString ContactPhotoRepository::applyFetch(const RelayRequestPlan& plan, const QString& photoRef,
                                            const ContactPhotoFetchResult& result) const
{
    if (result.error.has_value() || result.photoBytes.isEmpty())
        return QString(); // degrade gracefully

    if (!m_pairingStore.stillCurrent(plan.identity))
        return QString();

    return m_cache.store(photoRef, result.photoBytes);
}

QString ContactPhotoRepository::photoPathFor(const QString& contactUid, const QString& photoRef) const
{
    if (photoRef.isEmpty())
        return QString();

    const QString cached = cachedPathFor(photoRef);
    if (!cached.isEmpty())
        return cached;

    const std::optional<RelayRequestPlan> plan = planFetch();
    if (!plan.has_value())
        return QString(); // not paired -- degrade gracefully, no crash

    // The identity is captured in the plan BEFORE the fetch, which blocks this
    // thread on a nested event loop -- so a re-pair can be delivered during it
    // and applyFetch() can tell.
    const ContactPhotoFetchResult result =
        m_client.fetch(plan->endpoint.serverBaseUrl, contactUid, plan->endpoint.auth);
    return applyFetch(*plan, photoRef, result);
}
