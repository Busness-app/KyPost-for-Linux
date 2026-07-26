#include "pairing/PairingController.h"

#include "domain/DeviceRegistrationService.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/DeregisterClient.h"
#include "stores/SettingsStore.h"
#include "util/ReentrancyGuard.h"

#include <KLocalizedString>

#include <QHostAddress>
#include <QUrl>
#include <QUrlQuery>

namespace {

// Deep-link wire format, confirmed against both this project's Android and
// Swift sibling clients' real parsers:
// kypost://native-pair?sub=<id>&srv=<serverBaseUrl>&pt=<pairingToken>&reg=<optional>
//
// sub/srv/pt must be present in the query AND non-empty. There is no `hash`
// param at all -- the per-device pairing secret is no longer carried in the
// deep link/QR; it's issued only via the registration response (see
// DevicePairing::deviceSecret's doc comment). reg is optional;
// empty/absent means "derive from srv".
struct ParsedPairingLink
{
    QString subscriberId;
    QString serverBaseUrl;
    QString pairingToken;
    QString registrationUrl; // empty if reg was absent/empty in the link
};

// Mirrors Android's NativeRegistrationEndpointResolver.resolve: strips any
// trailing slashes off srv, then appends the well-known native-register
// path.
QString deriveRegistrationUrl(const QString& serverBaseUrl)
{
    QString base = serverBaseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base + QStringLiteral("/api/notifications/native/register");
}

// VibeSec finding: a kypost://native-pair link's `srv` accepted any scheme,
// including http://, with no warning -- and what the confirm dialog showed
// stripped the scheme entirely, so even an attentive user had no way to notice
// they were about to pair (and send the pairing token + real push deviceToken)
// in cleartext. https is required except for loopback, which every
// local/self-hosted-dev pairing flow (and this file's own test suite)
// legitimately targets over plain http.
//
// The disclosure half of that finding is now fixed too: pendingPairOrigin()
// below reports "scheme://host[:port]", and pendingPairInsecure() lets the
// dialog call out the remaining legitimate cleartext (loopback) case rather
// than letting it blend in with https.
bool isAcceptablePairingScheme(const QUrl& url)
{
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)
        return true;
    if (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0)
        return false;

    const QString host = url.host();
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0)
        return true;
    QHostAddress addr;
    return addr.setAddress(host) && addr.isLoopback();
}

