#include "net/PgpBootstrapClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTest>

class PgpBootstrapClientTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesIdentityAndProtection();
    void ignoresTheBrowserOnlyFields();
    void failureIsNotAnEmptySuccess();
};

void PgpBootstrapClientTest::parsesIdentityAndProtection()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"hasIdentity":true,"protection":"client"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpBootstrapClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const PgpBootstrapResult result = client.fetch(serverBaseUrl, auth);

    QCOMPARE(result.ok, true);
    QCOMPARE(result.hasIdentity, true);
    QCOMPARE(result.protection, QStringLiteral("client"));
}

// bootstrap carries wrappedPrivateKey, unlockRequired, signerPublicKeys and
// more that exist for the browser. Unknown/unused fields must not break
// parsing, and nothing here may start depending on them.
void PgpBootstrapClientTest::ignoresTheBrowserOnlyFields()
{
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"hasIdentity":false,"protection":"server","wrappedPrivateKey":"xxx","unlockRequired":true,)"
        R"("signerPublicKeys":[],"payloadEndpoint":"/x","somethingAddedLater":42})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpBootstrapClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const PgpBootstrapResult result = client.fetch(serverBaseUrl, auth);

    QCOMPARE(result.ok, true);
    QCOMPARE(result.hasIdentity, false);
    QCOMPARE(result.protection, QStringLiteral("server"));
}

// A failure must be distinguishable from a successful "no identity", or the
// compose screen cannot honor "couldn't check is not no".
void PgpBootstrapClientTest::failureIsNotAnEmptySuccess()
{
    FakeRelayServer fake(httpResponse(503, "Service Unavailable", "unavailable", "text/plain"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpBootstrapClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const PgpBootstrapResult result = client.fetch(serverBaseUrl, auth);

    QCOMPARE(result.ok, false);
    QVERIFY(result.error.has_value());
}

QTEST_GUILESS_MAIN(PgpBootstrapClientTest)
#include "PgpBootstrapClientTest.moc"
