#include "net/HttpClient.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCryptographicHash>
#include <QSslCertificate>
#include <QSslKey>
#include <QUrlQuery>

namespace {

// The relay is fronted by a CDN that challenges/blocks requests carrying
// Qt's stock "Mozilla/5.0" QNetworkAccessManager User-Agent, so a real
// product token is mandatory rather than cosmetic (AGENTS.md section 8).
// The relay's own hostname is deliberately not named here: it is pairing
// data (DevicePairing::serverBaseUrl), not a compile-time constant, and
// self-hosted instances are a supported case.
// Applied to every verb below via applyDefaultHeaders(); a caller-supplied
// User-Agent in `headers` still wins, because setRawHeader() runs after
// this and overwrites.
QString userAgent()
{
    return QStringLiteral("KyPost/%1 (Linux)").arg(QStringLiteral(KYPOST_VERSION));
}

void applyDefaultHeaders(QNetworkRequest& request, const QList<QPair<QString, QString>>& headers)
{
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    for (const auto& header : headers)
        request.setRawHeader(header.first.toUtf8(), header.second.toUtf8());
}

} // namespace

HttpClient::HttpClient(QNetworkAccessManager& manager, int transferTimeoutMs)
    : m_manager(manager)
{
    // A manager with no transfer timeout configured (the Qt default) leaves
    // waitForReply()'s QEventLoop waiting on ::finished forever if a server
    // accepts the connection but never responds. transferTimeout() == 0
    // means "unset" per Qt docs, so only set it in that case -- a caller
    // that configured its own timeout on the injected manager keeps it.
    if (m_manager.transferTimeout() == 0)
        m_manager.setTransferTimeout(transferTimeoutMs);
}

void HttpClient::setCertificatePin(const QByteArray& spkiSha256, const QUrl& origin)
{
    m_certificatePin = spkiSha256;
    m_pinnedOrigin = origin;
}

QByteArray HttpClient::certificatePin() const
{
    return m_certificatePin;
}

HttpClient::CertificatePinState HttpClient::certificatePinState() const
{
    return CertificatePinState{ m_certificatePin, m_pinnedOrigin };
}

void HttpClient::restoreCertificatePin(const CertificatePinState& state)
{
    m_certificatePin = state.spkiSha256;
    m_pinnedOrigin = state.origin;
}

void HttpClient::clearCertificatePin()
{
    m_certificatePin.clear();
    m_pinnedOrigin = QUrl();
}

HttpClient::RedirectValidator HttpClient::effectiveRedirectValidator(const QUrl& requestUrl,
                                                                      const RedirectValidator& redirectValidator)
{
    if (redirectValidator)
        return redirectValidator;

    // Default: refuse to leave the origin the caller asked for. Qt forwards
    // custom headers (the device secret) on every redirect status, and the
    // body too on 307/308, so a cross-host hop hands the credential to a host
    // the caller never named.
    return [requestUrl](const QUrl& target) { return sameUrlOrigin(target, requestUrl); };
}

void HttpClient::setCertificateMismatchHandler(CertificateMismatchHandler handler)
{
    m_certificateMismatchHandler = std::move(handler);
}

HttpClient::HttpResult HttpClient::get(const QUrl& url, const QList<QPair<QString, QString>>& query,
                                        const QList<QPair<QString, QString>>& headers,
                                        const RedirectValidator& redirectValidator)
{
    const QUrl requestUrl = urlWithQuery(url, query);
    if (!requestUrl.isValid())
        return HttpResult{ NetworkError::InvalidUrl, 0, {}, QStringLiteral("Invalid URL"), {}, {} };

    QNetworkRequest request(requestUrl);
    applyDefaultHeaders(request, headers);

    // Qt's default (NoLessSafeRedirectPolicy) follows redirects
    // automatically with no way for the caller to inspect the target.
    // UserVerifiedRedirectPolicy (NOT ManualRedirectPolicy, which never
    // emits QNetworkReply::redirected() at all -- it just refuses to
    // follow, full stop) is the one that pauses and waits for
    // redirectAllowed(), letting waitForReply() below re-validate every hop
    // before it's followed. Always on now: a caller that supplies no
    // validator gets the same-origin default rather than Qt's blind follow.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);

    return waitForReply(m_manager.get(request), effectiveRedirectValidator(requestUrl, redirectValidator));
}

HttpClient::HttpResult HttpClient::post(const QUrl& url, const QList<QPair<QString, QString>>& query,
                                         const QJsonObject& jsonBody, const QList<QPair<QString, QString>>& headers,
                                         const RedirectValidator& redirectValidator)
{
    const QUrl requestUrl = urlWithQuery(url, query);
    if (!requestUrl.isValid())
        return HttpResult{ NetworkError::InvalidUrl, 0, {}, QStringLiteral("Invalid URL"), {}, {} };

    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    applyDefaultHeaders(request, headers);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);

    return waitForReply(m_manager.post(request, QJsonDocument(jsonBody).toJson(QJsonDocument::Compact)),
                        effectiveRedirectValidator(requestUrl, redirectValidator));
}

