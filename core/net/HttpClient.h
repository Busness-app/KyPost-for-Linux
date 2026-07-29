#pragma once

#include "net/NetworkError.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
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
//     it is suspended (core/util/ReentrancyGuard.h exists for this, and
//     guards a method against itself only);
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
// MfaController for the shape and docs/THREADING.md for the order and the
// two constraints that decide it.
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

    // transferTimeoutMs guards waitForReply()'s QEventLoop against a
    // hung/silent server that never emits QNetworkReply::finished (no
    // response, no error) -- without it, the calling thread blocks forever.
    // Only applied to the injected manager if it doesn't already have a
    // transfer timeout configured (manager.transferTimeout() == 0), so a
    // caller's own configuration is never clobbered. Exposed as a
    // constructor parameter (rather than hardcoded) so tests can pass a
    // short override instead of waiting out the real default.
    explicit HttpClient(QNetworkAccessManager& manager, int transferTimeoutMs = 30000);

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
    HttpResult get(const QUrl& url, const QList<QPair<QString, QString>>& query,
                   const QList<QPair<QString, QString>>& headers = {},
                   const RedirectValidator& redirectValidator = {});

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

    // --- TOFU certificate pinning ---------------------------------------
    // Trust-on-first-use: the SPKI SHA-256 of the server's TLS certificate
    // is captured when the device pairs, then every later request must
    // match it. Not a hardcoded pin -- this client talks to whatever server
    // the user paired with, so there is no build-time value to bake in.
    //
    // SPKI rather than whole-certificate: the pin then survives an ordinary
    // certificate renewal that keeps the same key, which a full-certificate
    // hash would not. Verified that QSslKey::toDer() on a certificate's
    // public key yields SubjectPublicKeyInfo DER, byte-identical to
    // `openssl pkey -pubin -outform der`, so this hash matches what standard
    // pin-generation tooling (and kypost-android's OkHttp CertificatePinner)
    // produces.

    // Empty spkiSha256 disables enforcement. Set from the stored pairing at
    // startup.
    //
    // The pin is scoped to `origin` (scheme+host+port) and enforced ONLY on
    // requests to that origin. It describes the paired relay and nothing
    // else. Enforcing it on every reply made the PGP QR key-exchange feature
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
    using CertificateMismatchHandler = std::function<void()>;
    void setCertificateMismatchHandler(CertificateMismatchHandler handler);

private:
    QByteArray m_certificatePin;
    QUrl m_pinnedOrigin;
    CertificateMismatchHandler m_certificateMismatchHandler;
    // Appends query items to url via QUrlQuery, preserving any query url
    // already has — mirrors the Swift URL.appending(queryOrThrow:) extension.
    QUrl urlWithQuery(const QUrl& url, const QList<QPair<QString, QString>>& query) const;

    HttpResult waitForReply(QNetworkReply* reply, const RedirectValidator& redirectValidator) const;

    // Returns redirectValidator when the caller supplied one, otherwise a
    // same-origin-as-requestUrl validator. Never returns empty, so every
    // request runs under UserVerifiedRedirectPolicy.
    static RedirectValidator effectiveRedirectValidator(const QUrl& requestUrl,
                                                        const RedirectValidator& redirectValidator);

    QNetworkAccessManager& m_manager;
};

// scheme+host+port equality. Shared by HttpClient's redirect default, the
// pin's origin scoping, DeviceRegistrationService's pullEndpoint check and
// PairingController's reg/srv check, which all used to hand-roll it.
bool sameUrlOrigin(const QUrl& a, const QUrl& b);

// Appends apiPath to baseUrl's path -- preserves any existing path on
// baseUrl and ensures exactly one slash between the two, regardless of
// whether the caller's base URL was given with or without a trailing
// slash. Shared by every Relay HTTP client's endpointFor()-style helper
// (GroupsClient, MfaResponseClient, ContactSyncClient, ContactPhotoClient,
// RelayMailSource), which all used to hand-roll this same join.
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
