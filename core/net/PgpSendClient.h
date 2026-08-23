#pragma once

#include "net/HttpClient.h"
#include "net/NetworkError.h"
#include "pgp/PgpSendPlanner.h"

#include <QString>
#include <QUrl>
#include <optional>

struct RelayAuth;

struct PgpSendResult
{
    std::optional<NetworkError> error;
    QString detail; // for a log line; never user-facing wording
    bool ok = false;

    // Whether the relay filed a copy in Sent. False is not a failed send --
    // the mail went out either way -- but the user's outbox will not have it,
    // and they have to be told.
    bool sentSaved = false;

    // The relay flagged something about a send that otherwise succeeded: some
    // Bcc deliveries failed, or the Sent copy was refused.
    //
    // A BOOLEAN, not the sentence. The relay reports the detail only as prose
    // ("2 bcc delivery(s) failed"), and prose from the relay is not something
    // to print to the user on a path whose whole point is that the relay is
    // not trusted with this message. So this client can say that something
    // needs attention and cannot say how many -- which is an honest
    // limitation of the endpoint, not one to paper over by rendering its text.
    bool warned = false;
    QString warningDetail; // logs only
};

// Hands the relay a set of ciphertexts to deliver, for an account whose PGP
// key it does not hold.
//
// POST {serverBaseUrl}/api/mail/send-pgp
//
// The relay's role here is reduced to an SMTP relay for its own user: it holds
// the mailbox credentials this client must not, and none of the key material
// it would need to produce or inspect these ciphertexts.
//
// A 200 does NOT mean every recipient received it. Bcc deliveries are sent one
// at a time and a failure among them is reported as a warning on an otherwise
// successful response -- see `warned`.
//
// The subject sent is the placeholder, never the real one. The relay's own
// struct comment says the field is accepted and ignored and that no client
// should start reading it; the real subject is inside the ciphertext as a
// protected header, which is the point.
class PgpSendClient
{
public:
    explicit PgpSendClient(HttpClient& httpClient);

    // `to`/`cc`/`bcc` are the plaintext address lists. They are NOT the SMTP
    // envelope -- that comes from each delivery's own recipients -- and the
    // relay uses them only for the Sent-folder copy's headers. They are sent
    // because the Sent copy needs them; nothing else about the send depends on
    // them.
    PgpSendResult send(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& from,
                        const PgpSendPlan& plan, const QStringList& to, const QStringList& cc,
                        const QStringList& bcc, const QString& mode) const;

private:
    HttpClient& m_httpClient;
};