// The one place a State becomes a string. Every QML comparison and the
// pairingState property read through here, so the five literals that used to
// be scattered across this file (and one in main.cpp) now have a single
// definition the compiler checks the enum side of.
QString stateToString(PairingController::State state)
{
    switch (state) {
    case PairingController::State::Idle:
        return QStringLiteral("idle");
    case PairingController::State::Confirm:
        return QStringLiteral("confirm");
    case PairingController::State::Working:
        return QStringLiteral("working");
    case PairingController::State::Paired:
        return QStringLiteral("paired");
    case PairingController::State::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

bool sameOrigin(const QUrl& a, const QUrl& b)
{
    return a.scheme() == b.scheme() && a.host() == b.host() && a.port() == b.port();
}

std::optional<ParsedPairingLink> parseNativePairLink(const QUrl& url)
{
    if (url.scheme() != QStringLiteral("kypost") || url.host() != QStringLiteral("native-pair"))
        return std::nullopt;

    const QUrlQuery query(url);
    if (!query.hasQueryItem(QStringLiteral("sub")) || !query.hasQueryItem(QStringLiteral("srv"))
        || !query.hasQueryItem(QStringLiteral("pt")))
        return std::nullopt;

    ParsedPairingLink parsed;
    parsed.subscriberId = query.queryItemValue(QStringLiteral("sub"), QUrl::FullyDecoded);
    parsed.serverBaseUrl = query.queryItemValue(QStringLiteral("srv"), QUrl::FullyDecoded);
    parsed.pairingToken = query.queryItemValue(QStringLiteral("pt"), QUrl::FullyDecoded);
    parsed.registrationUrl = query.queryItemValue(QStringLiteral("reg"), QUrl::FullyDecoded);

    if (parsed.subscriberId.isEmpty() || parsed.serverBaseUrl.isEmpty() || parsed.pairingToken.isEmpty())
        return std::nullopt;

    const QUrl serverUrl(parsed.serverBaseUrl);
    if (!isAcceptablePairingScheme(serverUrl))
        return std::nullopt;

    // VibeSec finding: `reg` used to be able to point the actual
    // registration POST (carrying subscriberId/pairingToken/the real push
    // deviceToken) at a completely different host than `srv` -- the only
    // value pendingPairOrigin() (below) ever surfaces to the confirm dialog.
    // Requiring reg to share srv's origin means the host the user approves
    // is always the host that's actually contacted.
    if (!parsed.registrationUrl.isEmpty()) {
        const QUrl registrationUrl(parsed.registrationUrl);
        if (!isAcceptablePairingScheme(registrationUrl) || !sameOrigin(registrationUrl, serverUrl))
            return std::nullopt;
    }

    return parsed;
}

} // namespace

PairingController::PairingController(DeviceRegistrationService& service, PairingStore& pairingStore,
                                       SettingsStore& settingsStore, DeregisterClient& deregisterClient,
                                       QObject* parent)
    : QObject(parent)
    , m_service(service)
    , m_pairingStore(pairingStore)
    , m_settingsStore(settingsStore)
    , m_deregisterClient(deregisterClient)
{
    // Unlike MailController/ContactsController (which deliberately start
    // empty until QML calls a load slot), the pairing badge/menu entries
    // that read isPaired/pairedServerHost need a correct answer from the
    // very first frame -- there is no reasonable "unknown" state to show
    // meanwhile -- so this one refreshes eagerly on construction.
    refreshFromStore();
}

bool PairingController::isPaired() const
{
    return m_isPaired;
}

QString PairingController::pairedServerHost() const
{
    return m_pairedServerHost;
}

QString PairingController::deviceId() const
{
    return m_deviceId;
}

PairingController::State PairingController::state() const
{
    return m_state;
}

QString PairingController::pairingState() const
{
    return stateToString(m_state);
}

QString PairingController::pairingError() const
{
    return m_pairingError;
}

QString PairingController::pendingPairOrigin() const
{
    if (!m_pendingPair.has_value())
        return QString();
    const QUrl url(m_pendingPair->serverBaseUrl);
    // QUrl::authority() carries userinfo, which a hostile link could stuff with
    // an "@"-prefixed lookalike ("https://real.example@evil.example"). Rebuild
    // the origin from the parts that actually determine where the request goes.
    QString origin = url.scheme() + QStringLiteral("://") + url.host();
    if (url.port() != -1)
        origin += QStringLiteral(":") + QString::number(url.port());
    return origin;
}

bool PairingController::pendingPairInsecure() const
{
    if (!m_pendingPair.has_value())
        return false;
    return QUrl(m_pendingPair->serverBaseUrl).scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0;
}

void PairingController::setAppLocked(bool locked)
{
    m_appLocked = locked;
}

QString PairingController::deliveryMode() const
{
    return m_settingsStore.deliveryMode();
}

QString PairingController::transport() const
{
    return m_settingsStore.transport();
}

void PairingController::setPairingState(State state, const QString& error, bool forceNotify)
{
    const bool unchanged = (m_state == state && m_pairingError == error);
    m_state = state;
    m_pairingError = error;
    if (!unchanged || forceNotify)
        emit pairingStateChanged();
}

void PairingController::refreshFromStore()
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    const bool nowPaired = pairing.has_value();
    const QString host = nowPaired ? QUrl(pairing->serverBaseUrl).host() : QString();
    const QString deviceId = nowPaired ? pairing->deviceId : QString();

    if (nowPaired == m_isPaired && host == m_pairedServerHost && deviceId == m_deviceId)
        return;

    m_isPaired = nowPaired;
    m_pairedServerHost = host;
    m_deviceId = deviceId;
    emit pairingChanged();
}

bool PairingController::pairFromDeepLink(const QUrl& url)
{
    // A locked app must not raise a confirmable pairing prompt. The prompt is a
    // QQC2 Popup, drawn inside QQuickOverlay, which Qt stacks above ordinary
    // siblings -- including the app-lock overlay at z: 1000 -- so relying on
    // z-order here would be relying on an implementation detail. Refusing in the
    // controller settles it regardless of what paints on top.
    //
    // The request is dropped rather than queued, matching Android
    // (PushPairingActivity is a LockedActivity and finishes on start, discarding
    // the intent): the user re-opens the link after unlocking, and no
    // attacker-supplied payload survives across the lock waiting for a later
    // confirm.
    if (m_appLocked) {
        m_pendingPair.reset();
        setPairingState(State::Failed, i18n("Unlock KyPost first, then open the pairing link again."));
        return false;
    }

    const std::optional<ParsedPairingLink> parsed = parseNativePairLink(url);
    if (!parsed.has_value()) {
        m_pendingPair.reset();
        setPairingState(State::Failed, i18n("This pairing link is invalid or incomplete."));
        return false;
    }

    // See this method's header doc comment: a recognized link no longer
    // pairs immediately -- it waits in "confirm" for confirmPendingPair().
    PairingParams params;
    params.subscriberId = parsed->subscriberId;
    params.serverBaseUrl = parsed->serverBaseUrl;
    params.registrationUrl = parsed->registrationUrl.isEmpty() ? deriveRegistrationUrl(parsed->serverBaseUrl)
                                                                 : parsed->registrationUrl;
    params.pairingToken = parsed->pairingToken;
    m_pendingPair = params;
    // forceNotify=true: VibeSec fix -- m_pendingPair just changed even when
    // the state label ("confirm") didn't, e.g. a second link arriving while
    // the confirm dialog from a first link is still open. pendingPairOrigin
    // is NOTIFY-bound to pairingStateChanged, so without forcing the emit
    // here the dialog would keep showing the FIRST link's host while
    // confirmPendingPair() would actually act on the SECOND link's params.
    setPairingState(State::Confirm, QString(), /*forceNotify=*/true);
    return true;
}

