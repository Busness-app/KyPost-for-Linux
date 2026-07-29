#include "net/NativeRegistrationClient.h"

#include "net/HttpClient.h"

#include "FakeRelayServer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTest>

class NativeRegistrationClientTest : public QObject
{
    Q_OBJECT

private slots:
    void successParsesResponseAndSendsExpectedBody();
    void successOmitsOptionalFieldsWhenNotProvided();
    void successDerivesPullEndpointWhenServerOmitsIt();
    void unauthorizedFrom401();
    void backendMisconfiguredFrom503();
    void noQueryParamsOnRequestUrl();
    void unusableCredentialInA200IsRefused_data();
    void unusableCredentialInA200IsRefused();
};

void NativeRegistrationClientTest::successParsesResponseAndSendsExpectedBody()
{
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-1","deviceSecret":"top-secret-device-secret",)"
                             R"("devices":2,"deliveryMode":"pull",)"
                             R"("pullEndpoint":"http://relay.example/api/notifications/native/pull",)"
                             R"("transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port()));
    const NativeRegistrationResult result =
        client.registerDevice(endpoint, QStringLiteral("sub-1"), QStringLiteral("pair-tok"),
                               QStringLiteral("https://push.example/endpoint"), QStringLiteral("device-1"),
                               QStringLiteral("My Desktop"));

    QCOMPARE(result.outcome, RegistrationOutcome::Success);
    QVERIFY(result.response.ok);
    QVERIFY(result.response.synced);
    QCOMPARE(result.response.deviceId, QStringLiteral("dev-1"));
    QCOMPARE(result.response.deviceSecret, QStringLiteral("top-secret-device-secret"));
    QCOMPARE(result.response.devices, 2);
    QCOMPARE(result.response.deliveryMode, QStringLiteral("pull"));
    QCOMPARE(result.response.pullEndpoint, QStringLiteral("http://relay.example/api/notifications/native/pull"));
    QCOMPARE(result.response.transport, QStringLiteral("unifiedpush"));

    const QJsonObject sent = fake.receivedJsonBody();
    QCOMPARE(sent.value(QStringLiteral("subscriberId")).toString(), QStringLiteral("sub-1"));
    QVERIFY(!sent.contains(QStringLiteral("subscriberHash")));
    QCOMPARE(sent.value(QStringLiteral("pairingToken")).toString(), QStringLiteral("pair-tok"));
    QCOMPARE(sent.value(QStringLiteral("deviceToken")).toString(), QStringLiteral("https://push.example/endpoint"));
    QCOMPARE(sent.value(QStringLiteral("deviceId")).toString(), QStringLiteral("device-1"));
    QCOMPARE(sent.value(QStringLiteral("deviceName")).toString(), QStringLiteral("My Desktop"));
    QCOMPARE(sent.value(QStringLiteral("platform")).toString(), QStringLiteral("linux"));
    QCOMPARE(sent.value(QStringLiteral("transport")).toString(), QStringLiteral("unifiedpush"));

    QVERIFY(fake.receivedRequest().contains("Content-Type: application/json"));
}

void NativeRegistrationClientTest::successOmitsOptionalFieldsWhenNotProvided()
{
    const QByteArray body = R"({"ok":true,"synced":false,"deviceId":"dev-2","deviceSecret":"sec-2","devices":1,)"
                             R"("deliveryMode":"push","pullEndpoint":"","transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port()));
    const NativeRegistrationResult result = client.registerDevice(
        endpoint, QStringLiteral("sub-1"), QStringLiteral("pair-tok"), QStringLiteral("https://push.example/endpoint"),
        QString(), QString());

    QCOMPARE(result.outcome, RegistrationOutcome::Success);

    const QJsonObject sent = fake.receivedJsonBody();
    QVERIFY(!sent.contains(QStringLiteral("subscriberHash")));
    QVERIFY(!sent.contains(QStringLiteral("deviceId")));
    QVERIFY(!sent.contains(QStringLiteral("deviceName")));
    QVERIFY(!sent.contains(QStringLiteral("appVersion")));
}

