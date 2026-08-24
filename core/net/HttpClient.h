#pragma once

#include "net/NetworkError.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QThread>
#include <QSslCertificate>
#include <QUrl>
#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

// Synchronous-from-the-caller's-point-of-view wrapper around
// QNetworkAccessManager for Relay HTTP calls. get()/post() block the calling
// thread on a local QEventLoop driven by the reply's finished signal — this
// mirrors kypost-for-Mac's HTTPClient async/await call shape one-for-one
// (verified reference: Data/Networking/HTTPClient.swift, read for structure
// only), so every Task 14-18 client reads as a straight-line sequence
// instead of a signal/callback chain.
//
// THREADING, stated accurately rather than aspirationally.
//
// An HttpClient is safe to use from whichever thread owns it, and must be
// used from that thread only -- including its certificate-pin state, which a
// request reads mid-handshake. core/net/NetworkExecutor owns one on a worker
// thread and is the supported way to get there; see docs/THREADING.md.
//
// MIGRATION IN PROGRESS. Two instances currently coexist: the executor's,
// used by every converted controller, and one on the GUI thread in main()
// still serving the rest. For those remaining callers the paragraph below
// still applies in full.
//
// This header used to say "Callers must invoke get()/post() off the GUI
// thread once app/ wiring exists in a later phase." That phase did not
// arrive for a long time, and reading an intention as a description of the
// code is how the following kept being treated as unrelated bugs rather than
// one consequence:
//
//   * a nested QEventLoop keeps delivering QML input, so any controller
//     method that makes a network call can be re-entered from the UI while
//     it is suspended (core/util/ReentrancyGuard.h existed for this, and
//     guarded a method against itself only -- deleted once every caller had
//     moved to NetworkExecutor, since nothing can re-enter what is not
//     suspended);
//   * AppLock.lockNow() can land in the middle of PairingStore::save(),
//     which is why that function needs a lock epoch;
//   * DeviceRegistrationService has to snapshot the sealing key before the
//     call, because re-reading it afterwards is a TOCTOU;
//   * main.cpp's deferred re-registration retry needs a
//     QTimer::singleShot(0) purely to escape a half-finished unlock.
//
// Each of those is a real fix and none of them is the fix. Moving this
// class and its QNetworkAccessManager onto a worker thread -- and making the
// controllers dispatch-and-return against the busy/state properties they
// already expose -- is what removes the category. That is now underway; see
// PairingController for the shape and docs/THREADING.md for the order and
// the two constraints that decide it.
//
// Until a given caller is converted: treat its call sites as re-entrant, and
// assume any member read after a call may have changed underneath it.
//
// The QNetworkAccessManager is injected via constructor reference rather
// than default-constructed internally, so tests can point it at a local
// QTcpServer and so threading/lifetime ownership stays with the caller.
class HttpClient
{
public:
    struct HttpResult
    {
        std::optional<NetworkError> error;
        int statusCode = 0;
        QByteArray body;
        QString detail; // human-readable detail for Transport/InvalidUrl failures; empty otherwise
        // Response headers as received from the server, populated by all four
        // verb methods below (via waitForReply). Added in Task 18 for the
        // attachment-download endpoint, whose filename/mime type travel in
        // Content-Disposition/Content-Type rather than the JSON body -- empty
        // for the InvalidUrl early-return path, where no reply was made.
        QList<QPair<QString, QString>> headers;
        // SPKI SHA-256 of the certificate THIS reply was served over, or
        // empty for a plaintext request. Read from the reply's own
        // QSslConfiguration rather than from shared state, because
        // QNetworkReply::encrypted fires once per TLS *connection*, not per
        // request: on a pooled keep-alive reuse it never fires at all. A
        // process-global "last handshake seen anywhere" slot therefore
        // reported whatever host handshook most recently -- which let a
        // scanned PGP QR code aimed at an attacker's host decide the SPKI
        // that the next unattended re-registration pinned as the relay's.
        // peerCertificate() is populated on reused connections too, so this
        // is both correct and per-request.
        QByteArray peerSpkiSha256;
    };

