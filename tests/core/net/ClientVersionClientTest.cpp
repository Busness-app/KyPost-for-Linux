#include "net/ClientVersionClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"
#include "FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTest>

class ClientVersionClientTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesLatestVersionAndSendsDeviceHeaders();
    void treatsMissingEndpointAsUnsupportedRatherThanError();
    void treatsEmptyLatestVersionAsNoInformation();
    void reportsTransportFailureAsError();
};

void ClientVersionClientTest::parsesLatestVersionAndSendsDeviceHeaders()
{
    FakeRelayServer fake(httpResponse(200, "OK",
        R"({"latestVersion":"0.3.0","checkedAt":"2026-08-25T12:00:00Z","error":""})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ClientVersionClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-9"), QStringLiteral("secret-9") };
    const ClientVersionResult result = client.fetch(serverBaseUrl, auth);

    QVERIFY(!result.error.has_value());
    QVERIFY(result.supported);
    QCOMPARE(result.latestVersion, QStringLiteral("0.3.0"));
    QCOMPARE(result.checkedAt, QStringLiteral("2026-08-25T12:00:00Z"));

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("GET /api/client/version"));
    QVERIFY(request.contains("X-Kypost-Device-Id: device-9"));
    QVERIFY(request.contains("X-Kypost-Device-Secret: secret-9"));
}

// A server on 0.3.0 has no such route. That is not an error condition: it is
// the entire population of servers in the field before this ships, and every
// one of them would otherwise show a permanent failure on the About screen.
void ClientVersionClientTest::treatsMissingEndpointAsUnsupportedRatherThanError()
{
    FakeRelayServer fake(httpResponse(404, "Not Found", R"({"error":"not found"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ClientVersionClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const ClientVersionResult result = client.fetch(
        serverBaseUrl, RelayAuth{ QStringLiteral("d"), QStringLiteral("s") });

    QVERIFY(!result.error.has_value());
    QVERIFY(!result.supported);
    QVERIFY(result.latestVersion.isEmpty());
}

// The server sends an empty latestVersion before its first check completes,
// while a release is still soaking, and when the repo has no releases.
void ClientVersionClientTest::treatsEmptyLatestVersionAsNoInformation()
{
    FakeRelayServer fake(httpResponse(200, "OK",
        R"({"latestVersion":"","checkedAt":"","error":""})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ClientVersionClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const ClientVersionResult result = client.fetch(
        serverBaseUrl, RelayAuth{ QStringLiteral("d"), QStringLiteral("s") });

    QVERIFY(!result.error.has_value());
    QVERIFY(result.supported);
    QVERIFY(result.latestVersion.isEmpty());
}

void ClientVersionClientTest::reportsTransportFailureAsError()
{
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ClientVersionClient client(http);

    // Port 1 with nothing listening: a connection refusal, not an HTTP status.
    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:1"));
    const ClientVersionResult result = client.fetch(
        serverBaseUrl, RelayAuth{ QStringLiteral("d"), QStringLiteral("s") });

    QVERIFY(result.error.has_value());
}

QTEST_MAIN(ClientVersionClientTest)
#include "ClientVersionClientTest.moc"
