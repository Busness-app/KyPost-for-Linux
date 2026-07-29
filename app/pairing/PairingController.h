#pragma once

#include "domain/DeviceRegistrationService.h"

#include <QObject>
#include <QString>

#include <optional>

class QUrl;
class PairingStore;
class SettingsStore;
class CertificatePinSink;
class NetworkExecutor;

// QML-facing bridge (Task 34) over core/domain's DeviceRegistrationService/
// PairingStore. Registered as the "Pairing" QML singleton in main.cpp.
// pairFromDeepLink/pairFromPastedLink are the real replacement for the
// Task 12 routeDeepLink stub -- see main.cpp's routeDeepLink for the
// kypost://native-pair wiring. pairFromParsedParams (and therefore any
// successful pair) runs deviceRegistrationService.pair() synchronously on
// the calling (GUI) thread -- see Phase 6 global constraint 2, this is a
// known, accepted freeze-the-UI tradeoff for this phase, not a bug.
//
// deviceToken wiring (Task 43, see task-43-report.md): pair()'s deviceToken
// argument is m_deviceToken, populated via setDeviceToken() below rather
// than always QString() as it was up through Task 34. This client's push
// transport is UnifiedPush; main.cpp calls setDeviceToken() whenever
// UnifiedPushConnector reports its endpoint (constructed after engine.load(),
// so this is late-bound the same way pairingControllerForDeepLinks is --
// see setDeviceToken()'s own doc comment below), including once immediately
// after pushConnector's construction with whatever endpoint is already
// known. A live pairing attempt against a real backend was previously
// rejected with HTTP 400 (the register endpoint requires a non-empty
// deviceToken); this wiring, live-verified in Task 43, fixed that.
class PairingController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isPaired READ isPaired NOTIFY pairingChanged)
    // Host-only, never the full URL with token -- matches the existing
    // "never log the full endpoint" precedent from Task 11's post-push
    // security review.
    Q_PROPERTY(QString pairedServerHost READ pairedServerHost NOTIFY pairingChanged)
    Q_PROPERTY(QString deviceId READ deviceId NOTIFY pairingChanged)
    Q_PROPERTY(State state READ state NOTIFY pairingStateChanged)
    // Kept for the existing QML bindings; derived from `state` above, never
    // assigned independently.
    Q_PROPERTY(QString pairingState READ pairingState NOTIFY pairingStateChanged) // "idle" | "confirm" | "working" | "paired" | "failed"
    Q_PROPERTY(QString pairingError READ pairingError NOTIFY pairingStateChanged) // meaningful only when pairingState == "failed"
    // Origin ("scheme://host[:port]") of the server a not-yet-confirmed
    // pairFromDeepLink()/pairFromPastedLink() call wants to pair with --
    // meaningful only when pairingState == "confirm". Lets the confirmation UI
    // show the user which server is asking, before confirmPendingPair() makes
    // any network call. See PairingController.cpp's pairFromDeepLink() doc
    // comment for why this gate exists.
    //
    // The full origin rather than the bare host: this used to expose
    // QUrl::host() only, so "http://evil.example:8443" and
    // "https://evil.example" were indistinguishable in the one confirmation
    // the user ever sees -- and the scheme is the difference between sending
    // the pairing token and the real push device token over TLS or in
    // cleartext.
    Q_PROPERTY(QString pendingPairOrigin READ pendingPairOrigin NOTIFY pairingStateChanged)
    // True when that pending origin is plaintext http. Only reachable for
    // loopback (isAcceptablePairingScheme rejects http elsewhere), which is a
    // legitimate self-hosted/dev case -- but it must be announced rather than
    // blending in with https.
    Q_PROPERTY(bool pendingPairInsecure READ pendingPairInsecure NOTIFY pairingStateChanged)
    // True once a background re-registration was rejected with 401.
    //
    // AGENTS.md section 8 records this as a known live-system gotcha ("Re-
    // registration silently 401s once the pairing token expires ... handle
    // the 401 explicitly") and both call sites in main.cpp used to discard
    // the std::optional<NativeRegistrationResult> that reports it. The
    // consequence is invisible and total: the relay keeps publishing to a
    // dead endpoint, push stops arriving, and the UI still shows a healthy
    // "Paired" badge. The roots bind this to a persistent banner telling the
    // user to pair again.
    Q_PROPERTY(bool reregistrationRejected READ reregistrationRejected NOTIFY reregistrationRejectedChanged)
    // True once any request has been aborted because the relay's TLS
    // certificate no longer matches the SPKI pinned when this device paired
    // (HttpClient's trust-on-first-use pin).
    //
    // This needs its own persistent surface because the condition is total
    // and unrecoverable from inside the app: the pin is checked on
    // ::encrypted, so EVERY request aborts before it is sent, forever, and
    // the only exit is to pair again and capture a new pin. It previously
    // had no surface at all -- NetworkError::CertificateMismatch was
    // produced in core/net/HttpClient.cpp and read nowhere, so a routine
    // certificate rotation at the relay presented as an app that had simply
    // stopped working, with "Refresh failed" as its entire explanation.
    // The roots bind this to a banner that says what happened and offers
    // removePairing() as the way out.
    Q_PROPERTY(bool certificateMismatch READ certificateMismatch NOTIFY certificateMismatchChanged)
    // Task 39: read-only display fields for Settings > Notifications.
    // Sourced straight from SettingsStore on every read (no local cache).
    // deliveryMode/transport only ever change together with isPaired/
    // pairedServerHost/deviceId above (DeviceRegistrationService::pair()
    // writes all of them atomically on RegistrationOutcome::Success, per its
    // own class doc comment), so reusing pairingChanged() as NOTIFY here is
    // correct rather than adding a second signal that would always fire in
    // lockstep with it anyway. (A third property here, pushServerBaseUrl,
    // displayed the embedded ntfy subscriber's server; it was removed with
    // that tier on 2026-07-26 -- see core/domain/TransportStateMachine.h.)
    Q_PROPERTY(QString deliveryMode READ deliveryMode NOTIFY pairingChanged)     // "push" | "pull" | "" (never registered)
    Q_PROPERTY(QString transport READ transport NOTIFY pairingChanged)          // server-normalized transport name, "" if never registered

