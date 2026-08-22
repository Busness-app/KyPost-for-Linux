#include "pairing/PairingController.h"

#include "domain/DeviceRegistrationService.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/DeregisterClient.h"
#include "net/NetworkExecutor.h"
#include "net/CertificatePinSink.h"
#include "net/HttpClient.h"
#include "stores/SettingsStore.h"

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

// The only path a `reg` parameter may name. The registration endpoint is
// well-known, so there is no reason for a link to nominate any other.
constexpr auto kNativeRegisterPath = "/api/notifications/native/register";

// The origin string shown in the confirm dialog -- the only thing standing
// between a hostile deep link and a pairing.
//
// QUrl::authority() carries userinfo, which a hostile link could stuff with
// an "@"-prefixed lookalike ("https://real.example@evil.example"), so the
// origin is rebuilt from the parts that actually determine where the request
// goes. host() alone is still not safe to display: it defaults to
// QUrl::FullyDecoded, which turns punycode back into Unicode, and Qt applies
// no mixed-script or whole-script-confusable policy (its IDN whitelist
// contains .com). "https://mail.xn--urll-76d.com" then renders as
// "mail.url<CYRILLIC KHA>l.com" -- glyph-identical in the monospace font this
// is shown in. Always display the ACE form, as Chrome and Firefox do.
// IPv6 literals are bracketed so the port cannot be misread as part of the
// address.
QString displayOrigin(const QUrl& url)
{
    const QString host = url.host(QUrl::FullyEncoded);
    QString origin = url.scheme() + QStringLiteral("://");
    origin += host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(host) : host;
    if (url.port() != -1)
        origin += QStringLiteral(":") + QString::number(url.port());
    return origin;
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
    //
    // Origin alone was not enough: sameOrigin compares scheme/host/port and
    // leaves the PATH free, so `reg` could name the user's own real mail
    // server -- which is all the confirm dialog shows -- while pointing the
    // POST at an unrelated same-origin endpoint. The relay's unauthenticated
    // /api/health answers POST with a JSON object, which the registration
    // client used to accept as a successful pairing, overwriting the working
    // device credential with empty strings. Pin the path too.
    if (!parsed.registrationUrl.isEmpty()) {
        const QUrl registrationUrl(parsed.registrationUrl);
        if (!isAcceptablePairingScheme(registrationUrl) || !sameUrlOrigin(registrationUrl, serverUrl)
            || registrationUrl.path(QUrl::FullyEncoded) != QLatin1String(kNativeRegisterPath)) {
            return std::nullopt;
        }
    }

    return parsed;
}

} // namespace

