#include "net/HttpClient.h"
#include "net/NetworkError.h"

#include "FakeRelayServer.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

class HttpClientTest : public QObject
{
    Q_OBJECT

private slots:
    void getSuccessReturnsBodyUnmodifiedAndPreservesExistingQuery();
    void getMapsUnauthorizedFrom401();
    void postSendsJsonBodyAndContentTypeHeader();
    void putSendsJsonBodyAndContentTypeHeader();
    void delSendsQueryParamsWithNoBody();
    void transportFailureWhenNothingListens();
    void transportFailureWhenServerHangs();
    void getFollowsRedirectWhenValidatorApprovesTarget();
    void getDoesNotFollowRedirectWhenValidatorRejectsTarget();
    void crossOriginRedirectIsRefusedByDefault();
    void refusesAResponseLargerThanTheLimit();
    void refusesAChunkedResponseThatDeclaresNoLength();
    void acceptsAResponseExactlyAtTheLimit();
    void perCallLimitOverridesTheClientDefault();
};

void HttpClientTest::getSuccessReturnsBodyUnmodifiedAndPreservesExistingQuery()
{
    const QByteArray body = "{\"ok\":true}";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient client(manager);

    // url already carries a query item; get() must append sub/hash rather
    // than replacing it.
    QUrl url(QStringLiteral("http://127.0.0.1:%1/api/thing").arg(fake.port()));
    url.setQuery(QStringLiteral("existing=1"));

    const HttpClient::HttpResult result =
        client.get(url, { { QStringLiteral("sub"), QStringLiteral("abc") } });

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.statusCode, 200);
    QCOMPARE(result.body, body);
    QVERIFY(result.detail.isEmpty());
    QVERIFY(fake.receivedRequest().contains(
        "GET /api/thing?existing=1&sub=abc HTTP/1.1"));
}

void HttpClientTest::getMapsUnauthorizedFrom401()
{
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "{}"));
    QNetworkAccessManager manager;
    HttpClient client(manager);

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/thing").arg(fake.port()));
    const HttpClient::HttpResult result = client.get(url, {});

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::Unauthorized);
    QCOMPARE(result.statusCode, 401);
}

void HttpClientTest::postSendsJsonBodyAndContentTypeHeader()
{
    FakeRelayServer fake(httpResponse(200, "OK", "{}"));
    QNetworkAccessManager manager;
    HttpClient client(manager);

    QJsonObject json;
    json[QStringLiteral("challengeId")] = QStringLiteral("chal-1");
    const QByteArray expectedBody = QJsonDocument(json).toJson(QJsonDocument::Compact);

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/mfa/respond").arg(fake.port()));
    const HttpClient::HttpResult result =
        client.post(url, { { QStringLiteral("sub"), QStringLiteral("s1") } }, json);

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.statusCode, 200);

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("POST /api/mfa/respond?sub=s1 HTTP/1.1"));
    QVERIFY(request.contains("Content-Type: application/json"));
    QVERIFY(request.endsWith(expectedBody));
}

void HttpClientTest::putSendsJsonBodyAndContentTypeHeader()
{
    FakeRelayServer fake(httpResponse(200, "OK", "{}"));
    QNetworkAccessManager manager;
    HttpClient client(manager);

    QJsonObject json;
    json[QStringLiteral("name")] = QStringLiteral("NewName");
    const QByteArray expectedBody = QJsonDocument(json).toJson(QJsonDocument::Compact);

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/inbox/folders").arg(fake.port()));
    const HttpClient::HttpResult result =
        client.put(url, { { QStringLiteral("sub"), QStringLiteral("s1") } }, json);

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.statusCode, 200);

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("PUT /api/inbox/folders?sub=s1 HTTP/1.1"));
    QVERIFY(request.contains("Content-Type: application/json"));
    QVERIFY(request.endsWith(expectedBody));
}

void HttpClientTest::delSendsQueryParamsWithNoBody()
{
    FakeRelayServer fake(httpResponse(200, "OK", "{}"));
    QNetworkAccessManager manager;
    HttpClient client(manager);

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/inbox/folders").arg(fake.port()));
    const HttpClient::HttpResult result = client.del(url, { { QStringLiteral("folder"), QStringLiteral("OldFolder") } });

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.statusCode, 200);

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("DELETE /api/inbox/folders?folder=OldFolder HTTP/1.1"));
    QVERIFY(!request.contains("Content-Length:"));
}

