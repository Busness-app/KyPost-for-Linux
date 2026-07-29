#include "net/MfaResponseClient.h"

#include "net/HttpClient.h"

#include "FakeRelayServer.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTest>

class MfaResponseClientTest : public QObject
{
    Q_OBJECT

private slots:
    void successParsesStatusAndBuildsEndpointFromServerBaseUrl();
    void conflictFrom409IsRejected();
    void unauthorizedFrom401IsReportedAsUnauthorized();
    void sentRequestCarriesDeviceHeadersAndSlimBody();
    void approveCarriesMatchDigits();
};

// Regression coverage for the Go-verified shape: internal/api/
// push_mfa_handlers.go's handlePushRespond authenticates via
// X-Kypost-Device-Id/X-Kypost-Device-Secret headers, with only
// {challengeId, approve, matchDigits} in the body -- no credentials ride in
// the JSON.

void MfaResponseClientTest::successParsesStatusAndBuildsEndpointFromServerBaseUrl()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"status":"approved"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    MfaResponseClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const MfaResponseResult result =
        client.respond(serverBaseUrl, QStringLiteral("chal-1"), QStringLiteral("device-1"), QStringLiteral("secret-1"), true);

    QCOMPARE(result.outcome, MfaResponseOutcome::Success);
    QVERIFY(result.status.has_value());
    QCOMPARE(*result.status, QStringLiteral("approved"));

    QVERIFY(fake.receivedRequest().contains("POST /api/mfa/push/respond HTTP/1.1"));
    // No query-param auth on this endpoint.
    QVERIFY(!fake.receivedRequest().contains("?"));
}

void MfaResponseClientTest::conflictFrom409IsRejected()
{
    FakeRelayServer fake(httpResponse(409, "Conflict", R"({"error":"already resolved","status":"approved"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    MfaResponseClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const MfaResponseResult result =
        client.respond(serverBaseUrl, QStringLiteral("chal-1"), QStringLiteral("device-1"), QStringLiteral("secret-1"), true);

    QCOMPARE(result.outcome, MfaResponseOutcome::Rejected);
    // status is optional on the 409 path per the brief, but surfaced here
    // since the server included it.
    QVERIFY(result.status.has_value());
    QCOMPARE(*result.status, QStringLiteral("approved"));
}

// 401/403 must NOT be folded into Rejected. Rejected means "the challenge
// was already resolved"; a 401 means "this device's credentials were
// refused", which with the credential PIN gate engaged is simply what a
// locked app gets -- load() hands out an empty deviceSecret by design. The
// user was told their approval had already been handled, when nothing had
// been sent and unlocking would have fixed it.
void MfaResponseClientTest::unauthorizedFrom401IsReportedAsUnauthorized()
{
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "Unauthorized\n"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    MfaResponseClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const MfaResponseResult result = client.respond(serverBaseUrl, QStringLiteral("chal-1"), QStringLiteral("device-1"),
                                                      QStringLiteral("secret-1"), false);

    QCOMPARE(result.outcome, MfaResponseOutcome::Unauthorized);
    // No status: the server sends none on this path, and inventing one
    // would let the caller print "already resolved (...)" again.
    QVERIFY(!result.status.has_value());
}

void MfaResponseClientTest::sentRequestCarriesDeviceHeadersAndSlimBody()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"status":"rejected"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    MfaResponseClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    client.respond(serverBaseUrl, QStringLiteral("chal-42"), QStringLiteral("device-42"), QStringLiteral("secret-42"), false);

    QVERIFY(fake.receivedRequest().contains("X-Kypost-Device-Id: device-42"));
    QVERIFY(fake.receivedRequest().contains("X-Kypost-Device-Secret: secret-42"));

    const QJsonObject sent = fake.receivedJsonBody();

    QCOMPARE(sent.value(QStringLiteral("challengeId")).toString(), QStringLiteral("chal-42"));
    QVERIFY(!sent.contains(QStringLiteral("subscriberId")));
    QVERIFY(!sent.contains(QStringLiteral("subscriberHash")));
    QVERIFY(!sent.contains(QStringLiteral("deviceId")));

    // The regression this task exists to prevent: the boolean key on the
    // wire must be "approve", not the stale Swift client's "approved".
    QVERIFY(sent.contains(QStringLiteral("approve")));
    QVERIFY(!sent.contains(QStringLiteral("approved")));
    QCOMPARE(sent.value(QStringLiteral("approve")).toBool(), false);

    // A deny carries the key but never a value -- the safe answer must not
    // depend on reading a number, and the server ignores it here.
    QCOMPARE(sent.value(QStringLiteral("matchDigits")).toString(), QString());

    // Exactly these three fields — no leftover credential fields.
    QCOMPARE(sent.size(), 3);
}

void MfaResponseClientTest::approveCarriesMatchDigits()
{
    // The server verifies this itself (mfa.Store::ResolvePushWithMatch) and
    // answers 400 to an approval that omits it, so it has to be on the wire.
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"status":"approved"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    MfaResponseClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    client.respond(serverBaseUrl, QStringLiteral("chal-42"), QStringLiteral("device-42"), QStringLiteral("secret-42"),
                    true, QStringLiteral("47"));

    const QJsonObject sent = fake.receivedJsonBody();
    QCOMPARE(sent.value(QStringLiteral("approve")).toBool(), true);
    QCOMPARE(sent.value(QStringLiteral("matchDigits")).toString(), QStringLiteral("47"));
}

QTEST_GUILESS_MAIN(MfaResponseClientTest)
#include "MfaResponseClientTest.moc"
