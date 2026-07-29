#include "domain/PgpQrRepository.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/NetworkError.h"
#include "net/HttpClient.h"
#include "net/PgpQrClient.h"
#include "net/RelayAuth.h"

#include <QUrl>

namespace {

// 400 ("no pgp identity configured") isn't in NetworkError's own switch (see
// core/net/NetworkError.cpp), so it collapses to NetworkError::Server --
// distinguish it here via the raw statusCode PgpQrClient carries alongside
// the generic error, rather than adding a case to the shared enum every
// other client's exhaustive switch would then have to account for.
PgpQrTokenStatus tokenStatusFrom(const PgpQrTokenResult& result)
{
    if (result.statusCode == 400)
        return PgpQrTokenStatus::NoPgpIdentity;
    if (!result.error.has_value())
        return PgpQrTokenStatus::Success;
    switch (*result.error) {
    case NetworkError::Unauthorized:
        return PgpQrTokenStatus::Unauthorized;
    case NetworkError::ServiceUnavailable:
        return PgpQrTokenStatus::ServiceUnavailable;
    default:
        return PgpQrTokenStatus::Retry;
    }
}

} // namespace

PgpQrRepository::PgpQrRepository(PgpQrClient& client, PairingStore& pairingStore)
    : m_client(client)
    , m_pairingStore(pairingStore)
{
}

std::optional<std::pair<QUrl, RelayAuth>> PgpQrRepository::resolvePairing() const
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return std::nullopt;
    return std::make_pair(QUrl(pairing->serverBaseUrl),
                          RelayAuth{ pairing->deviceId, pairing->deviceSecret });
}

PgpQrTokenOutcome PgpQrRepository::fetchTokenWith(HttpClient& httpClient, const QUrl& serverBaseUrl,
                                                   const RelayAuth& auth)
{
    // Constructed here rather than borrowed: PgpQrClient is a stateless
    // wrapper over an HttpClient reference, and on the async path that
    // client belongs to the executor thread.
    PgpQrClient client(httpClient);
    const PgpQrTokenResult result = client.fetchToken(serverBaseUrl, auth);
    const PgpQrTokenStatus status = tokenStatusFrom(result);

    if (status != PgpQrTokenStatus::Success)
        return { status, {}, {}, {}, result.detail };

    return { PgpQrTokenStatus::Success, result.token, result.expiresAt, result.url, QString() };
}

// The synchronous form, kept as the composition of the two halves so the
// existing tests still pin the policy and the async path cannot drift.
PgpQrTokenOutcome PgpQrRepository::fetchMyToken()
{
    const std::optional<std::pair<QUrl, RelayAuth>> pairing = resolvePairing();
    if (!pairing.has_value())
        return { PgpQrTokenStatus::NotPaired, {}, {}, {}, QStringLiteral("Not paired") };

    const PgpQrTokenResult result = m_client.fetchToken(pairing->first, pairing->second);
    const PgpQrTokenStatus status = tokenStatusFrom(result);

    if (status != PgpQrTokenStatus::Success)
        return { status, {}, {}, {}, result.detail };

    return { PgpQrTokenStatus::Success, result.token, result.expiresAt, result.url, QString() };
}
