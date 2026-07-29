#pragma once

#include <QString>
#include <QStringList>

// Inbound MFA push challenge shape, verified against kypost-android's
// MfaChallengePayload (app/src/main/java/com/urlxl/mail/push/
// MfaChallengePayload.kt). `deviceId`/`deviceSecret` needed to authenticate
// the `/api/mfa/push/respond` request live in a separate pairing/session
// store, not on this model. `receivedAt` is the local ISO-8601 UTC
// timestamp recorded when the challenge arrived (same QString convention as
// Email::atUtc), not itself a wire field.
//
// The payload also carries ipAddress/userAgent/issuedAt context for the
// approval screen; those are not modelled here because nothing on this
// client renders them yet (see MfaController's STATUS note).
struct MfaChallenge
{
    QString challengeId;
    QString receivedAt;

    // The number-match value the server is simultaneously showing in the
    // browser that started the sign-in, plus the wrong options to offer
    // beside it (sent comma-joined on the wire). Empty from a server that
    // predates number matching.
    //
    // Not decoration: the server verifies matchDigits on the approve path and
    // refuses an approval without it, so an approval screen that does not
    // collect this cannot approve anything. See MfaResponseClient.h.
    QString matchDigits;
    QStringList decoyDigits;

    bool operator==(const MfaChallenge&) const = default;
};