    // Byte ceilings for a response body. A relay is inside this app's threat
    // model everywhere else, so "the relay would never send that much" is not
    // a bound -- QNetworkReply buffers the whole body in memory and
    // readAll() used to hand over whatever arrived, with nothing anywhere
    // saying no. One hostile or compromised response was an unbounded
    // allocation.
    //
    // The shape is kypost-android's (MemoryBudget.kt): a tight default that
    // every route inherits, plus named exceptions for the two that
    // legitimately dwarf it. The VALUES are not Android's, deliberately.
    // Android asks the relay for `limit=50`; this client sends no limit at
    // all, so it gets the server default of 500 (maxInboxLimit in
    // server_inbox.go), and the relay does not truncate bodies -- warmBody()
    // stores them whole. A full window here is therefore up to ~10x the one
    // Android's 8 MB was measured against, and copying that number would
    // have broken first sync on any substantial mailbox.
    //
    // If the inbox window is ever given an explicit `limit`, revisit
    // kMaxInboxResponseBytes with it -- the two numbers are one decision.
    static constexpr qint64 kMaxResponseBytes = 8LL * 1024 * 1024;
    // The full-window inbox fetch: 500 messages with complete HTML bodies.
    static constexpr qint64 kMaxInboxResponseBytes = 64LL * 1024 * 1024;
    // One attachment. Matches the relay's own 25 MB cap, so a response above
    // this is already outside what the server will knowingly serve.
    static constexpr qint64 kMaxAttachmentResponseBytes = 25LL * 1024 * 1024;

    // transferTimeoutMs guards waitForReply()'s QEventLoop against a
    // hung/silent server that never emits QNetworkReply::finished (no
    // response, no error) -- without it, the calling thread blocks forever.
    // Only applied to the injected manager if it doesn't already have a
    // transfer timeout configured (manager.transferTimeout() == 0), so a
    // caller's own configuration is never clobbered.
    //
    // maxResponseBytes bounds what any single reply may allocate. Both are
    // constructor parameters rather than hardcoded so tests can drive them
    // without waiting out, or generating, real megabytes.
    explicit HttpClient(QNetworkAccessManager& manager, int transferTimeoutMs = 30000,
                         qint64 maxResponseBytes = kMaxResponseBytes);

    // Called with each redirect target get() is offered, when non-empty.
    // Returning false stops the redirect from being followed -- the caller
    // then sees the original (pre-redirect) response instead. Existing
    // callers that don't pass one keep Qt's normal automatic-redirect
    // behavior unchanged. See PgpQrController::scanQrPayload's use of this
    // for the VibeSec finding it closes: a URL that legitimately passes a
    // safety check (isSafeQrTarget) could otherwise still redirect the
    // actual request to a target that check would have rejected.
    using RedirectValidator = std::function<bool(const QUrl&)>;

    // HttpResult never decodes JSON: decoding into a concrete struct is each
    // Task 14-18 client's own responsibility (QJsonDocument::fromJson on
    // HttpResult::body, mapping a QJsonParseError to NetworkError::Decoding
    // if error is unset here but parsing still fails).
    // When no redirectValidator is supplied, every verb below defaults to
    // refusing any redirect that leaves the request's own origin. Qt's
    // default (NoLessSafeRedirectPolicy) follows cross-HOST redirects and
    // strips nothing: measured per status code, all of 301/302/303/307/308
    // forward custom headers -- including X-Kypost-Device-Secret -- to the
    // new host, and 307/308 forward the request body as well, which on the
    // registration POST is the subscriber id, the pairing token and the
    // UnifiedPush endpoint. The relay itself emits no redirects, so nothing
    // legitimate is lost by refusing them.
    //
    // maxResponseBytes overrides this client's default ceiling for this one
    // call. Only get() takes it: the two routes that need more than the
    // default -- the inbox window and an attachment download -- are both
    // GETs, and every POST/PUT/DELETE response on this API is a small status
    // object. A route that needs the exception says so at its call site,
    // which is where the reason is legible.
    HttpResult get(const QUrl& url, const QList<QPair<QString, QString>>& query,
                   const QList<QPair<QString, QString>>& headers = {},
                   const RedirectValidator& redirectValidator = {},
                   std::optional<qint64> maxResponseBytes = std::nullopt);