public:
    // The pairing state machine, as a type rather than five string literals
    // compared with == across three files. main.cpp used to test
    // `pairingState() == QStringLiteral("paired")` against a literal written
    // in PairingController.cpp: rename one and the other silently stops
    // matching, with no compiler error and no test failure -- just a feature
    // that quietly never happens again.
    //
    // Q_ENUM so QML can say Pairing.Paired. The `pairingState` string
    // property below is kept, and is now produced from this enum in exactly
    // one place (stateToString), so the existing QML comparisons keep
    // working without five more literals to maintain.
    enum class State { Idle, Confirm, Working, Paired, Failed };
    Q_ENUM(State)

    PairingController(DeviceRegistrationService& service, PairingStore& pairingStore, SettingsStore& settingsStore,
                       CertificatePinSink& pinSink, NetworkExecutor& executor,
                       QObject* parent = nullptr);

    bool isPaired() const;
    QString pairedServerHost() const;
    QString deviceId() const;
    State state() const;
    QString pairingState() const; // stateToString(state()) -- for existing QML bindings
    QString pairingError() const;
    QString pendingPairOrigin() const;
    bool pendingPairInsecure() const;
    QString deliveryMode() const;
    QString transport() const;
    bool reregistrationRejected() const;
    bool certificateMismatch() const;

