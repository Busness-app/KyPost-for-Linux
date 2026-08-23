#pragma once

#include "net/HttpClient.h"
#include "net/NetworkError.h"

#include <QString>
#include <QUrl>
#include <QVector>
#include <optional>

struct RelayAuth;

// One recipient's answer from the relay's key-discovery ladder.
struct ResolvedRecipientKey
{
    QString address;
    QString publicKey;   // ASCII-armored; empty when the ladder found nothing
    QString fingerprint; // the relay's claim about the key above
    // Where the key came from: a pinned contact, WKD, or a keyserver. Carried
    // so the compose screen can say how well-known a recipient's key is
    // instead of presenting every source as equally settled.
    QString tier;
    // The ONLY gate. A key can be present and unusable -- revoked, expired,
    // wrong capability -- and the relay folds all of that in here.
    bool usable = false;

    bool operator==(const ResolvedRecipientKey&) const = default;
};

enum class PgpResolveStatus
{
    Resolved,

    // 409: this account's key is NOT client-protected, so the server encrypts
    // on its own and refuses to hand out recipient keys. The opposite meaning
    // to the 409 on /api/mail/pgp-payload, which is why neither is called
    // "Conflict" here.
    ServerEncryptsInstead,

    // 413: more addresses than the relay will resolve in one request.
    TooManyRecipients,

    Failed,
};

struct PgpResolveResult
{
    PgpResolveStatus status = PgpResolveStatus::Failed;
    std::optional<NetworkError> error;
    QString detail;
    QVector<ResolvedRecipientKey> keys;
};

// Asks the relay for the recipients' actual public keys, for an account whose
// own key this device holds and the server does not.
//
// WHY THIS IS THE RELAY'S JOB. The discovery ladder behind it -- pinned
// contact key, then WKD, then keyserver, honouring the user's own discovery
// settings and suppressions -- exists once, on the server. Reimplementing it
// here would be a second and weaker copy of rules that decide which key a
// message is encrypted to.
//
// WHAT THAT COSTS, stated rather than glossed: on this one account type the
// server cannot read the mail, so a relay that returns the wrong public key
// can read outgoing mail it otherwise could not. Importing these keys into the
// user's own GnuPG keyring (see core/pgp/OpenPgpKeyImporter.h) is the answer
// chosen for that -- gpg then owns the record, and a key that changes under a
// recipient is visible in the user's own tooling rather than only inside this
// app.
//
// There is deliberately NO fallback. An address with no usable key comes back
// unusable and the caller must refuse to send, never quietly downgrade to
// plaintext -- the server's own pickup-link fallback works by storing the
// plaintext on the relay, which is the thing client-side protection exists to
// prevent.
class PgpResolveClient
{
public:
    explicit PgpResolveClient(HttpClient& httpClient);

    // POST {serverBaseUrl}/api/pgp/recipients/resolve -- body is
    // {"addresses": [...]}.
    PgpResolveResult resolve(const QUrl& serverBaseUrl, const RelayAuth& auth,
                              const QStringList& addresses) const;

private:
    HttpClient& m_httpClient;
};