    // Sets Content-Type: application/json.
    HttpResult post(const QUrl& url, const QList<QPair<QString, QString>>& query,
                     const QJsonObject& jsonBody, const QList<QPair<QString, QString>>& headers = {},
                     const RedirectValidator& redirectValidator = {});

    // Sets Content-Type: application/json. Mirrors post()'s signature shape
    // -- added in Task 17 for the folder-rename endpoint.
    HttpResult put(const QUrl& url, const QList<QPair<QString, QString>>& query,
                    const QJsonObject& jsonBody, const QList<QPair<QString, QString>>& headers = {},
                    const RedirectValidator& redirectValidator = {});

    // No body -- mirrors get()'s signature shape. Added in Task 17 for the
    // folder-delete endpoint, which takes its target via query param.
    HttpResult del(const QUrl& url, const QList<QPair<QString, QString>>& query,
                    const QList<QPair<QString, QString>>& headers = {},
                    const RedirectValidator& redirectValidator = {});

    // --- TOFU certificate pinning ----------------------------------------
    //
    // THIS IS AN ISSUER PIN, NOT A PIN ON THE RELAY'S OWN KEY, and the
    // difference is the whole of what it does and does not prove.
    //
    // Trust-on-first-use: an SPKI SHA-256 is captured from the handshake that
    // completes pairing, and every later request to that origin must match it.
    // Not a hardcoded pin -- this client talks to whatever server the user
    // paired with, so there is no build-time value to bake in.
    //
    // What is hashed is the SPKI of the LEAF'S ISSUER; see
    // pinnedSpkiFromChain() below for the measurement that forced that and for
    // the CDN constraint behind it. So what this establishes is continuity of
    // the AUTHORITY vouching for the relay, not continuity of the relay's own
    // key: it narrows trust from "any CA in the store" to "certificates issued
    // under this one intermediate", which is what catches hostile Wi-Fi and a
    // corporate MITM root. It does NOT uniquely identify mail.urlxl.com, and
    // it does not detect mis-issuance by that same CA.
    //
    // This comment used to describe a leaf pin and say the SPKI form let the
    // pin "survive an ordinary certificate renewal that keeps the same key".
    // Both were true when it was written and neither is now: the anchor moved
    // to the issuer precisely BECAUSE Cloudflare's renewals do not keep the
    // same key. Read as a live claim it credited this pin with proving
    // something it never checks.
    //
    // SPKI rather than whole-certificate is still the right form: it survives
    // a re-issue of the intermediate that keeps its key, which a
    // full-certificate hash would not. Verified that QSslKey::toDer() on a
    // certificate's public key yields SubjectPublicKeyInfo DER, byte-identical
    // to `openssl pkey -pubin -outform der`, so this hash matches what
    // standard pin-generation tooling (and kypost-android's OkHttp
    // CertificatePinner) produces.

    // Empty spkiSha256 disables enforcement. Set from the stored pairing at
    // startup.
    //
    // The pin is scoped to `origin` (scheme+host+port) and enforced ONLY on
    // requests to that origin. It describes the authority vouching for the
    // paired relay's origin and nothing else -- see the block above for why
    // that is a weaker statement than "the paired relay". Enforcing it on every reply made the PGP QR key-exchange feature
    // -- which deliberately fetches from other servers
    // (PgpQrController::scanQrPayload) -- impossible on any paired device,
    // and worse, raised the persistent "your mail server's certificate
    // changed, unpair and pair again" banner on a scan of any third-party
    // QR code. That is an attacker-triggerable false alarm that talks the
    // user into exactly the unpair/re-pair the pairing-hijack findings need.
    void setCertificatePin(const QByteArray& spkiSha256, const QUrl& origin);
    QByteArray certificatePin() const;

