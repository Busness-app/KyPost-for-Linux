#include "net/HttpClient.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCryptographicHash>
#include <QSslCertificate>
#include <QSslConfiguration>
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

// SHA-256 of a certificate's SubjectPublicKeyInfo, or empty when the key
// cannot be read. QSslCertificate::publicKey() returns a NULL QSslKey when
// the backend cannot represent the key type, and QSslKey::toDer() on a null
// key returns an empty QByteArray -- hashing that yields SHA256("") =
// e3b0c442..., a fixed public constant that EVERY unparseable certificate
// would also satisfy. So the empty case propagates as empty, never as a hash.
QByteArray spkiSha256(const QSslCertificate& cert)
{
    if (cert.isNull())
        return {};
    const QByteArray der = cert.publicKey().toDer();
    return der.isEmpty() ? QByteArray() : QCryptographicHash::hash(der, QCryptographicHash::Sha256);
}

void applyDefaultHeaders(QNetworkRequest& request, const QList<QPair<QString, QString>>& headers)
{
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    for (const auto& header : headers)
        request.setRawHeader(header.first.toUtf8(), header.second.toUtf8());
}

} // namespace

void HttpClient::assertOwningThread(const char* where) const
{
    if (Q_LIKELY(QThread::currentThread() == m_owningThread))
        return;

    // Deliberately NOT a bare Q_ASSERT_X. The default build type here is
    // Release, which defines NDEBUG and compiles Q_ASSERT out entirely -- so
    // an assert-only check would be absent from exactly the build users run,
    // and its presence in the source would be misleading. The condition is
    // evaluated unconditionally and reported unconditionally; the abort is
    // the debug-build extra.
    //
    // Worth this much noise because the failure is silent and
    // security-relevant: the certificate pin is read mid-handshake by a
    // request on the owning thread, so a write from anywhere else is a data
    // race on the value that decides whether the device secret goes to the
    // right server.
    qCritical("%s: HttpClient touched from a thread other than the one that constructed it. Its "
              "certificate-pin state is read mid-handshake, so this is a data race on the value "
              "that decides where the device secret is sent. Use NetworkExecutor::configure() or "
              "run() to reach the executor's client.",
              where);
    Q_ASSERT_X(false, where, "HttpClient used from the wrong thread");
}

HttpClient::HttpClient(QNetworkAccessManager& manager, int transferTimeoutMs, qint64 maxResponseBytes)
    : m_owningThread(QThread::currentThread())
    , m_manager(manager)
    , m_maxResponseBytes(maxResponseBytes)
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
    assertOwningThread("HttpClient::setCertificatePin");
    m_certificatePin = spkiSha256;
    m_pinnedOrigin = origin;
}

QByteArray HttpClient::certificatePin() const
{
    assertOwningThread("HttpClient::certificatePin");
    return m_certificatePin;
}

QByteArray HttpClient::pinnedSpkiFromChain(const QList<QSslCertificate>& chain)
{
    // chain[0] is the leaf and chain[1] is what signed it: TLS requires the
    // chain be sent leaf-first with each certificate certifying the one
    // before it, and Qt preserves that order. Verified against the live
    // relay rather than assumed -- peerCertificateChain() returned
    // [urlxl.com, WE1, GTS Root R4] with peerCertificate() == chain[0],
    // on fresh AND pooled keep-alive connections alike.
    if (chain.size() < 2)
        return {};

    const QSslCertificate& leaf = chain.at(0);
    const QSslCertificate& issuer = chain.at(1);

    // Cheap guard against pinning the wrong link. A chain that arrives
    // reordered or cross-signed could otherwise anchor us to the ROOT, which
    // every public site under that root would satisfy -- a silent widening
    // of trust rather than a visible failure. Fail closed instead.
    if (issuer.subjectDisplayName() != leaf.issuerDisplayName())
        return {};

    return spkiSha256(issuer);
}

HttpClient::CertificatePinState HttpClient::certificatePinState() const
{
    assertOwningThread("HttpClient::certificatePinState");
    return CertificatePinState{ m_certificatePin, m_pinnedOrigin };
}

void HttpClient::restoreCertificatePin(const CertificatePinState& state)
{
    assertOwningThread("HttpClient::restoreCertificatePin");
    m_certificatePin = state.spkiSha256;
    m_pinnedOrigin = state.origin;
}