PairingController::PairingController(DeviceRegistrationService& service, PairingStore& pairingStore,
                                       SettingsStore& settingsStore, CertificatePinSink& pinSink,
                                       NetworkExecutor& executor, QObject* parent)
    : QObject(parent)
    , m_service(service)
    , m_pairingStore(pairingStore)
    , m_settingsStore(settingsStore)
    , m_pinSink(pinSink)
    , m_executor(executor)
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
    return displayOrigin(QUrl(m_pendingPair->serverBaseUrl));
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
    const QString host = nowPaired ? QUrl(pairing->serverBaseUrl).host(QUrl::FullyEncoded) : QString();
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
    // m_inNetworkCall is now an in-flight FLAG, not a re-entrancy guard.
    // With the blocking call moved off this thread there is no nested event
    // loop to be re-entered through; what is left to prevent is two
    // overlapping registrations, which would race to set pairingState and
    // could both mint (and burn) a server-side device secret.
    if (m_inNetworkCall)
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
    pairFromParsedParams(pending.subscriberId, pending.serverBaseUrl, pending.pairingToken,
                          pending.registrationUrl);
    // True means "started", not "paired". The answer arrives on
    // pairingState; no QML call site reads this return value (checked), and
    // it is kept only so the slot signature stays source-compatible.
    return true;
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
    // Same reasoning as pairFromDeepLink() and confirmPendingPair(): a QQC2
    // Popup or Kirigami.OverlaySheet renders inside QQuickOverlay, which Qt
    // stacks above ordinary siblings -- including the app-lock overlay at
    // z: 1000 -- so a Settings sheet left open when the app locked stays
    // visible AND clickable over the PIN screen. This method deregisters the
    // device server-side and destroys the credential, so it is the one that
    // must not be reachable that way. QML z-order is not a security boundary.
    if (m_appLocked)
        return;

    if (m_inNetworkCall)
        return;

    // The deregister POST is best-effort and its result was already ignored,
    // so it is dispatched and forgotten rather than waited for. Local state
    // is cleared immediately and unconditionally below -- which is what the
    // user asked for, and what must happen even if the relay is unreachable.
    //
    // Everything the request needs is flattened out of PairingStore FIRST:
    // the store is cleared on the very next lines, and it is confined to
    // this thread besides.
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (pairing.has_value() && !pairing->deviceId.isEmpty() && !pairing->deviceSecret.isEmpty()) {
        const QUrl serverBaseUrl(pairing->serverBaseUrl);
        const QString deviceId = pairing->deviceId;
        const QString deviceSecret = pairing->deviceSecret;
        m_executor.runDetached([serverBaseUrl, deviceId, deviceSecret](HttpClient& http) {
            DeregisterClient client(http);
            client.deregister(serverBaseUrl, deviceId, deviceSecret);
        });
    }
    if (!m_pairingStore.clear()) {
        // Local state still clears below, but say so: "unpaired" with the
        // credential still sitting in the keychain is a different situation
        // from a clean unpair, and the user is the only one who can fix the
        // secret store.
        setPairingState(State::Failed,
                         i18n("Unpaired, but some credentials could not be removed from this "
                              "system's secret store. Check that your keyring is unlocked."));
    }
    // The stored pin went with PairingStore::clear(), but the copy HttpClient
    // is enforcing lives in memory for the process lifetime. Leaving it set is
    // what made the certificate-mismatch banner's own advice impossible to
    // follow: the re-pair POST met the relay's new certificate, the stale pin
    // aborted it, and the banner came straight back until the user restarted
    // -- which minimize-to-tray makes non-obvious and the UI never mentions.
    m_pinSink.clearPin();

    // A fresh pin is captured on the next pair, so the old mismatch is no
    // longer meaningful -- and leaving the banner up after the user has
    // acted on it would be its own bug.
    setCertificateMismatch(false);
    setReregistrationRejected(false);
    refreshFromStore();
}

void PairingController::setDeviceToken(const QString& token)
{
    m_deviceToken = token;
}

void PairingController::setAccountReplacementHandlers(std::function<bool()> purge, std::function<void()> escalate)
{
    m_purgeCachedAccountData = std::move(purge);
    m_escalateToFullWipe = std::move(escalate);
}

