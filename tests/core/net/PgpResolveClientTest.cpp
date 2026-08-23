#include "net/PgpResolveClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTest>

namespace {

PgpResolveResult resolveAgainst(FakeRelayServer& fake, const QStringList& addresses)
{
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpResolveClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    return client.resolve(serverBaseUrl, auth, addresses);
}

} // namespace

class PgpResolveClientTest : public QObject
{
    Q_OBJECT

private slots:
    void resolvesEachRecipientsKey();
    void asksTheRightEndpointWithPairingAuth();
    void anUnusableKeyIsNotQuietlyUsable();
    void anAbsentUsableFlagIsNotConsent();
    void aServerCustodyAccountIsToldSoRatherThanRetrying();
    void tooManyRecipientsIsItsOwnAnswer();
    void anUndecodableResponseIsNotAnEmptySuccess();
    void anOutageStaysRetryable();
};

void PgpResolveClientTest::resolvesEachRecipientsKey()
{
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"results":[)"
        R"({"address":"a@example.com","publicKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----\nx\n","fingerprint":"AAAA1111","tier":"contact","usable":true},)"
        R"({"address":"b@example.com","publicKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----\ny\n","fingerprint":"BBBB2222","tier":"wkd","usable":true}]})"));

    const PgpResolveResult result =
        resolveAgainst(fake, { QStringLiteral("a@example.com"), QStringLiteral("b@example.com") });

    QCOMPARE(result.status, PgpResolveStatus::Resolved);
    QCOMPARE(result.keys.size(), 2);
    QCOMPARE(result.keys.at(0).address, QStringLiteral("a@example.com"));
    QCOMPARE(result.keys.at(0).fingerprint, QStringLiteral("AAAA1111"));
    QCOMPARE(result.keys.at(0).tier, QStringLiteral("contact"));
    QVERIFY(result.keys.at(0).usable);
    QVERIFY(result.keys.at(0).publicKey.startsWith(QStringLiteral("-----BEGIN PGP PUBLIC KEY BLOCK-----")));
    QCOMPARE(result.keys.at(1).tier, QStringLiteral("wkd"));
}

void PgpResolveClientTest::asksTheRightEndpointWithPairingAuth()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"results":[]})"));
    QCOMPARE(resolveAgainst(fake, { QStringLiteral("a@example.com") }).status,
             PgpResolveStatus::Resolved);

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.startsWith("POST /api/pgp/recipients/resolve"));
    QVERIFY(request.contains("X-Kypost-Device-Id: device-1"));
    QVERIFY(request.contains("X-Kypost-Device-Secret: secret-1"));
    QCOMPARE(fake.receivedJsonBody().value(QStringLiteral("addresses")).toArray().size(), 1);
}

// A key can be present and unusable -- revoked, expired, wrong capability --
// and the relay folds all of that into one flag. Carrying the key while
// dropping the flag would let a caller encrypt to a revoked key.
void PgpResolveClientTest::anUnusableKeyIsNotQuietlyUsable()
{
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"results":[{"address":"a@example.com","publicKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----\nx\n","fingerprint":"AAAA","tier":"keyserver","usable":false}]})"));

    const PgpResolveResult result = resolveAgainst(fake, { QStringLiteral("a@example.com") });

    QCOMPARE(result.status, PgpResolveStatus::Resolved);
    QCOMPARE(result.keys.size(), 1);
    QVERIFY2(!result.keys.at(0).usable, "a revoked or expired key came back usable");
}

// `usable` is omitempty on the wire, so a missing field is the common shape
// for "no". Reading absence as consent is how a caller ends up encrypting to
// something the relay never vouched for.
void PgpResolveClientTest::anAbsentUsableFlagIsNotConsent()
{
    FakeRelayServer fake(
        httpResponse(200, "OK", R"({"results":[{"address":"a@example.com","tier":"none"}]})"));

    const PgpResolveResult result = resolveAgainst(fake, { QStringLiteral("a@example.com") });

    QCOMPARE(result.keys.size(), 1);
    QVERIFY2(!result.keys.at(0).usable, "an absent usable flag was read as yes");
    QVERIFY(result.keys.at(0).publicKey.isEmpty());
}

// 409 here means the opposite of the 409 on /api/mail/pgp-payload: this
// account is NOT client-protected, so the server encrypts on its own and there
// is nothing for this path to do.
void PgpResolveClientTest::aServerCustodyAccountIsToldSoRatherThanRetrying()
{
    FakeRelayServer fake(httpResponse(409, "Conflict",
                                       R"({"error":"this account's PGP key is not client-protected"})"));

    QCOMPARE(resolveAgainst(fake, { QStringLiteral("a@example.com") }).status,
             PgpResolveStatus::ServerEncryptsInstead);
}

void PgpResolveClientTest::tooManyRecipientsIsItsOwnAnswer()
{
    FakeRelayServer fake(
        httpResponse(413, "Request Entity Too Large", R"({"error":"too many addresses"})"));

    QCOMPARE(resolveAgainst(fake, { QStringLiteral("a@example.com") }).status,
             PgpResolveStatus::TooManyRecipients);
}

void PgpResolveClientTest::anUndecodableResponseIsNotAnEmptySuccess()
{
    FakeRelayServer fake(httpResponse(200, "OK", "not json"));

    const PgpResolveResult result = resolveAgainst(fake, { QStringLiteral("a@example.com") });

    QCOMPARE(result.status, PgpResolveStatus::Failed);
    QCOMPARE(result.error, std::optional<NetworkError>(NetworkError::Decoding));
    QVERIFY(result.keys.isEmpty());
}

void PgpResolveClientTest::anOutageStaysRetryable()
{
    FakeRelayServer fake(httpResponse(503, "Service Unavailable", "down", "text/plain"));

    const PgpResolveResult result = resolveAgainst(fake, { QStringLiteral("a@example.com") });

    QCOMPARE(result.status, PgpResolveStatus::Failed);
    QVERIFY(result.error.has_value());
    QVERIFY(!result.detail.isEmpty());
}

QTEST_GUILESS_MAIN(PgpResolveClientTest)
#include "PgpResolveClientTest.moc"
