#pragma once

#include "net/MfaResponseClient.h"

#include <QObject>
#include <QString>

class NetworkExecutor;
class PairingStore;

// QML-facing bridge (Task 34) over core/net's MfaResponseClient, reading the
// authenticating sub/hash/deviceId straight out of PairingStore (this
// device's own MFA push-challenge responses are always sent as "this
// device", never on behalf of another). Registered as the "Mfa" QML
// singleton in main.cpp. respond() runs synchronously on the calling (GUI)
// thread -- see Phase 6 global constraint 2, this is a known, accepted
// freeze-the-UI tradeoff for this phase, not a bug.
//
// STATUS (2026-07-25): no QML consumes this. The Unlock/approval screen that
// used to live at app/qml/pages/MfaApproval.qml was deleted because it could
// never run: kypost-server derives transport "unifiedpush" from
// platform "linux" (internal/api/server.go), and push_mfa_handlers.go filters
// unifiedpush devices out of every MFA challenge -- so a Linux device is never
// notified of one, in push OR pull mode.
//
// SETTLED (2026-08-23, by the user): Linux cannot do MFA push. This class is
// dead code, not a half-built feature waiting on a backend change.
//
// The note above used to call the server filter "explicitly temporary". That
// was wrong about what it is. MFATransportEligible's own comment gives the
// reason -- a challenge carries sign-in metadata (IP address, user agent, the
// match digits) and UnifiedPush delivers through an unencrypted public broker
// such as ntfy.sh -- so it is a privacy control, and the thing that would have
// to change is encryption of the push payload itself, in the UnifiedPush
// ecosystem rather than in KyPost.
//
// POST /api/mfa/push/respond is still a valid device-authenticated endpoint,
// which is why this compiles and its tests pass. Nothing sends this device a
// challenge to respond TO. Do not wire a screen to it.

class MfaController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString respondState READ respondState NOTIFY respondStateChanged) // "idle" | "sending" | "done" | "failed"
    Q_PROPERTY(QString resultMessage READ resultMessage NOTIFY respondStateChanged) // human-readable, meaningful for "done"/"failed"
    Q_PROPERTY(bool inFlight READ inFlight NOTIFY inFlightChanged)

public:
    // Takes the executor rather than an MfaResponseClient. The client is a
    // stateless wrapper around an HttpClient reference, so it is constructed
    // per call ON the executor thread, where the HttpClient it must borrow
    // actually lives. Holding a long-lived client here would mean holding a
    // reference to an HttpClient owned by another thread.
    MfaController(NetworkExecutor& executor, PairingStore& pairingStore, QObject* parent = nullptr);

    QString respondState() const;
    QString resultMessage() const;
    // True between respond() and its answer, so a screen can disable its
    // buttons for the duration -- the job the synchronous call's return
    // value used to do implicitly by not coming back.
    bool inFlight() const;

public slots:
    // Returns immediately. respondState goes to "sending" before this
    // returns, then to "done" or "failed" when the reply arrives; QML binds
    // to that rather than to a return value.
    //
    // The pairing is read HERE, on the calling thread, and only the plain
    // strings it yields are handed to the executor. PairingStore is not
    // thread-safe -- it caches, and the credential gate mutates it -- so it
    // stays confined to this thread, which is also why the "not paired"
    // answer is still immediate.
    //
    // Ignored while a response is already in flight. That is request
    // coalescing, not the re-entrancy guard it replaces: with the blocking
    // call gone there is no nested event loop to be re-entered through, so
    // the only thing left to prevent is two overlapping answers to the same
    // challenge racing to set respondState.
    //
    // matchDigits is the number-match value the user picked; the server
    // requires it to approve and ignores it to deny. See MfaResponseClient.h —
    // an approve that sends none is refused with a 400.
    void respond(const QString& challengeId, bool approve, const QString& matchDigits = QString());
    void reset(); // back to "idle", for a retry

signals:
    void respondStateChanged();
    void inFlightChanged();

private:
    void setRespondState(const QString& state, const QString& message = QString());
    void setInFlight(bool inFlight);
    // Maps an MfaResponseOutcome onto respondState/resultMessage. Split out
    // because it now runs from a completion handler rather than inline, and
    // is the only part a test needs to reach without a server.
    void applyResult(const MfaResponseResult& result, bool approve);

    NetworkExecutor& m_executor;
    PairingStore& m_pairingStore;
    QString m_respondState = QStringLiteral("idle");
    QString m_resultMessage;
    bool m_inFlight = false;
};