// Runs after a registration has SUCCEEDED and before its result is persisted.
// Returns false when the caller must abandon the pairing entirely.
//
// Order is the whole point, and it is the opposite of the obvious one. Purging
// first would mean a replacement that failed -- offline, a rejected token, a
// locked keyring -- had already deleted the mail, contacts and pairing of the
// account that was working a moment earlier. Nothing is destroyed until the
// replacement is proven.
bool PairingController::purgePreviousAccountIfReplaced(const PairingParams& params)
{
    if (!m_registrationHadPreviousAccount)
        return true; // first pairing on this device: nothing to replace

    // Compared against what was current when this registration STARTED, not
    // against the store now: a second registration can land while this one is
    // in flight, and re-reading would judge this reply against an account it
    // never saw.
    const bool sameAccount = m_registrationStartSubscriberId == params.subscriberId
        && m_registrationStartServerBaseUrl == params.serverBaseUrl;
    if (sameAccount)
        return true;

    if (!m_purgeCachedAccountData) {
        // Unwired. Refuse rather than proceed: shipping the mixed state is
        // the failure this exists to prevent, and a missing handler is a
        // composition-root bug, not a reason to relax the rule.
        qCritical("PairingController: account replacement with no purge handler installed -- refusing");
        return false;
    }

    if (m_purgeCachedAccountData())
        return true;

    // Something survived. There is no acceptable state in which two accounts'
    // data coexist on a device whose schema cannot tell them apart, so the
    // device is wiped rather than left mixed -- and the new pairing is
    // refused, because proceeding would be the mixing.
    qCritical("PairingController: could not erase the previous account's cached data -- "
              "erasing this device instead rather than leaving two accounts' mail readable together");
    if (m_escalateToFullWipe)
        m_escalateToFullWipe();
    return false;
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

bool PairingController::certificateMismatch() const
{
    return m_certificateMismatch;
}

void PairingController::setCertificateMismatch(bool mismatch, const QByteArray& observedSpki)
{
    // The fingerprint is part of this state, not separate from it: the
    // recovery dialog is unusable without it, and a stale one left over from
    // a previous mismatch would be worse than none. Compared as a pair so
    // re-reporting the same mismatch with a newly-read key still notifies.
    if (m_certificateMismatch == mismatch && m_observedSpki == observedSpki)
        return;
    m_certificateMismatch = mismatch;
    m_observedSpki = mismatch ? observedSpki : QByteArray();
    emit certificateMismatchChanged();
}

namespace {
// Colon-separated uppercase hex, the form every certificate tool prints, so
// what the UI shows can be compared byte-for-byte against
// `openssl pkey -pubin -outform der | sha256sum` on the server.
QString formatFingerprint(const QByteArray& sha256)
{
    if (sha256.isEmpty())
        return {};
    const QByteArray hex = sha256.toHex().toUpper();
    QString out;
    out.reserve(hex.size() + hex.size() / 2);
    for (int i = 0; i < hex.size(); i += 2) {
        if (i > 0)
            out += QLatin1Char(':');
        out += QLatin1String(hex.constData() + i, 2);
    }
    return out;
}
} // namespace

QString PairingController::expectedCertificateFingerprint() const
{
    // Read from the pin sink rather than PairingStore: the sink is what is
    // actually being ENFORCED, and the two can legitimately differ mid-flight
    // (DeviceRegistrationService suspends the pin while registering). Showing
    // the stored value while a different one is in force would be a lie at
    // exactly the moment the user is trying to make a trust decision.
    return formatFingerprint(m_pinSink.pinState().spkiSha256);
}

QString PairingController::observedCertificateFingerprint() const
{
    return formatFingerprint(m_observedSpki);
}

void PairingController::reconnectToServer()
{
    // Same guard as removePairing(): this rotates the device credential and
    // re-anchors the TLS trust anchor, so it must not be reachable from a
    // popup left open over the lock screen. QML z-order is not a security
    // boundary (AGENTS.md 6d).
    if (m_appLocked)
        return;
    if (m_inNetworkCall)
        return;

    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value() || pairing->deviceId.isEmpty()) {
        setPairingState(State::Failed, i18n("This device is not paired, so there is nothing to reconnect."));
        return;
    }

    PairingParams params;
    params.subscriberId = pairing->subscriberId;
    params.serverBaseUrl = pairing->serverBaseUrl;
    params.registrationUrl = pairing->registrationUrl.isEmpty() ? deriveRegistrationUrl(pairing->serverBaseUrl)
                                                                 : pairing->registrationUrl;
    params.pairingToken = pairing->pairingToken;
    params.deviceName = pairing->deviceName;

    setPairingState(State::Working);

    // Snapshot which account is current BEFORE the request. Judging the reply
    // against the store afterwards is a TOCTOU: a second registration can land
    // while this one is in flight (AGENTS.md 6d records the same shape for the
    // sealing key), and it would decide "is this a replacement?" against an
    // account this attempt never saw.
    const PairingStore::LoadResult previous = m_pairingStore.loadChecked();
    // Refused, not assumed. "Could not read the store" is not "there is no
    // previous account": treating it as one skips the replacement purge
    // entirely, and the previous account's cached mail is then handed to
    // whoever this registration is pairing. Nothing is destroyed on this
    // path either -- refusing costs a retry, and a registration whose
    // credentials this store cannot be read for could not be saved to it
    // anyway.
    if (previous.status == PairingStore::LoadStatus::Unreadable) {
        NativeRegistrationResult out;
        out.outcome = RegistrationOutcome::Failure;
        out.detail = i18n("Could not read this system's secret store, so KyPost cannot tell whether "
                           "pairing would replace an existing account. Check that a keyring service "
                           "(gnome-keyring or kwallet) is running and unlocked, then pair again.");
        applyRegistrationResult(out);
        return;
    }
    const std::optional<DevicePairing>& previousAccount = previous.pairing;
    m_registrationHadPreviousAccount = previousAccount.has_value() && !previousAccount->subscriberId.isEmpty();
    m_registrationStartSubscriberId = previousAccount.has_value() ? previousAccount->subscriberId : QString();
    m_registrationStartServerBaseUrl = previousAccount.has_value() ? previousAccount->serverBaseUrl : QString();

    // Phase 1 here: suspends the pin, which is what lets the request reach a
    // server presenting the NEW certificate at all. If this attempt is
    // abandoned or fails, PairAttempt's destructor puts the old pin back --
    // the reason a failed reconnect cannot silently disable pinning.
    DeviceRegistrationService::PairAttempt attempt = m_service.beginPair();
    if (const std::optional<RegistrationOutcome> refused = attempt.refusedOutcome()) {
        NativeRegistrationResult out;
        out.outcome = *refused;
        applyRegistrationResult(out);
        return;
    }

    m_inNetworkCall = true;
    m_executor.run(
        this,
        [params, deviceToken = m_deviceToken](HttpClient& http) {
            return DeviceRegistrationService::sendRegistration(http, params, deviceToken);
        },
        [this, params, attempt = std::move(attempt)](const NativeRegistrationResult& result) mutable {
            m_inNetworkCall = false;
            // Reconnect targets the account already paired, so this is a no-op
            // by construction. Run it anyway rather than depending on that
            // staying true if this method ever grows a second caller.
            if (result.outcome == RegistrationOutcome::Success && !purgePreviousAccountIfReplaced(params)) {
                NativeRegistrationResult refused = result;
                refused.outcome = RegistrationOutcome::Failure;
                applyRegistrationResult(refused);
                return;
            }
            const NativeRegistrationResult applied = m_service.finishPair(std::move(attempt), params, result);
            if (applied.outcome == RegistrationOutcome::Success) {
                // The new certificate is now the pinned one, so the condition
                // the banner describes is over.
                setCertificateMismatch(false);
                setReregistrationRejected(false);
            }
            applyRegistrationResult(applied);
        });
}

