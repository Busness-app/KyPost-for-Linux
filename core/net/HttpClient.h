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
// instead of a signal/callback chain. Callers must invoke get()/post() off
// the GUI thread once app/ wiring exists in a later phase.
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
    HttpResult get(const QUrl& url, const QList<QPair<QString, QString>>& query,
                   const QList<QPair<QString, QString>>& headers = {},
                   const RedirectValidator& redirectValidator = {});

    // Sets Content-Type: application/json.
    HttpResult post(const QUrl& url, const QList<QPair<QString, QString>>& query,
                     const QJsonObject& jsonBody, const QList<QPair<QString, QString>>& headers = {});

    // Sets Content-Type: application/json. Mirrors post()'s signature shape
    // -- added in Task 17 for the folder-rename endpoint.
    HttpResult put(const QUrl& url, const QList<QPair<QString, QString>>& query,
                    const QJsonObject& jsonBody, const QList<QPair<QString, QString>>& headers = {});

    // No body -- mirrors get()'s signature shape. Added in Task 17 for the
    // folder-delete endpoint, which takes its target via query param.
    HttpResult del(const QUrl& url, const QList<QPair<QString, QString>>& query,
                    const QList<QPair<QString, QString>>& headers = {});

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

    // Empty disables enforcement. Set from the stored pairing at startup.
    void setCertificatePin(const QByteArray& spkiSha256);
    QByteArray certificatePin() const;

    // SPKI SHA-256 seen on the most recent TLS handshake, or empty if the
    // last request was plaintext or never connected. This is the value the
    // pairing flow captures.
    QByteArray lastPeerSpkiSha256() const;

private:
    QByteArray m_certificatePin;
    mutable QByteArray m_lastPeerSpkiSha256;
    // Appends query items to url via QUrlQuery, preserving any query url
    // already has — mirrors the Swift URL.appending(queryOrThrow:) extension.
    QUrl urlWithQuery(const QUrl& url, const QList<QPair<QString, QString>>& query) const;

    HttpResult waitForReply(QNetworkReply* reply, const RedirectValidator& redirectValidator = {}) const;

    QNetworkAccessManager& m_manager;
};

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