void HttpClientTest::transportFailureWhenNothingListens()
{
    // Grab an ephemeral port, then close the listener immediately so
    // nothing is listening on it when the client connects.
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost));
    const quint16 freePort = probe.serverPort();
    probe.close();

    QNetworkAccessManager manager;
    HttpClient client(manager);
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/thing").arg(freePort));
    const HttpClient::HttpResult result = client.get(url, {});

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::Transport);
    QCOMPARE(result.statusCode, 0);
    QVERIFY(!result.detail.isEmpty());
}

void HttpClientTest::transportFailureWhenServerHangs()
{
    // Accepts the connection but never writes a response -- simulates a
    // hung/silent server, which used to leave waitForReply()'s QEventLoop
    // spinning forever since nothing ever emits QNetworkReply::finished.
    // Uses a short (100ms) constructor-injected timeout override so this
    // test stays fast/deterministic rather than waiting out the real
    // 30-second default.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QTcpSocket* accepted = nullptr;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &accepted]() {
        accepted = server.nextPendingConnection();
    });

    QNetworkAccessManager manager;
    HttpClient client(manager, 100);

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/thing").arg(server.serverPort()));
    const HttpClient::HttpResult result = client.get(url, {});

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::Transport);
    QCOMPARE(result.statusCode, 0);
    QVERIFY(!result.detail.isEmpty());

    delete accepted;
}

void HttpClientTest::getFollowsRedirectWhenValidatorApprovesTarget()
{
    const QByteArray finalBody = "{\"ok\":true,\"from\":\"final\"}";
    FakeRelayServer finalServer(httpResponse(200, "OK", finalBody));

    const QByteArray location =
        QStringLiteral("http://127.0.0.1:%1/final").arg(finalServer.port()).toUtf8();
    FakeRelayServer redirectingServer(
        httpResponse(302, "Found", "", "text/plain", { { "Location", location } }));

    QNetworkAccessManager manager;
    HttpClient client(manager);

    QStringList approvedTargets;
    const HttpClient::RedirectValidator approveAll = [&approvedTargets](const QUrl& target) {
        approvedTargets.append(target.toString());
        return true;
    };

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/start").arg(redirectingServer.port()));
    const HttpClient::HttpResult result = client.get(url, {}, {}, approveAll);

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.statusCode, 200);
    QCOMPARE(result.body, finalBody);
    QCOMPARE(approvedTargets.size(), 1);
    QVERIFY(approvedTargets.first().contains(QStringLiteral("/final")));
}

void HttpClientTest::getDoesNotFollowRedirectWhenValidatorRejectsTarget()
{
    // VibeSec regression guard: a redirect target must be re-validated by
    // the caller-supplied validator, not followed blindly -- otherwise a
    // URL that legitimately passes an initial safety check (e.g.
    // isSafeQrTarget) could still redirect the actual request to a
    // link-local/metadata address.
    const QByteArray finalBody = "{\"ok\":true,\"from\":\"final\"}";
    FakeRelayServer finalServer(httpResponse(200, "OK", finalBody));

    const QByteArray location =
        QStringLiteral("http://127.0.0.1:%1/final").arg(finalServer.port()).toUtf8();
    FakeRelayServer redirectingServer(
        httpResponse(302, "Found", "", "text/plain", { { "Location", location } }));

    QNetworkAccessManager manager;
    HttpClient client(manager);

    const HttpClient::RedirectValidator rejectAll = [](const QUrl&) { return false; };

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/start").arg(redirectingServer.port()));
    const HttpClient::HttpResult result = client.get(url, {}, {}, rejectAll);

    // The redirect was never followed, and the rejection surfaces as a
    // clear failure -- not a misleadingly "successful" 302 response the
    // caller might mistake for legitimate data, and definitely not the
    // final server's body.
    QVERIFY(result.error.has_value());
    QVERIFY(result.body != finalBody);
    // ...and it is named for what happened. Aborting mid-redirect leaves
    // the 3xx as the reply's status, which the status-code mapping turns
    // into a generic Server error -- so "the relay tried to send the device
    // secret to another host" and "the relay returned a 500" used to be the
    // same value to every caller and every log line.
    QCOMPARE(*result.error, NetworkError::RedirectRefused);
}