HttpClient::HttpResult HttpClient::put(const QUrl& url, const QList<QPair<QString, QString>>& query,
                                        const QJsonObject& jsonBody, const QList<QPair<QString, QString>>& headers,
                                        const RedirectValidator& redirectValidator)
{
    const QUrl requestUrl = urlWithQuery(url, query);
    if (!requestUrl.isValid())
        return HttpResult{ NetworkError::InvalidUrl, 0, {}, QStringLiteral("Invalid URL"), {}, {} };

    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    applyDefaultHeaders(request, headers);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);

    return waitForReply(m_manager.put(request, QJsonDocument(jsonBody).toJson(QJsonDocument::Compact)),
                        effectiveRedirectValidator(requestUrl, redirectValidator));
}

HttpClient::HttpResult HttpClient::del(const QUrl& url, const QList<QPair<QString, QString>>& query,
                                        const QList<QPair<QString, QString>>& headers,
                                        const RedirectValidator& redirectValidator)
{
    const QUrl requestUrl = urlWithQuery(url, query);
    if (!requestUrl.isValid())
        return HttpResult{ NetworkError::InvalidUrl, 0, {}, QStringLiteral("Invalid URL"), {}, {} };

    QNetworkRequest request(requestUrl);
    applyDefaultHeaders(request, headers);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);

    return waitForReply(m_manager.deleteResource(request),
                        effectiveRedirectValidator(requestUrl, redirectValidator));
}

bool sameUrlOrigin(const QUrl& a, const QUrl& b)
{
    return a.scheme() == b.scheme() && a.host() == b.host() && a.port() == b.port();
}

QUrl HttpClient::urlWithQuery(const QUrl& url, const QList<QPair<QString, QString>>& query) const
{
    if (query.isEmpty())
        return url;

    QUrlQuery urlQuery(url);
    for (const auto& item : query)
        urlQuery.addQueryItem(item.first, item.second);

    QUrl result = url;
    result.setQuery(urlQuery);
    return result;
}

