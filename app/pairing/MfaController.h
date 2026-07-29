#pragma once

#include <QObject>
#include <QString>

class MfaResponseClient;
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
// This class is deliberately KEPT rather than deleted with the UI: the server
// filter is explicitly temporary ("until encryption is added" -- the RFC 8291
// UnifiedPush encryption plan is unimplemented server-side), and
// POST /api/mfa/push/respond remains a valid, device-authenticated endpoint.
// When that lands, this is the half that already works; only the QML needs
// rebuilding. Do not wire a screen to it before the backend change, or it
// will be dead on arrival again.

class MfaController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString respondState READ respondState NOTIFY respondStateChanged) // "idle" | "sending" | "done" | "failed"
    Q_PROPERTY(QString resultMessage READ resultMessage NOTIFY respondStateChanged) // human-readable, meaningful for "done"/"failed"

public:
    MfaController(MfaResponseClient& client, PairingStore& pairingStore, QObject* parent = nullptr);

    QString respondState() const;
    QString resultMessage() const;

public slots:
    // Reads sub/hash/deviceId from pairingStore.load() -- if not paired,
    // respondState="failed"+resultMessage set, no network call. Otherwise
    // calls client.respond(serverBaseUrl, challengeId, sub, hash, deviceId,
    // approve) and maps the MfaResponseOutcome to respondState/
    // resultMessage: Success -> "done"; Rejected -> "failed" with a message
    // distinguishing "already resolved" when the server's status field
    // carried that information, else a generic denial message; Failure ->
    // "failed" with the detail.
    // matchDigits is the number-match value the user picked; the server
    // requires it to approve and ignores it to deny. See MfaResponseClient.h —
    // an approve that sends none is refused with a 400.
    void respond(const QString& challengeId, bool approve, const QString& matchDigits = QString());
    void reset(); // back to "idle", for a retry

signals:
    void respondStateChanged();

private:
    void setRespondState(const QString& state, const QString& message = QString());

    MfaResponseClient& m_client;
    PairingStore& m_pairingStore;
    QString m_respondState = QStringLiteral("idle");
    QString m_resultMessage;
};