void NativeRegistrationClientTest::successDerivesPullEndpointWhenServerOmitsIt()
{
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-3","deviceSecret":"sec-3","devices":1,)"
                             R"("deliveryMode":"pull","pullEndpoint":"","transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port()));
    const NativeRegistrationResult result = client.registerDevice(
        endpoint, QStringLiteral("sub-1"), QStringLiteral("pair-tok"), QStringLiteral("https://push.example/endpoint"),
        QString(), QString());

    QCOMPARE(result.outcome, RegistrationOutcome::Success);
    QCOMPARE(result.response.pullEndpoint,
             QStringLiteral("http://127.0.0.1:%1/api/notifications/native/pull").arg(fake.port()));
}

// A 200 carrying any JSON object used to count as Success: `ok` was parsed
// and never read, and an empty deviceId/deviceSecret was stored as-is. That
// let a deep link whose `reg` named an unrelated same-origin endpoint -- the
// relay's unauthenticated /api/health answers POST with a JSON object --
// overwrite the working device credential with empty strings while the UI
// still reported "Paired".
void NativeRegistrationClientTest::unusableCredentialInA200IsRefused_data()
{
    QTest::addColumn<QByteArray>("body");

    QTest::newRow("health-endpoint-shaped")
        << QByteArray(R"({"status":"ok","uptime":42})");
    QTest::newRow("ok-false")
        << QByteArray(R"({"ok":false,"deviceId":"d","deviceSecret":"s"})");
    QTest::newRow("empty-deviceId")
        << QByteArray(R"({"ok":true,"deviceId":"","deviceSecret":"s"})");
    QTest::newRow("empty-deviceSecret")
        << QByteArray(R"({"ok":true,"deviceId":"d","deviceSecret":""})");
}

void NativeRegistrationClientTest::unusableCredentialInA200IsRefused()
{
    QFETCH(QByteArray, body);

    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port()));
    const NativeRegistrationResult result = client.registerDevice(
        endpoint, QStringLiteral("sub-1"), QStringLiteral("pair-tok"), QStringLiteral("https://push.example/endpoint"),
        QString(), QString());

    QCOMPARE(result.outcome, RegistrationOutcome::Failure);
    QVERIFY(!result.detail.isEmpty());
    // Nothing usable escapes to the caller, so PairingStore::save() can never
    // be handed an empty credential to persist over a working one.
    QVERIFY(result.response.deviceSecret.isEmpty());
}

void NativeRegistrationClientTest::unauthorizedFrom401()
{
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "{}"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port()));
    const NativeRegistrationResult result = client.registerDevice(
        endpoint, QStringLiteral("sub-1"), QStringLiteral("pair-tok"), QStringLiteral("https://push.example/endpoint"),
        QString(), QString());

    QCOMPARE(result.outcome, RegistrationOutcome::Unauthorized);
}

void NativeRegistrationClientTest::backendMisconfiguredFrom503()
{
    FakeRelayServer fake(httpResponse(503, "Service Unavailable", "{}"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port()));
    const NativeRegistrationResult result = client.registerDevice(
        endpoint, QStringLiteral("sub-1"), QStringLiteral("pair-tok"), QStringLiteral("https://push.example/endpoint"),
        QString(), QString());

    QCOMPARE(result.outcome, RegistrationOutcome::BackendMisconfigured);
}

void NativeRegistrationClientTest::noQueryParamsOnRequestUrl()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port()));
    client.registerDevice(endpoint, QStringLiteral("sub-1"), QStringLiteral("pair-tok"),
                           QStringLiteral("https://push.example/endpoint"), QString(), QString());

    // Unlike every other Relay endpoint in this batch, native/register takes
    // no query-param auth — sub must not be appended to the URL.
    QVERIFY(fake.receivedRequest().contains("POST /api/notifications/native/register HTTP/1.1"));
    QVERIFY(!fake.receivedRequest().contains("?"));
}

QTEST_GUILESS_MAIN(NativeRegistrationClientTest)
#include "NativeRegistrationClientTest.moc"