HttpClient::HttpResult HttpClient::waitForReply(QNetworkReply* reply, const RedirectValidator& redirectValidator) const
{
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // TOFU pinning. ::encrypted fires after the handshake completes but
    // before any request body is written, so aborting here means a
    // mismatched server never receives the device credentials -- which is
    // the whole point, and is why this is not done by inspecting the reply
    // afterwards.
    //
    // Enforcement is scoped to the pinned origin: the pin describes the
    // paired relay, and applying it to a deliberately cross-server PGP QR
    // fetch only ever produces a false "your mail server is being
    // impersonated" alarm.
    bool pinMismatch = false;
    const bool enforcePin = !m_certificatePin.isEmpty() && sameUrlOrigin(reply->request().url(), m_pinnedOrigin);
    QObject::connect(reply, &QNetworkReply::encrypted, reply, [this, reply, enforcePin, &pinMismatch]() {
        if (!enforcePin)
            return;

        const QSslCertificate peer = reply->sslConfiguration().peerCertificate();
        if (peer.isNull())
            return;

        // QSslCertificate::publicKey() returns a NULL QSslKey when the
        // backend cannot represent the key type, and QSslKey::toDer() on a
        // null key returns an empty QByteArray. Hashing that yields
        // SHA256("") = e3b0c442... -- a fixed, public constant. Pinning it
        // would mean every other certificate Qt also fails to parse
        // satisfies the pin, silently degrading TOFU to trust-on-every-use.
        // Never derive a pin from nothing: refuse the connection when
        // enforcing, and leave lastPeerSpkiSha256 empty so the pairing flow
        // records "no pin" (enforcement off) rather than a bogus one.
        const QByteArray spkiDer = peer.publicKey().toDer();
        if (spkiDer.isEmpty() || QCryptographicHash::hash(spkiDer, QCryptographicHash::Sha256) != m_certificatePin) {
            pinMismatch = true;
            reply->abort();
        }
    });
    // UserVerifiedRedirectPolicy (set above in get(), only when
    // redirectValidator is non-empty -- NOT ManualRedirectPolicy, which
    // never emits redirected() at all; see get()'s own comment) means Qt
    // pauses and waits for redirectAllowed() before
    // following each hop -- re-run the same safety check against the
    // redirect target here rather than following it blindly. Not calling
    // redirectAllowed() leaves the reply completing with the redirect
    // response itself (e.g. the 302), which is exactly what "don't follow
    // this" should look like to the caller.
    bool redirectRefused = false;
    if (redirectValidator) {
        QObject::connect(reply, &QNetworkReply::redirected, reply,
                          [reply, redirectValidator, &redirectRefused](const QUrl& target) {
                              if (redirectValidator(target)) {
                                  reply->redirectAllowed();
                                  return;
                              }
                              // Recorded, not just aborted. Aborting leaves
                              // the 3xx as the reply's status, which the
                              // status-code mapping below turns into a
                              // generic Server error -- so this, the one
                              // condition on this path that means "somebody
                              // tried to send the device secret somewhere
                              // else", read identically to a 500.
                              redirectRefused = true;
                              qWarning("HttpClient: refused a redirect to %s -- outside the origin the "
                                       "caller named; the credential was not sent",
                                       qUtf8Printable(target.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery
                                                                      | QUrl::RemovePath)));
                              // Simply not calling redirectAllowed() leaves Qt waiting
                              // indefinitely for a decision under UserVerifiedRedirectPolicy
                              // (unlike ManualRedirectPolicy, this isn't "give up and
                              // return the 3xx response" -- it just never finishes) --
                              // abort() is what actually completes the reply once the
                              // target is rejected.
                              reply->abort();
                          });
    }
    loop.exec();

    HttpResult result;
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();

    // Per-reply, read after the fact rather than from the ::encrypted
    // handler: that signal fires once per TLS *connection*, so on a pooled
    // keep-alive reuse it never fires and a shared "last SPKI seen" member
    // would still hold whatever host handshook most recently. peerCertificate()
    // is populated on reused connections, so this is the value the pairing
    // flow can actually trust to describe the server that answered.
    if (const QSslCertificate peer = reply->sslConfiguration().peerCertificate(); !peer.isNull()) {
        const QByteArray spkiDer = peer.publicKey().toDer();
        // Never derive a pin from nothing: QSslCertificate::publicKey()
        // yields a null key when the backend cannot represent the key type,
        // and SHA256("") is a fixed public constant that every other
        // unparseable certificate would also satisfy.
        if (!spkiDer.isEmpty())
            result.peerSpkiSha256 = QCryptographicHash::hash(spkiDer, QCryptographicHash::Sha256);
    }
    const QList<QNetworkReply::RawHeaderPair> rawHeaders = reply->rawHeaderPairs();
    result.headers.reserve(rawHeaders.size());
    for (const auto& header : rawHeaders)
        result.headers.append({ QString::fromLatin1(header.first), QString::fromLatin1(header.second) });

    // Checked before the status code: an aborted handshake yields no status,
    // and even if it somehow did, "wrong server" outranks whatever that
    // server said.
    if (pinMismatch) {
        result.error = NetworkError::CertificateMismatch;
        // No `detail` string. This used to carry an English sentence, which
        // then travelled all the way to the UI as a user-facing message --
        // core/ cannot call i18n() (AGENTS.md section 5: KF6::I18n is an
        // app/ dependency), so the wording belongs in app/, and the enum
        // value alone is what core/ is entitled to report. The wording now
        // lives in the roots' certificate-mismatch banner.
        //
        // Reported out-of-band as well, because the enum reaches callers one
        // failed request at a time while this condition is global to the
        // pairing and needs a persistent explanation with a way out.
        if (m_certificateMismatchHandler)
            m_certificateMismatchHandler();
    } else if (redirectRefused) {
        // Ranked above the status code for the same reason the pin mismatch
        // is: the 3xx the reply is carrying describes what the server WANTED
        // to happen, not what did, and "we refused to follow that" is the
        // fact the caller needs.
        result.error = NetworkError::RedirectRefused;
    } else if (result.statusCode != 0) {
        // Got an HTTP response — map by status code even if QNetworkReply
        // also flagged an error of its own (e.g. 404 sets ContentNotFoundError).
        result.error = networkErrorFromStatusCode(result.statusCode);
    } else if (reply->error() != QNetworkReply::NoError) {
        result.error = NetworkError::Transport;
        result.detail = reply->errorString();
    }

    reply->deleteLater();
    return result;
}

QUrl joinUrlPath(const QUrl& baseUrl, const QString& apiPath)
{
    QUrl url = baseUrl;
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    path += apiPath;
    url.setPath(path);
    return url;
}

std::optional<QJsonObject> decodeJsonObject(const QByteArray& body, QString* errorString)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorString)
            *errorString = parseError.errorString();
        return std::nullopt;
    }
    return doc.object();
}

std::optional<QJsonArray> decodeJsonArray(const QByteArray& body, QString* errorString)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (errorString)
            *errorString = parseError.errorString();
        return std::nullopt;
    }
    return doc.array();
}
