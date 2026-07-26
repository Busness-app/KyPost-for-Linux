#include "net/PgpRecipientChecker.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QTest>

class PgpRecipientCheckerTest : public QObject
{
    Q_OBJECT

private slots:
    void reportsOnlyTheAddressesWithNoUsableKey();
    void sendsTheAddressesInTheRequestBody();
    void revokedKeyIsAlreadyKeylessWithoutReDerivation();
    void revokedFlagAloneNeverMakesAKeyedRecipientKeyless();
    void failureIsNotAnEmptyKeylessList();
};

void PgpRecipientCheckerTest::reportsOnlyTheAddressesWithNoUsableKey()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"results":[
        {"address":"alice@example.com","hasKey":true,"revoked":false,"expired":false,"tier":"contact-verified"},
        {"address":"bob@example.com","hasKey":false,"revoked":false,"expired":false,"tier":"none"}
    ]})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const RecipientKeyCheckResult result = checker.check(
        serverBaseUrl, auth,
        QStringList{ QStringLiteral("alice@example.com"), QStringLiteral("bob@example.com") });

    QCOMPARE(result.ok, true);
    QCOMPARE(result.keylessRecipients, QStringList{ QStringLiteral("bob@example.com") });
}

void PgpRecipientCheckerTest::sendsTheAddressesInTheRequestBody()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"results":[]})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    checker.check(serverBaseUrl, auth,
                  QStringList{ QStringLiteral("alice@example.com"), QStringLiteral("bob@example.com") });

    const QJsonArray sent = fake.receivedJsonBody().value(QStringLiteral("addresses")).toArray();
    QCOMPARE(sent.size(), 2);
    QCOMPARE(sent.at(0).toString(), QStringLiteral("alice@example.com"));
    QCOMPARE(sent.at(1).toString(), QStringLiteral("bob@example.com"));
}

// hasKey is already false for a revoked or expired key -- the handler sets it
// from ks.Usable() (pgp_keyserver.go:143). A revoked contact is therefore
// keyless without this client deriving anything from revoked/expired.
void PgpRecipientCheckerTest::revokedKeyIsAlreadyKeylessWithoutReDerivation()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"results":[
        {"address":"dave@example.com","hasKey":false,"revoked":true,"expired":false,"tier":"none"}
    ]})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const RecipientKeyCheckResult result =
        checker.check(serverBaseUrl, auth, QStringList{ QStringLiteral("dave@example.com") });

    QCOMPARE(result.keylessRecipients, QStringList{ QStringLiteral("dave@example.com") });
}

// The falsifying inverse of the test above, and the one that actually pins
// "never re-derive keylessness from revoked/expired": hasKey is the ONLY input.
// A response saying hasKey: true, revoked: true is self-contradictory by the
// server's own rule (hasKey comes from ks.Usable()), and this client must
// still take hasKey at its word rather than second-guessing it from the
// advisory flags -- a client that OR-ed revoked/expired in would warn about
// recipients the send path will happily encrypt to, and the warning's whole
// contract is that it is a lower bound, never a prediction.
void PgpRecipientCheckerTest::revokedFlagAloneNeverMakesAKeyedRecipientKeyless()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"results":[
        {"address":"erin@example.com","hasKey":true,"revoked":true,"expired":true,"tier":"contact-verified"}
    ]})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const RecipientKeyCheckResult result =
        checker.check(serverBaseUrl, auth, QStringList{ QStringLiteral("erin@example.com") });

    QCOMPARE(result.ok, true);
    QVERIFY(result.keylessRecipients.isEmpty());
}

// A failed preflight must not read as "everyone has a key". It is only an
// inline warning, so ok=false means "show nothing", never "all clear".
void PgpRecipientCheckerTest::failureIsNotAnEmptyKeylessList()
{
    FakeRelayServer fake(httpResponse(500, "Internal Server Error", "boom", "text/plain"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const RecipientKeyCheckResult result =
        checker.check(serverBaseUrl, auth, QStringList{ QStringLiteral("alice@example.com") });

    QCOMPARE(result.ok, false);
    QVERIFY(result.keylessRecipients.isEmpty());
}

QTEST_GUILESS_MAIN(PgpRecipientCheckerTest)
#include "PgpRecipientCheckerTest.moc"