    // The pin AND the origin it is scoped to, as one value.
    //
    // certificatePin() alone cannot round-trip the enforcement state,
    // because a pin without its origin is not restorable -- which is how
    // DeviceRegistrationService::pair() came to disable pinning
    // permanently: it clears the pin before registering (it must -- the
    // registration is what establishes the new anchor) and only re-armed it
    // on the success path, so ANY failed re-registration left enforcement
    // off for the rest of the process. reregisterIfPaired() runs unattended
    // on every push-endpoint rotation, so an on-path attacker needed only
    // to make one of those fail.
    struct CertificatePinState
    {
        QByteArray spkiSha256;
        QUrl origin;

        bool isEnforcing() const { return !spkiSha256.isEmpty(); }
    };

    CertificatePinState certificatePinState() const;
    void restoreCertificatePin(const CertificatePinState& state);

    // The SPKI this client pins for a given peer chain: the SHA-256 of the
    // LEAF'S ISSUER, not of the leaf.
    //
    // The relay is behind Cloudflare (AGENTS.md section 8), which terminates
    // TLS on a ~90-day Universal SSL certificate and generates a new key at
    // every renewal. Pinning the leaf therefore fired "this server is being
    // impersonated" roughly quarterly with no attacker involved -- measured
    // on the live endpoint, a leaf valid 2026-07-20/2026-10-18 under an
    // issuer (Google Trust Services WE1) valid until 2029. A quarterly false
    // alarm whose only remedy is a "reconnect" button teaches the user to
    // click through the one dialog that exists to stop a real impersonation,
    // which is worse than not pinning.
    //
    // Pinning the issuer keeps the defence that applies to a CDN-fronted
    // origin -- a chain from some OTHER CA, i.e. hostile Wi-Fi or a
    // corporate MITM root -- and lets rotation pass unnoticed. It does not
    // detect mis-issuance by that same CA. That is the deliberate trade, and
    // it is why this is the issuer rather than the root, which any public
    // site under the same root would satisfy.
    //
    // Empty means "cannot pin", and every caller treats that as a failure
    // rather than a pass: deriving a pin from nothing is what turns
    // trust-on-first-use into trust-on-every-use.
    static QByteArray pinnedSpkiFromChain(const QList<QSslCertificate>& chain);

    // Drops the in-memory pin and its origin. Must be called wherever the
    // trust anchor is discarded or re-established -- unpairing, and before a
    // registration request, whose whole purpose is to establish a new one.
    // Without this, the certificate-mismatch banner's own instruction
    // ("remove this pairing and pair again") could not succeed: the re-pair
    // POST met the relay's new certificate, the stale in-process pin aborted
    // it, and the banner came straight back. Only a full process restart
    // cleared it, which the UI never mentions and minimize-to-tray hides.
    void clearCertificatePin();

    // Invoked (on the calling thread, from inside the blocking call) every
    // time a request is aborted because the peer's SPKI did not match the
    // pin. Exists because a mismatch is not a per-request problem: it stops
    // EVERY request, permanently, until the device re-pairs, and the
    // per-request NetworkError alone gave the user a generic "Refresh
    // failed" with no explanation and no way out. main.cpp routes this to a
    // persistent banner offering re-pairing. Empty by default; core/ has no
    // opinion about what the app does with it.
    // Carries the SPKI SHA-256 actually presented by the server that failed
    // the pin, so the UI can show the user what changed instead of only that
    // something did. A recovery action that asks "trust this new
    // certificate?" without naming it is a confirmation the user cannot
    // reason about -- they need the expected and the observed fingerprint
    // side by side. Empty only if the peer key could not be read at all.
    using CertificateMismatchHandler = std::function<void(const QByteArray& observedSpkiSha256)>;
    void setCertificateMismatchHandler(CertificateMismatchHandler handler);

private:
    // The thread this client was constructed on, and the only one that may
    // touch it. Checked by Q_ASSERT on every public entry point.
    //
    // This exists because ThreadSanitizer cannot do the job here. Measured,
    // not assumed: Qt is not built with -fsanitize=thread, so the
    // happens-before edges its event queue establishes are invisible, and a
    // 20-line program that does nothing but hand a std::function to a worker
    // thread via QMetaObject::invokeMethod produces 11 race reports. Two
    // different suppression sets were tried against a probe carrying both a
    // real race and the Qt-mediated false positives; both cut the real
    // reports as well (12 -> 6 and 12 -> 4), and TSan happens-before
    // annotations at our own seam only moved 11 -> 10 because the residual
    // reports are Qt's internal QMetaCallEvent storage, which cannot be
    // annotated from outside. See docs/THREADING.md.
    //
    // An affinity assertion is strictly better for the defect that actually
    // matters -- an HttpClient being touched from the wrong thread, which is
    // how the pin could be written while a request reads it mid-handshake.
    // It is deterministic, fires in every debug run including all 76 tests,
    // and has no false positives at all.
    void assertOwningThread(const char* where) const;
    QThread* m_owningThread = nullptr;