void HttpClient::clearCertificatePin()
{
    assertOwningThread("HttpClient::clearCertificatePin");
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
    assertOwningThread("HttpClient::setCertificateMismatchHandler");
    m_certificateMismatchHandler = std::move(handler);
}

HttpClient::HttpResult HttpClient::get(const QUrl& url, const QList<QPair<QString, QString>>& query,
                                        const QList<QPair<QString, QString>>& headers,
                                        const RedirectValidator& redirectValidator,
                                        std::optional<qint64> maxResponseBytes)
{
    assertOwningThread("HttpClient::get");
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

    return waitForReply(m_manager.get(request), effectiveRedirectValidator(requestUrl, redirectValidator),
                         maxResponseBytes.value_or(m_maxResponseBytes));
}

HttpClient::HttpResult HttpClient::post(const QUrl& url, const QList<QPair<QString, QString>>& query,
                                         const QJsonObject& jsonBody, const QList<QPair<QString, QString>>& headers,
                                         const RedirectValidator& redirectValidator)
{
    assertOwningThread("HttpClient::post");
    const QUrl requestUrl = urlWithQuery(url, query);
    if (!requestUrl.isValid())
        return HttpResult{ NetworkError::InvalidUrl, 0, {}, QStringLiteral("Invalid URL"), {}, {} };

    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    applyDefaultHeaders(request, headers);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);

    return waitForReply(m_manager.post(request, QJsonDocument(jsonBody).toJson(QJsonDocument::Compact)),
                        effectiveRedirectValidator(requestUrl, redirectValidator), m_maxResponseBytes);
}

HttpClient::HttpResult HttpClient::put(const QUrl& url, const QList<QPair<QString, QString>>& query,
                                        const QJsonObject& jsonBody, const QList<QPair<QString, QString>>& headers,
                                        const RedirectValidator& redirectValidator)
{
    assertOwningThread("HttpClient::put");
    const QUrl requestUrl = urlWithQuery(url, query);
    if (!requestUrl.isValid())
        return HttpResult{ NetworkError::InvalidUrl, 0, {}, QStringLiteral("Invalid URL"), {}, {} };

    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    applyDefaultHeaders(request, headers);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);

    return waitForReply(m_manager.put(request, QJsonDocument(jsonBody).toJson(QJsonDocument::Compact)),
                        effectiveRedirectValidator(requestUrl, redirectValidator), m_maxResponseBytes);
}

HttpClient::HttpResult HttpClient::del(const QUrl& url, const QList<QPair<QString, QString>>& query,
                                        const QList<QPair<QString, QString>>& headers,
                                        const RedirectValidator& redirectValidator)
{
    assertOwningThread("HttpClient::del");
    const QUrl requestUrl = urlWithQuery(url, query);
    if (!requestUrl.isValid())
        return HttpResult{ NetworkError::InvalidUrl, 0, {}, QStringLiteral("Invalid URL"), {}, {} };

    QNetworkRequest request(requestUrl);
    applyDefaultHeaders(request, headers);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);

    return waitForReply(m_manager.deleteResource(request),
                        effectiveRedirectValidator(requestUrl, redirectValidator), m_maxResponseBytes);
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

