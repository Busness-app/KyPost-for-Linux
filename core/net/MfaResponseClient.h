#pragma once

#include "net/NetworkError.h"

#include <QString>
#include <QUrl>
#include <optional>

class HttpClient;

// Rejected used to cover both "the server refused this device's credentials"
// (401/403) and "this challenge was already resolved" (409), on the theory
// that the caller does the same thing either way. It does not: the two have
// different causes, different fixes, and only one of them is the user's
// fault.
//
// Collapsing them meant a 401 was reported to the user as "This request was
// already handled or denied" -- which is not merely vague, it is false, and
// it is the message a locked device gets EVERY time. With the credential PIN
// gate engaged, PairingStore::load() returns an empty deviceSecret by
// design, so an MFA response sent while locked is guaranteed to 401. The
// user was told their approval had already been dealt with, when in fact
// nothing was sent and unlocking would have fixed it.
enum class MfaResponseOutcome
{
    Success,
    // 401/403 -- the relay refused this device's credentials. Usually means
    // the app is locked with the credential gate on, or the pairing has been
    // revoked server-side. Actionable: unlock, or pair again.
    Unauthorized,
    // 409 -- the challenge is already resolved. Nothing to do.
    Rejected,
    Failure,
};

// status is populated from the response body's "status" field when present
// (always on Success; optionally on a 409 Rejected). detail carries a
// human-readable failure reason and is meaningful only on Failure.
struct MfaResponseResult
{
    MfaResponseOutcome outcome = MfaResponseOutcome::Failure;
    std::optional<QString> status;
    std::optional<QString> detail;
};

// Responds to a push-based MFA challenge via POST {serverBaseUrl}/api/mfa/
// push/respond. Verified against internal/api/push_mfa_handlers.go's
// handlePushRespond — the device authenticates via X-Kypost-Device-Id/
// X-Kypost-Device-Secret headers (RelayAuth), same as every other
// authenticated Relay endpoint; the body carries {challengeId, approve,
// matchDigits}.
//
// matchDigits is the number-match value the server displays in the browser
// that started the sign-in. The server verifies it itself
// (mfa.Store::ResolvePushWithMatch) and REFUSES an approval that does not
// carry it, answering 400; three wrong values spend the challenge's attempt
// budget and it answers 429 thereafter. A deny needs no number and the server
// ignores the field there.
//
// So whoever rebuilds the approval QML (see MfaController's STATUS note) must
// build a number-match chooser, not a bare Approve button: an Approve that
// sends no digits cannot succeed.
class MfaResponseClient
{
public:
    explicit MfaResponseClient(HttpClient& httpClient);

    // matchDigits is ignored when approve is false.
    MfaResponseResult respond(const QUrl& serverBaseUrl, const QString& challengeId, const QString& deviceId,
                               const QString& deviceSecret, bool approve,
                               const QString& matchDigits = QString()) const;

private:
    HttpClient& m_httpClient;
};
