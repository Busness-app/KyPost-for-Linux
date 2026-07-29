#pragma once

#include "net/RelayAuth.h"

#include <QString>
#include <QUrl>
#include <optional>
#include <utility>

class HttpClient;
class PgpQrClient;
class PairingStore;
struct RelayAuth;

enum class PgpQrTokenStatus { Success, NotPaired, NoPgpIdentity, Unauthorized, ServiceUnavailable, Retry };

struct PgpQrTokenOutcome
{
    PgpQrTokenStatus status = PgpQrTokenStatus::Retry;
    QString token;      // meaningful only when status == Success
    QString expiresAt;  // meaningful only when status == Success
    QString url;        // meaningful only when status == Success
    QString detail;     // meaningful when status != Success
};

// Wraps only the "My QR Code" token-fetch side of PGP QR exchange -- the
// only half that needs this device's own pairing (RelayAuth/serverBaseUrl)
// resolved, same as GroupsRepository::refresh(). The key-fetch side
// (scanning someone else's code) needs no pairing at all -- the token in
// the scanned URL is the sole credential, and the scan target may be a
// different server than this device's own paired one -- so
// PgpQrController calls PgpQrClient::fetchKey() directly instead of going
// through a repository method here (mirrors MailController's existing
// "holds both a repository and a raw client" composition).
class PgpQrRepository
{
public:
    PgpQrRepository(PgpQrClient& client, PairingStore& pairingStore);

    PgpQrTokenOutcome fetchMyToken();

    // The half of fetchMyToken() that may run off the calling thread: the
    // request and the pure status mapping, with the pairing already resolved
    // into plain values by the caller.
    //
    // Static and taking an HttpClient because PairingStore must NOT be
    // reached from another thread -- it caches, and the credential gate
    // mutates it. See docs/THREADING.md; this is the same prepare/send split
    // DeviceRegistrationService needed, minus the persistence.
    static PgpQrTokenOutcome fetchTokenWith(HttpClient& httpClient, const QUrl& serverBaseUrl,
                                             const RelayAuth& auth);

    // Resolves the pairing on the CALLING thread. Returns nullopt when not
    // paired, which the caller reports without any request.
    std::optional<std::pair<QUrl, RelayAuth>> resolvePairing() const;

private:
    PgpQrClient& m_client;
    PairingStore& m_pairingStore;
};
