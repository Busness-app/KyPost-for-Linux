#pragma once

#include "net/HttpClient.h"
#include "net/NetworkError.h"

#include <QString>
#include <QUrl>
#include <optional>

struct RelayAuth;

// Response from GET {serverBaseUrl}/api/pgp/bootstrap -- the Go backend's
// PGP bootstrap handler. The real response also carries wrappedPrivateKey,
// unlockRequired, signerPublicKeys, payloadEndpoint, fingerprint, keyId,
// publicKey, keySource and createdAt, but those exist for the browser (which
// does the actual OpenPGP work); this client never holds the account's
// private key and does no OpenPGP itself, so only hasIdentity and protection
// are parsed. ok is false on any transport/HTTP/decode failure so a caller
// can tell "couldn't check" apart from a genuine "no identity" -- see
// PgpBootstrapClientTest::failureIsNotAnEmptySuccess.
struct PgpBootstrapResult
{
    std::optional<NetworkError> error;
    QString detail; // human-readable detail on error; empty otherwise
    bool ok = false;
    bool hasIdentity = false;
    QString protection;
};

// Talks to the account's PGP bootstrap endpoint. Follows PgpQrClient's
// template (constructor takes HttpClient&, one method per endpoint, a small
// *Result struct per call) rather than anything new.
class PgpBootstrapClient
{
public:
    explicit PgpBootstrapClient(HttpClient& httpClient);

    // GET {serverBaseUrl}/api/pgp/bootstrap -- same pairing-header auth shape
    // as PgpQrClient::fetchToken.
    PgpBootstrapResult fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const;

private:
    HttpClient& m_httpClient;
};