bool PairingController::pairFromPastedLink(const QString& text)
{
    return pairFromDeepLink(QUrl(text));
}

bool PairingController::confirmPendingPair()
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    // Re-checked here, not just in pairFromDeepLink: this covers the lock
    // engaging while a confirm dialog is already open and visible, which is
    // exactly the case the Popup-over-lock-overlay stacking would expose.
    if (m_appLocked) {
        setPairingState(State::Failed, i18n("Unlock KyPost first, then open the pairing link again."));
        return false;
    }

    if (!m_pendingPair.has_value()) {
        setPairingState(State::Failed, i18n("There is no pending pairing request to confirm."));
        return false;
    }

    const PairingParams pending = *m_pendingPair;
    m_pendingPair.reset();
    return pairFromParsedParams(pending.subscriberId, pending.serverBaseUrl, pending.pairingToken,
                                 pending.registrationUrl);
}

void PairingController::cancelPendingPair()
{
    m_pendingPair.reset();
    setPairingState(State::Idle);
}

void PairingController::reset()
{
    m_pendingPair.reset();
    setPairingState(State::Idle);
}

void PairingController::removePairing()
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;

    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (pairing.has_value() && !pairing->deviceId.isEmpty() && !pairing->deviceSecret.isEmpty()) {
        // Best-effort: the result is intentionally ignored -- local state
        // clears unconditionally below regardless of network outcome.
        m_deregisterClient.deregister(QUrl(pairing->serverBaseUrl), pairing->deviceId, pairing->deviceSecret);
    }
    m_pairingStore.clear();
    refreshFromStore();
}

void PairingController::setDeviceToken(const QString& token)
{
    m_deviceToken = token;
}

bool PairingController::reregistrationRejected() const
{
    return m_reregistrationRejected;
}

void PairingController::setReregistrationRejected(bool rejected)
{
    if (m_reregistrationRejected == rejected)
        return;
    m_reregistrationRejected = rejected;
    emit reregistrationRejectedChanged();
}

bool PairingController::pairFromParsedParams(const QString& sub, const QString& srv, const QString& pt,
                                              const QString& reg)
{
    setPairingState(State::Working);

    PairingParams params;
    params.subscriberId = sub;
    params.serverBaseUrl = srv;
    params.registrationUrl = reg.isEmpty() ? deriveRegistrationUrl(srv) : reg;
    params.pairingToken = pt;
    // deviceName: not part of the deep-link wire format and not otherwise
    // sourced by this task -- left empty. A later task can add a "name this
    // device" field to the pairing UI if the plan wants one; nothing here
    // depends on it being non-empty.

    // m_deviceToken is set via setDeviceToken(), called from main.cpp
    // whenever UnifiedPushConnector reports its current endpoint (Task 43
    // fix -- the backend rejects a first-time pairing request with a 400
    // when deviceToken is empty, since the field is required). Still empty
    // if the distributor hasn't reported an endpoint yet by the time the
    // user completes pairing; the existing endpointChanged ->
    // reregisterIfPaired wiring in main.cpp corrects a stale/empty token
    // once one becomes available.
    const NativeRegistrationResult result = m_service.pair(params, m_deviceToken);

    switch (result.outcome) {
    case RegistrationOutcome::Success:
        refreshFromStore();
        setReregistrationRejected(false);
        setPairingState(State::Paired);
        return true;
    case RegistrationOutcome::Unauthorized:
        setPairingState(State::Failed,
                         i18n("This pairing link was rejected. Check the link and try again."));
        return false;
    case RegistrationOutcome::BackendMisconfigured:
        setPairingState(State::Failed, i18n("The server is not configured for pairing yet."));
        return false;
    case RegistrationOutcome::Failure:
        setPairingState(State::Failed,
                         result.detail.isEmpty() ? i18n("Pairing failed, please try again.") : result.detail);
        return false;
    }
    return false;
}