void PairingController::pairFromParsedParams(const QString& sub, const QString& srv, const QString& pt,
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
    // Snapshot which account is current BEFORE the request. Judging the reply
    // against the store afterwards is a TOCTOU: a second registration can land
    // while this one is in flight (AGENTS.md 6d records the same shape for the
    // sealing key), and it would decide "is this a replacement?" against an
    // account this attempt never saw.
    const PairingStore::LoadResult previous = m_pairingStore.loadChecked();
    // Refused, not assumed. "Could not read the store" is not "there is no
    // previous account": treating it as one skips the replacement purge
    // entirely, and the previous account's cached mail is then handed to
    // whoever this registration is pairing. Nothing is destroyed on this
    // path either -- refusing costs a retry, and a registration whose
    // credentials this store cannot be read for could not be saved to it
    // anyway.
    if (previous.status == PairingStore::LoadStatus::Unreadable) {
        NativeRegistrationResult out;
        out.outcome = RegistrationOutcome::Failure;
        out.detail = i18n("Could not read this system's secret store, so KyPost cannot tell whether "
                           "pairing would replace an existing account. Check that a keyring service "
                           "(gnome-keyring or kwallet) is running and unlocked, then pair again.");
        applyRegistrationResult(out);
        return;
    }
    const std::optional<DevicePairing>& previousAccount = previous.pairing;
    m_registrationHadPreviousAccount = previousAccount.has_value() && !previousAccount->subscriberId.isEmpty();
    m_registrationStartSubscriberId = previousAccount.has_value() ? previousAccount->subscriberId : QString();
    m_registrationStartServerBaseUrl = previousAccount.has_value() ? previousAccount->serverBaseUrl : QString();

    // Phase 1 on this thread: the guards, the sealing-key snapshot and the
    // certificate-pin suspension all touch PairingStore or the pin fan-out,
    // neither of which may leave this thread. Only the request itself moves.
    DeviceRegistrationService::PairAttempt attempt = m_service.beginPair();
    if (const std::optional<RegistrationOutcome> refused = attempt.refusedOutcome()) {
        NativeRegistrationResult out;
        out.outcome = *refused;
        applyRegistrationResult(out);
        return;
    }

    m_inNetworkCall = true;

    // The attempt is moved into the completion handler so it survives the
    // gap. If it were dropped here instead, its destructor would restore the
    // pin while the registration was still in flight.
    m_executor.run(
        this,
        [params, deviceToken = m_deviceToken](HttpClient& http) {
            return DeviceRegistrationService::sendRegistration(http, params, deviceToken);
        },
        [this, params, attempt = std::move(attempt)](const NativeRegistrationResult& result) mutable {
            m_inNetworkCall = false;
            // The replacement is proven here and nothing has been persisted
            // yet -- the only moment at which erasing the previous account's
            // data is both safe and still possible before it becomes readable
            // under the new pairing.
            if (result.outcome == RegistrationOutcome::Success && !purgePreviousAccountIfReplaced(params)) {
                NativeRegistrationResult refused = result;
                refused.outcome = RegistrationOutcome::Failure;
                refused.detail = QStringLiteral(
                    "Could not remove the previous account's data from this device, so this device "
                    "has been erased instead. Pair again to continue.");
                // `attempt` is dropped without finishPair(), so its destructor
                // restores the previous pin and nothing from this registration
                // is written.
                applyRegistrationResult(refused);
                return;
            }
            // Phase 3, back here: persists, installs the new pin, writes the
            // delivery settings.
            applyRegistrationResult(m_service.finishPair(std::move(attempt), params, result));
        });
}

void PairingController::applyRegistrationResult(const NativeRegistrationResult& result)
{
    switch (result.outcome) {
    case RegistrationOutcome::Success:
        refreshFromStore();
        setReregistrationRejected(false);
        // A successful handshake just re-pinned this server's SPKI, so any
        // earlier mismatch is resolved by construction.
        setCertificateMismatch(false);
        setPairingState(State::Paired);
        return;
    case RegistrationOutcome::Unauthorized:
        setPairingState(State::Failed,
                         i18n("This pairing link was rejected. Check the link and try again."));
        return;
    case RegistrationOutcome::BackendMisconfigured:
        setPairingState(State::Failed, i18n("The server is not configured for pairing yet."));
        return;
    case RegistrationOutcome::CredentialsLocked:
        setPairingState(State::Failed, i18n("Unlock KyPost first, then try again."));
        return;
    case RegistrationOutcome::Failure:
        setPairingState(State::Failed,
                         result.detail.isEmpty() ? i18n("Pairing failed, please try again.") : result.detail);
        return;
    }
}