    QByteArray m_certificatePin;
    QUrl m_pinnedOrigin;
    CertificateMismatchHandler m_certificateMismatchHandler;
    // Appends query items to url via QUrlQuery, preserving any query url
    // already has — mirrors the Swift URL.appending(queryOrThrow:) extension.
    QUrl urlWithQuery(const QUrl& url, const QList<QPair<QString, QString>>& query) const;

    // maxResponseBytes is passed explicitly rather than read from
    // m_maxResponseBytes so the per-call override on get() has one place to
    // take effect, and so no verb can silently forget to apply a ceiling.
    HttpResult waitForReply(QNetworkReply* reply, const RedirectValidator& redirectValidator,
                             qint64 maxResponseBytes) const;

    // Returns redirectValidator when the caller supplied one, otherwise a
    // same-origin-as-requestUrl validator. Never returns empty, so every
    // request runs under UserVerifiedRedirectPolicy.
    static RedirectValidator effectiveRedirectValidator(const QUrl& requestUrl,
                                                        const RedirectValidator& redirectValidator);

    QNetworkAccessManager& m_manager;
    qint64 m_maxResponseBytes;
};

// scheme+host+port equality. Shared by HttpClient's redirect default, the
// pin's origin scoping, DeviceRegistrationService's pullEndpoint check and
// PairingController's reg/srv check, which all used to hand-roll it.
bool sameUrlOrigin(const QUrl& a, const QUrl& b);

// Appends apiPath to baseUrl's path -- preserves any existing path on
// baseUrl and ensures exactly one slash between the two, regardless of
// whether the caller's base URL was given with or without a trailing
// slash. Shared by every Relay HTTP client's endpointFor()-style helper
// (GroupsClient, ContactSyncClient, ContactPhotoClient, RelayMailSource),
// which all used to hand-roll this same join.
//
// apiPath MUST already be percent-encoded. Every caller but one passes a
// fixed ASCII literal, for which that is a no-op; the one that interpolates
// a runtime value (ContactPhotoClient's contact uid) has to encode it to a
// single segment first, because this function will not do it for them -- an
// unencoded '/' is a segment separator here, not data.
QUrl joinUrlPath(const QUrl& baseUrl, const QString& apiPath);

// Parses body as JSON and returns its top-level value as a QJsonObject, or
// nullopt if the body isn't valid JSON or its top-level value isn't an
// object. When errorString is non-null and decoding fails, it's set to the
// QJsonParseError's message so callers can fold it into their own
// human-readable detail text (existing wording/error-code mapping at each
// call site is preserved -- this only replaces the parse-and-validate
// boilerplate, not what callers do with the result).
std::optional<QJsonObject> decodeJsonObject(const QByteArray& body, QString* errorString = nullptr);

// Same as decodeJsonObject, but for a top-level JSON array.
std::optional<QJsonArray> decodeJsonArray(const QByteArray& body, QString* errorString = nullptr);
