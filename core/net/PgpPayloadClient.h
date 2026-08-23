#pragma once

#include "net/HttpClient.h"
#include "net/NetworkError.h"

#include <QString>
#include <QUrl>
#include <QVector>
#include <optional>

struct RelayAuth;

// Why this endpoint exists at all, since it is not obvious from the client
// side: a client-protected message has NO body on the inbox row. That is the
// whole definition of PgpMessageState::ClientProtected -- encrypted, no
// server-side decrypt error, empty body -- so there is nothing cached to
// decrypt. The backend's own comment on handlePGPPayload gives the reason it
// is a separate route rather than another inbox field: an armored message can
// be megabytes, and the delta path would carry every one of them on every
// poll.
enum class PgpPayloadStatus
{
    // Ciphertext in hand. The only status with a populated encryptedPayload.
    Fetched,

    // There is nothing here to decrypt. Terminal, and deliberately NOT
    // folded into Failed -- a transport error is worth retrying and this is
    // not. Two routes reach it: the server's 404 (the message carries no
    // OpenPGP payload at all), and a 200 whose encryptedPayload is blank,
    // which is the signed-but-not-encrypted case. Named for the absent
    // ciphertext rather than an absent "payload" because the second of those
    // does carry one -- a detached signature -- and this client has no
    // verifier to make anything of it. Saying "no OpenPGP payload" there
    // would be false.
    NoCiphertext,

    // 409: the account still holds its key server-side and has not migrated
    // to client custody. The server refuses ciphertext to such an account,
    // and no longer decrypts for it either, so there is no route to the
    // content from here at all until the migration is completed elsewhere.
    ServerCustody,

    // The message is too large to read on this device. Covers BOTH the
    // server's 413 (it will not hold the message in memory) and this
    // client's own response ceiling (it will not allocate that much). Kept
    // as one status on purpose: the causes differ but the user's position is
    // identical -- retrying cannot change it, and webmail is the way through
    // -- and a distinction the UI cannot act on is one more state to get
    // wrong.
    TooLarge,

    // Transport, auth, decode, or any other status. Retryable as far as this
    // client knows; `error` and `detail` say what happened.
    Failed,
};

// One public key the relay's address book binds to this message's sender.
//
// ALREADY NARROWED. The relay returns only the keys bound to the sender it
// resolved, and its own comment is explicit that this narrowing IS the
// binding: the client no longer parses the From header at all. Re-deriving a
// binding here from the keys' User IDs would be both forgeable (one key, two
// self-asserted User IDs) and parser-dependent, which is the arrangement this
// replaced.
struct PgpSignerKey
{
    QString publicKey; // ASCII-armored; public, nothing secret here
    // The relay saw more than one key claiming this address and cannot say
    // which is right. Offering it anyway would let whichever key verified
    // decide the answer.
    bool conflict = false;

    bool operator==(const PgpSignerKey&) const = default;
};

struct PgpPayloadResult
{
    PgpPayloadStatus status = PgpPayloadStatus::Failed;
    std::optional<NetworkError> error;
    QString detail; // human-readable detail on failure; empty otherwise

    // ASCII-armored OpenPGP message, verbatim. Populated only on Fetched.
    QString encryptedPayload;

    // The keys a signature may be credited to. Empty means no verdict beyond
    // "cannot check" is available.
    QVector<PgpSignerKey> signerKeys;

    // The addr-spec the relay resolved from the From header, and the ONLY
    // identity a verification verdict may be shown against.
    //
    // `sender` -- the display form -- is deliberately absent from this struct.
    // The two are attacker-separable: a From with display name
    // "bob@example.com" and mailbox eve@evil.example renders as
    // `bob@example.com <eve@evil.example>` while the binding correctly uses
    // eve@evil.example. A UI that put a verified badge next to the display
    // form would be crediting text the attacker chose freely. Not parsing it
    // at all is the cheapest way to make that impossible.
    QString resolvedSender;
};

// Fetches one message's raw OpenPGP ciphertext, so this device can decrypt it
// with the user's own key instead of sending them to webmail.
//
// Follows PgpBootstrapClient's template (constructor takes HttpClient&, one
// method per endpoint, a small *Result struct) rather than introducing
// anything new.
//
// signerKeys and resolvedSender ARE parsed, now that there is a verifier to
// bind them (2026-08-23). `sender` -- the display form of the From header --
// still is not, and must not be: see PgpPayloadResult::resolvedSender.
//
// signaturePayload and signedPartBase64 remain unparsed. They serve the
// signed-but-NOT-encrypted case, which this client reports as NoCiphertext
// and has no verifier for; a detached signature needs the exact transmitted
// octets it covers, which is a different problem from this one.
class PgpPayloadClient
{
public:
    explicit PgpPayloadClient(HttpClient& httpClient);

    // GET {serverBaseUrl}/api/mail/pgp-payload?mailbox=&messageId=
    //
    // messageId is an IMAP UID parsed server-side as an integer, but travels
    // as an ordinary query-string value -- same as RelayMailSource's
    // attachment calls, which hit the same parser (attachmentRequestParams).
    PgpPayloadResult fetch(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& mailbox,
                            const QString& messageId) const;

private:
    HttpClient& m_httpClient;
};
