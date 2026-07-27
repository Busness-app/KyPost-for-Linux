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
}

QTEST_GUILESS_MAIN(HttpClientTest)
#include "HttpClientTest.moc"