HttpClient::HttpResult HttpClient::waitForReply(QNetworkReply* reply, const RedirectValidator& redirectValidator,
                                                 qint64 maxResponseBytes) const
{
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // Response size ceiling, enforced on two separate facts because a hostile
    // server controls both and neither alone is sufficient:
    //
    //   * what it CLAIMS -- Content-Length, checked as soon as the headers
    //     arrive, so an outsized body is refused before a single byte of it
    //     is buffered. This is the cheap path and the common one.
    //   * what it SENDS -- downloadProgress, because Content-Length is
    //     optional (chunked transfer omits it) and, more to the point, a
    //     server that means harm can simply lie. Without this a declared
    //     1 KB followed by a gigabyte would sail through.
    //
    // QNetworkReply buffers the whole body in memory, so aborting is what
    // actually bounds the allocation; there is no streaming consumer here to
    // apply backpressure instead.
    bool tooLarge = false;
    const auto refuseIfOversized = [reply, maxResponseBytes, &tooLarge](qint64 bytes) {
        if (tooLarge || bytes <= maxResponseBytes)
            return;
        tooLarge = true;
        qWarning("HttpClient: refusing a response of %lld bytes; the limit for this request is %lld",
                  static_cast<long long>(bytes), static_cast<long long>(maxResponseBytes));
        reply->abort();
    };
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, refuseIfOversized]() {
        const QVariant declared = reply->header(QNetworkRequest::ContentLengthHeader);
        if (declared.isValid())
            refuseIfOversized(declared.toLongLong());
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                      [refuseIfOversized](qint64 received, qint64 /*total*/) { refuseIfOversized(received); });

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
    QByteArray observedSpki;
    const bool enforcePin = !m_certificatePin.isEmpty() && sameUrlOrigin(reply->request().url(), m_pinnedOrigin);
    QObject::connect(reply, &QNetworkReply::encrypted, reply,
                      [this, reply, enforcePin, &pinMismatch, &observedSpki]() {
        if (!enforcePin)
            return;

        const QSslConfiguration config = reply->sslConfiguration();
        const QList<QSslCertificate> chain = config.peerCertificateChain();
        if (chain.isEmpty())
            return;

        // What we pin now: the leaf's issuer. See
        // HttpClient::pinnedSpkiFromChain() for why it is not the leaf.
        // Empty means the chain could not be anchored at all, which is a
        // refusal and not a pass.
        const QByteArray issuerSpki = pinnedSpkiFromChain(chain);

        // Devices paired before the anchor moved carry a LEAF pin, and that
        // pin is still evidence this is the server they paired with -- so
        // honour it rather than greeting every existing install with an
        // impersonation banner on upgrade. It keeps working until the CDN
        // next rolls the leaf; the mismatch that follows is re-anchored by
        // the dialog's Reconnect, which re-registers and stores an ISSUER
        // pin, and it never fires again. One alarm, once, per legacy device.
        const QByteArray legacyLeafSpki = spkiSha256(chain.first());

        const bool anchored = (!issuerSpki.isEmpty() && issuerSpki == m_certificatePin)
            || (!legacyLeafSpki.isEmpty() && legacyLeafSpki == m_certificatePin);
        if (!anchored) {
            pinMismatch = true;
            // Recorded so the recovery UI can show what was actually
            // presented -- and it shows the value that WOULD be pinned, so
            // the fingerprint in the dialog is the one the user is agreeing
            // to. Stays empty when the chain could not be anchored, which is
            // a different situation and must not render as a fingerprint.
            observedSpki = issuerSpki;
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
    //
    // This is the value the pairing flow stores and pins, so it is the
    // ISSUER's SPKI, not the leaf's -- see pinnedSpkiFromChain(). Reading
    // the chain here rather than in ::encrypted matters for the same
    // keep-alive reason as above, and peerCertificateChain() was confirmed
    // populated on pooled reuses too, not just fresh handshakes.
    result.peerSpkiSha256 = pinnedSpkiFromChain(reply->sslConfiguration().peerCertificateChain());
    const QList<QNetworkReply::RawHeaderPair> rawHeaders = reply->rawHeaderPairs();
    result.headers.reserve(rawHeaders.size());
    for (const auto& header : rawHeaders)
        result.headers.append({ QString::fromLatin1(header.first), QString::fromLatin1(header.second) });

    // Checked before the status code: an aborted handshake yields no status,
    // and even if it somehow did, "wrong server" outranks whatever that
    // server said.
    if (tooLarge) {
        // Ranked above the status code for the same reason pinMismatch and
        // redirectRefused are: abort() leaves whatever status had arrived,
        // and "we stopped reading because it was too big" is the fact the
        // caller needs -- not a 200 with a truncated body, which is exactly
        // how a size guard turns into a silent data-corruption bug.
        // Deliberately discards the partial body: half a JSON document is
        // not a smaller JSON document.
        result.error = NetworkError::ResponseTooLarge;
        result.body.clear();
    } else if (pinMismatch) {
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
            m_certificateMismatchHandler(observedSpki);
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
    // Encoded in, encoded out. setPath()'s default DecodedMode encodes '?'
    // and '#' but NOT '/', which is a legal path character -- so a caller
    // splicing a runtime value into apiPath could add path segments, and a
    // value of "../../api/notifications/native/pull" rewrote the endpoint
    // outright. Working in encoded space means a caller CAN encode a value
    // to one segment and have that survive; see ContactPhotoClient.
    QUrl url = baseUrl;
    QString path = url.path(QUrl::FullyEncoded);
    if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    path += apiPath;
    url.setPath(path, QUrl::StrictMode);
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
