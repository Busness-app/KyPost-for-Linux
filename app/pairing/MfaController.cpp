#include "pairing/MfaController.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/HttpClient.h"
#include "net/MfaResponseClient.h"
#include "net/NetworkExecutor.h"

#include <KLocalizedString>

#include <QUrl>

MfaController::MfaController(NetworkExecutor& executor, PairingStore& pairingStore, QObject* parent)
    : QObject(parent)
    , m_executor(executor)
    , m_pairingStore(pairingStore)
{
}

QString MfaController::respondState() const
{
    return m_respondState;
}

QString MfaController::resultMessage() const
{
    return m_resultMessage;
}

bool MfaController::inFlight() const
{
    return m_inFlight;
}

void MfaController::setRespondState(const QString& state, const QString& message)
{
    if (m_respondState == state && m_resultMessage == message)
        return;
    m_respondState = state;
    m_resultMessage = message;
    emit respondStateChanged();
}

void MfaController::setInFlight(bool inFlight)
{
    if (m_inFlight == inFlight)
        return;
    m_inFlight = inFlight;
    emit inFlightChanged();
}

void MfaController::respond(const QString& challengeId, bool approve, const QString& matchDigits)
{
    if (m_inFlight)
        return;

    // Read on THIS thread, before anything is dispatched. PairingStore
    // caches and is mutated by the credential gate, so it stays confined to
    // the GUI thread; only the plain strings below cross to the executor.
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value()) {
        setRespondState(QStringLiteral("failed"), i18n("Not paired"));
        return;
    }

    const QUrl serverBaseUrl(pairing->serverBaseUrl);
    const QString deviceId = pairing->deviceId;
    const QString deviceSecret = pairing->deviceSecret;

    setInFlight(true);
    setRespondState(QStringLiteral("sending"));

    // Everything captured by value. The work runs on the executor thread and
    // must not reach back into anything owned by this one -- which is why
    // the pairing was flattened into strings above rather than captured as a
    // reference to the store.
    //
    // MfaResponseClient is constructed here, inside the work, because it is
    // a stateless wrapper around the HttpClient reference it is handed --
    // and that HttpClient belongs to the executor thread. Holding one as a
    // member would mean holding a reference to another thread's object.
    m_executor.run(
        this,
        [serverBaseUrl, challengeId, deviceId, deviceSecret, approve, matchDigits](HttpClient& http) {
            MfaResponseClient client(http);
            return client.respond(serverBaseUrl, challengeId, deviceId, deviceSecret, approve, matchDigits);
        },
        [this, approve](const MfaResponseResult& result) {
            // Back on this object's own thread, so touching QML-bound state
            // and emitting signals is safe.
            setInFlight(false);
            applyResult(result, approve);
        });
}

void MfaController::applyResult(const MfaResponseResult& result, bool approve)
{
    switch (result.outcome) {
    case MfaResponseOutcome::Success:
        setRespondState(QStringLiteral("done"), approve ? i18n("Approved") : i18n("Denied"));
        break;
    case MfaResponseOutcome::Unauthorized:
        // Not "already handled" -- the relay refused this device. The
        // overwhelmingly common cause is the credential PIN gate with the
        // app locked, where load() hands out an empty deviceSecret by
        // design and every authenticated request 401s until unlock.
        setRespondState(QStringLiteral("failed"),
                         i18n("KyPost could not authenticate to your server. Unlock the app, or "
                              "pair this device again."));
        break;
    case MfaResponseOutcome::Rejected:
        // status is populated from the response body when the server
        // included one (always on Success, optionally on a 409 Rejected --
        // see MfaResponseResult's doc comment); its presence is what lets
        // us tell "this challenge was already resolved" apart from a
        // bare denial. The status value itself is server-supplied free text
        // (data), only the surrounding sentence is i18n()-wrapped chrome.
        if (result.status.has_value() && !result.status->isEmpty()) {
            setRespondState(QStringLiteral("failed"),
                             i18n("This request was already resolved (%1).", *result.status));
        } else {
            setRespondState(QStringLiteral("failed"),
                             i18n("This request was already handled or denied."));
        }
        break;
    case MfaResponseOutcome::Failure:
        setRespondState(QStringLiteral("failed"),
                         result.detail.has_value() && !result.detail->isEmpty()
                             ? *result.detail
                             : i18n("Failed to send response, please try again."));
        break;
    }
}

void MfaController::reset()
{
    // Deliberately does not cancel an in-flight request -- there is nothing
    // to cancel it with, and the completion handler is what clears
    // m_inFlight. Resetting the label while a response is still on the wire
    // would show "idle" over a request that is about to answer.
    if (m_inFlight)
        return;
    setRespondState(QStringLiteral("idle"));
}