public slots:
    // Re-reads pairingStore.load(), updates isPaired/pairedServerHost/
    // deviceId. Called once from the constructor, and again by
    // pairFromParsedParams() on a successful pair and by removePairing().
    void refreshFromStore();
    // Parses a kypost://native-pair URL per the wire format documented
    // on PairingController.cpp's parseNativePairLink(): sub/srv/pt query
    // params required and must be present AND non-empty (no `hash` param --
    // the per-device secret is issued only via the registration response),
    // reg optional (empty/absent derives the registration endpoint from
    // srv). On parse failure sets
    // pairingState="failed"+pairingError and returns false without any
    // network call.
    //
    // VibeSec fix: this app is registered as the OS-wide handler for the
    // kypost:// scheme (packaging/flatpak/com.urlxl.mail.desktop's
    // MimeType), so a link clicked anywhere on the system -- a browser, a
    // chat client, another app -- reaches this method, including via
    // KDBusService relaying a second launch's argv to an already-running
    // instance (main.cpp's routeDeepLink()), with none of this app's own UI
    // ever having been on screen. A successful parse therefore no longer
    // pairs immediately: it stores the parsed params and moves
    // pairingState to "confirm", where pendingPairOrigin tells the UI which
    // server is asking. Only an explicit confirmPendingPair() call actually
    // performs the network call and persists the new pairing;
    // cancelPendingPair() (or a fresh call to this method/pairFromPastedLink)
    // discards it instead.
    bool pairFromDeepLink(const QUrl& url);
    // Same as pairFromDeepLink but the input is a pasted string the user
    // typed/pasted into a TextField -- wrapped in QUrl(text), same
    // validation path (including the confirm gate above).
    bool pairFromPastedLink(const QString& text);
    // Performs the network call for whatever pairFromDeepLink/
    // pairFromPastedLink most recently parsed into pairingState=="confirm".
    // Returns false with no network call if there is no pending request
    // (e.g. called twice, or after cancelPendingPair()).
    bool confirmPendingPair();
    // Discards a pending pairFromDeepLink/pairFromPastedLink request without
    // ever making a network call, returning pairingState to "idle".
    void cancelPendingPair();
    void reset(); // sets pairingState back to "idle" (for a "Try Again" button after a failure)
    // Best-effort POST .../native/deregister (only when a deviceSecret is
    // actually stored -- a pairing from before this field existed has none,
    // and simply skips straight to the local clear below), then
    // unconditionally pairingStore.clear() + refreshFromStore() regardless
    // of the network outcome: offline, already-removed, or no secret at all
    // must never leave the user stuck "paired".
    void removePairing();
    // Late-bound, same pattern as main.cpp's pairingControllerForDeepLinks
    // pointer (Task 34): UnifiedPushConnector is constructed after this
    // class in main.cpp's dependency order, so main.cpp calls this whenever
    // UnifiedPushConnector::endpointChanged fires (including once with the
    // already-known endpoint right after pushConnector's construction).
    // The backend's deviceToken field is required (POST
    // /api/notifications/native/register) -- sending QString() here made
    // every first-time pairing attempt fail with a 400, discovered during
    // live E2E testing (Task 43). pairFromParsedParams() below now sends
    // whatever this holds, empty or not, rather than always QString().
    void setDeviceToken(const QString& token);

    // Called by main.cpp on every AppLockManager::lockedChanged, plus once at
    // startup. While locked, pairFromDeepLink() refuses to enter the confirm
    // state and confirmPendingPair() refuses to act.
    //
    // Pushed state rather than a pull-based probe, matching setDeviceToken()
    // above, so no constructor signature changes. The confirm prompt is a QQC2
    // Popup, which Qt renders inside QQuickOverlay -- above any z-ordered
    // sibling, including the app-lock overlay at z: 1000. Gating here rather
    // than relying on that stacking means the question does not have to be
    // answered, and covers both the argv deep-link path and the
    // KDBusService::activateRequested relay by construction.
    void setAppLocked(bool locked);

    // Called by main.cpp with the outcome of every background
    // reregisterIfPaired() -- true only for RegistrationOutcome::
    // Unauthorized, which is the expired-pairing-token case. Cleared by any
    // later successful (re-)pair.
    void setReregistrationRejected(bool rejected);

    // Called by main.cpp from HttpClient's certificate-mismatch handler,
    // which fires from inside a blocking request on the GUI thread. Latching
    // (only ever set true here) until removePairing() clears it: the pin is
    // process-wide and the condition does not resolve on its own.
    void setCertificateMismatch(bool mismatch);

signals:
    void pairingChanged();
    void pairingStateChanged();
    void reregistrationRejectedChanged();
    void certificateMismatchChanged();

private:
    // Builds a PairingParams from already-validated fields, sets
    // pairingState="working", calls
    // deviceRegistrationService.pair(params, m_deviceToken), maps
    // RegistrationOutcome to pairingState/pairingError, calls
    // refreshFromStore() on success.
    // Returns nothing: the registration is dispatched and the answer arrives
    // on pairingState. See applyRegistrationResult below.
    void pairFromParsedParams(const QString& sub, const QString& srv, const QString& pt, const QString& reg);
    // The completion half, running back on this object's own thread.
    void applyRegistrationResult(const NativeRegistrationResult& result);
    // forceNotify: emit pairingStateChanged() even when (state, error) is
    // unchanged from the current values -- needed when some OTHER piece of
    // NOTIFY-bound state (e.g. m_pendingPair) changed too, since QML
    // property bindings only re-evaluate on the declared NOTIFY signal, not
    // on every call to this setter. See pairFromDeepLink()'s call site.
    void setPairingState(State state, const QString& error = QString(), bool forceNotify = false);

    DeviceRegistrationService& m_service;
    PairingStore& m_pairingStore;
    SettingsStore& m_settingsStore;
    // Only to drop the in-process certificate pin on unpair -- see
    // removePairing(). PairingController makes no requests of its own.
    CertificatePinSink& m_pinSink;
    NetworkExecutor& m_executor;
    State m_state = State::Idle;
    QString m_pairingError;
    bool m_isPaired = false;
    QString m_pairedServerHost;
    QString m_deviceId;
    QString m_deviceToken; // set via setDeviceToken(); empty until UnifiedPushConnector reports a real endpoint
    // Set by pairFromDeepLink()/pairFromPastedLink() on a successful parse,
    // consumed by confirmPendingPair(), discarded by cancelPendingPair() or
    // by a fresh pairFromDeepLink()/pairFromPastedLink() call. Meaningful
    // only while pairingState == "confirm".
    std::optional<PairingParams> m_pendingPair;
    // Guards this controller's network-calling slots against re-entering
    // through the nested QEventLoop HttpClient runs -- QML keeps delivering
    // clicks while a blocking call is suspended. See
    // core/util/ReentrancyGuard.h.
    bool m_inNetworkCall = false;
    bool m_reregistrationRejected = false;
    bool m_certificateMismatch = false;
    bool m_appLocked = false;
};
