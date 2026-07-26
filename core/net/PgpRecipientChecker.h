#pragma once

#include "net/HttpClient.h"
#include "net/NetworkError.h"

#include <QString>
#include <QStringList>
#include <QUrl>
#include <optional>

struct RelayAuth;

// Response from POST {serverBaseUrl}/api/pgp/recipients/check -- the Go
// backend's contacts-only preflight (pgp_keyserver.go:129-151), never the
// /api/pgp/recipients/resolve endpoint: resolve hands real public keys to a
// browser doing its own encryption and refuses with 409 for any account that
// is not client-protected, the one account type this client cannot send
// encrypted mail for at all. check is the only endpoint this class is
// allowed to call.
//
// keylessRecipients is a lower bound, not a prediction: check reads only the
// user's contacts, while the actual send path additionally runs WKD and
// keyserver discovery. An address reported keyless here may still be
// encrypted to successfully at send time -- this list is for an early inline
// warning, not a refusal to send.
//
// ok is false on any transport/HTTP/decode failure, and keylessRecipients is
// always empty in that case, so a failed check never reads as "everyone has
// a key" -- see PgpRecipientCheckerTest::failureIsNotAnEmptyKeylessList.
struct RecipientKeyCheckResult
{
    std::optional<NetworkError> error;
    QString detail; // human-readable detail on error; empty otherwise
    bool ok = false;
    QStringList keylessRecipients;
};

// Asks the relay which of a set of recipient addresses have no usable PGP
// key on file, so compose can show an early inline warning before sending.
// Follows PgpQrClient/PgpBootstrapClient's template (constructor takes
// HttpClient&, one method per endpoint, a small *Result struct per call).
// Never holds the account's private key and does no OpenPGP itself.
class PgpRecipientChecker
{
public:
    explicit PgpRecipientChecker(HttpClient& httpClient);

    // POST {serverBaseUrl}/api/pgp/recipients/check -- body is
    // {"addresses": [...]}. hasKey already folds revoked/expired in
    // server-side (HasKey = ks.Usable()), so keyless is derived only from
    // hasKey == false, never re-derived from the revoked/expired flags the
    // response also carries. tier exists for the web UI's badges and is not
    // modeled here.
    RecipientKeyCheckResult check(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                   const QStringList& addresses) const;

private:
    HttpClient& m_httpClient;
};