void HttpClientTest::crossOriginRedirectIsRefusedByDefault()
{
    // Qt's default (NoLessSafeRedirectPolicy) follows cross-HOST redirects
    // and strips nothing: every redirect status forwards custom headers --
    // including X-Kypost-Device-Secret -- and 307/308 forward the body too,
    // which on the registration POST is the subscriber id, the pairing token
    // and the UnifiedPush endpoint. A caller that names no validator now gets
    // a same-origin-only default instead.
    const QByteArray finalBody = "{\"ok\":true,\"from\":\"final\"}";
    FakeRelayServer finalServer(httpResponse(200, "OK", finalBody));

    const QByteArray location =
        QStringLiteral("http://127.0.0.1:%1/final").arg(finalServer.port()).toUtf8();
    FakeRelayServer redirectingServer(
        httpResponse(302, "Found", "", "text/plain", { { "Location", location } }));

    QNetworkAccessManager manager;
    HttpClient client(manager);

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/start").arg(redirectingServer.port()));
    const HttpClient::HttpResult result =
        client.get(url, {}, { { QStringLiteral("X-Kypost-Device-Secret"), QStringLiteral("top-secret") } });

    // Refused, and the secret never reached the other origin.
    QVERIFY(result.body != finalBody);
    QVERIFY(!finalServer.receivedRequest().contains("top-secret"));
    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::RedirectRefused);
}


// A relay is inside this app's threat model everywhere else, so "the relay
// would never send that much" is not a bound. QNetworkReply buffers the whole
// body in memory and readAll() handed over whatever arrived, with nothing
// anywhere saying no -- one hostile response was an unbounded allocation.
void HttpClientTest::refusesAResponseLargerThanTheLimit()
{
    const QByteArray body(4096, 'x');
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QNetworkAccessManager manager;
    HttpClient client(manager, 5000, /*maxResponseBytes=*/1024);

    const HttpClient::HttpResult result =
        client.get(QUrl(QStringLiteral("http://127.0.0.1:%1/big").arg(fake.port())), {});

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::ResponseTooLarge);
    // The partial body must NOT come back: half a JSON document is not a
    // smaller JSON document, and returning one is how a size guard becomes a
    // silent data-corruption bug.
    QVERIFY(result.body.isEmpty());
}

// Content-Length is the cheap check and cannot be the only one: it is
// OPTIONAL. A chunked response declares no length at all, so the header guard
// has nothing to look at and the running byte count is the sole defence.
//
// (An under-declared Content-Length is not the gap it looks like -- measured:
// Qt stops reading at the declared length and discards the rest, so a server
// claiming 8 bytes and sending 8192 cannot make this client allocate 8192.
// The first version of this test asserted exactly that and failed, correctly.
// Over-declaring IS caught, at the headers, before any body arrives.)
void HttpClientTest::refusesAChunkedResponseThatDeclaresNoLength()
{
    QByteArray response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: application/json\r\n";
    response += "Transfer-Encoding: chunked\r\n";
    response += "Connection: close\r\n\r\n";
    // 16 chunks of 512 bytes = 8192, well past the 1024 ceiling, with no
    // total ever declared.
    for (int i = 0; i < 16; ++i) {
        response += QByteArray::number(512, 16) + "\r\n";
        response += QByteArray(512, 'x') + "\r\n";
    }
    response += "0\r\n\r\n";

    FakeRelayServer fake(response);

    QNetworkAccessManager manager;
    HttpClient client(manager, 5000, /*maxResponseBytes=*/1024);

    const HttpClient::HttpResult result =
        client.get(QUrl(QStringLiteral("http://127.0.0.1:%1/chunked").arg(fake.port())), {});

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::ResponseTooLarge);
    QVERIFY(result.body.isEmpty());
}

// The boundary is inclusive: a response exactly at the ceiling is legitimate.
// Pinned because an off-by-one here rejects real mail rather than an attack,
// which is the failure mode nobody notices until a user cannot sync.
void HttpClientTest::acceptsAResponseExactlyAtTheLimit()
{
    const QByteArray body(1024, 'x');
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QNetworkAccessManager manager;
    HttpClient client(manager, 5000, /*maxResponseBytes=*/1024);

    const HttpClient::HttpResult result =
        client.get(QUrl(QStringLiteral("http://127.0.0.1:%1/exact").arg(fake.port())), {});

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.body.size(), 1024);
}

// The inbox window and an attachment download legitimately dwarf the default,
// and say so at their own call sites (RelayMailSource). Everything else keeps
// the tight ceiling.
void HttpClientTest::perCallLimitOverridesTheClientDefault()
{
    const QByteArray body(4096, 'x');
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QNetworkAccessManager manager;
    HttpClient client(manager, 5000, /*maxResponseBytes=*/1024);

    const HttpClient::HttpResult result =
        client.get(QUrl(QStringLiteral("http://127.0.0.1:%1/inbox").arg(fake.port())), {}, {}, {},
                    /*maxResponseBytes=*/8192);

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.body.size(), 4096);
}

QTEST_GUILESS_MAIN(HttpClientTest)
#include "HttpClientTest.moc"
