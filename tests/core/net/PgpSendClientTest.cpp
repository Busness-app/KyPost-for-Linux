#include "net/PgpSendClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTest>

namespace {

PgpSendPlan builtPlan(bool withSentCopy = true)
{
    PgpSendPlan plan;
    plan.status = PgpSendPlanStatus::Built;

    PgpDelivery visible;
    visible.smtpRecipients = { QStringLiteral("alice@example.com"), QStringLiteral("bob@example.com") };
    visible.message = "From: me@example.com\r\nTo: alice@example.com\r\n\r\nVISIBLE-CIPHERTEXT";
    plan.deliveries.append(visible);

    PgpDelivery blind;
    blind.smtpRecipients = { QStringLiteral("carol@example.com") };
    blind.message = "From: me@example.com\r\nTo: alice@example.com\r\n\r\nBLIND-CIPHERTEXT";
    plan.deliveries.append(blind);

    if (withSentCopy)
        plan.sentCopy = "From: me@example.com\r\n\r\nSENT-COPY-CIPHERTEXT";
    else
        plan.sentCopyUnavailable = true;
    return plan;
}

PgpSendResult sendAgainst(FakeRelayServer& fake, const PgpSendPlan& plan)
{
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpSendClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    return client.send(serverBaseUrl, auth, QStringLiteral("me@example.com"), plan,
                        { QStringLiteral("alice@example.com") }, { QStringLiteral("bob@example.com") },
                        { QStringLiteral("carol@example.com") }, QStringLiteral("plain"));
}

} // namespace

class PgpSendClientTest : public QObject
{
    Q_OBJECT

private slots:
    void everyDeliveryReachesTheRelayWithItsOwnRecipients();
    void theRealSubjectIsNeverSent();
    void anEncryptedSentCopyIsClaimedAsSuch();
    void noSentCopyIsNeverClaimedAsEncrypted();
    void aPartialBccFailureIsSurfacedWithoutPrintingRelayProse();
    void aSentCopyTheRelayDidNotSaveIsReported();
    void aPlanThatDidNotBuildIsNeverSent();
    void aPlainTextErrorBodyBecomesTheDetail();
    void anUndecodableResponseIsNotAnEmptySuccess();
};

void PgpSendClientTest::everyDeliveryReachesTheRelayWithItsOwnRecipients()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true,"warning":""})"));

    const PgpSendResult result = sendAgainst(fake, builtPlan());

    QVERIFY(result.ok);
    QVERIFY(fake.receivedRequest().startsWith("POST /api/mail/send-pgp"));

    const QJsonObject body = fake.receivedJsonBody();
    const QJsonArray deliveries = body.value(QStringLiteral("deliveries")).toArray();
    QCOMPARE(deliveries.size(), 2);

    // The Bcc delivery keeps its own single recipient -- the split the planner
    // made must survive serialisation, or every blind recipient is named to
    // every other.
    const QJsonObject blind = deliveries.at(1).toObject();
    QCOMPARE(blind.value(QStringLiteral("recipients")).toArray().size(), 1);
    QCOMPARE(blind.value(QStringLiteral("recipients")).toArray().at(0).toString(),
             QStringLiteral("carol@example.com"));
    QVERIFY(blind.value(QStringLiteral("ciphertext")).toString().contains(QStringLiteral("BLIND-CIPHERTEXT")));

    const QJsonObject visible = deliveries.at(0).toObject();
    QCOMPARE(visible.value(QStringLiteral("recipients")).toArray().size(), 2);
}

// The relay's own struct comment says this field is accepted and ignored and
// that no client should start reading it. Sending the real one would put the
// subject of every encrypted message on a path the relay can read, which is
// the entire thing this mode prevents.
void PgpSendClientTest::theRealSubjectIsNeverSent()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true})"));
    sendAgainst(fake, builtPlan());

    QCOMPARE(fake.receivedJsonBody().value(QStringLiteral("subject")).toString(),
             QStringLiteral("[Encrypted] Email Sent by KyPost"));
}

void PgpSendClientTest::anEncryptedSentCopyIsClaimedAsSuch()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true})"));
    const PgpSendResult result = sendAgainst(fake, builtPlan(/*withSentCopy=*/true));

    const QJsonObject body = fake.receivedJsonBody();
    QVERIFY(body.value(QStringLiteral("sentCopy")).toString().contains(QStringLiteral("SENT-COPY")));
    QVERIFY2(body.value(QStringLiteral("sentCopyEncrypted")).toBool(),
             "the relay stores a copy only when this is asserted");
    QVERIFY(result.sentSaved);
}

// The relay files a copy ONLY when sentCopyEncrypted is set. Claiming it for
// something that is not ciphertext would be asking the relay to file the
// plaintext of a message it cannot read -- on the account's IMAP host, which
// holds no key at all.
void PgpSendClientTest::noSentCopyIsNeverClaimedAsEncrypted()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true})"));
    const PgpSendResult result = sendAgainst(fake, builtPlan(/*withSentCopy=*/false));

    const QJsonObject body = fake.receivedJsonBody();
    QVERIFY2(!body.contains(QStringLiteral("sentCopy")), "an absent copy was sent anyway");
    QVERIFY2(!body.value(QStringLiteral("sentCopyEncrypted")).toBool(),
             "the encrypted claim was made with nothing to back it");
    // And the relay's cheerful sentSaved:true does not override what this
    // client knows: there was no copy to save.
    QVERIFY2(!result.sentSaved, "a copy that was never sent was reported as saved");
}

// A 200 does not mean every recipient received it: Bcc deliveries go one at a
// time and a failure among them is a warning on an otherwise successful
// response.
void PgpSendClientTest::aPartialBccFailureIsSurfacedWithoutPrintingRelayProse()
{
    FakeRelayServer fake(httpResponse(
        200, "OK", R"({"ok":true,"sentSaved":true,"warning":"2 bcc delivery(s) failed"})"));

    const PgpSendResult result = sendAgainst(fake, builtPlan());

    QVERIFY(result.ok);
    QVERIFY2(result.warned, "a partial delivery failure was swallowed");
    // The sentence is kept for a log line and is deliberately not the thing a
    // caller is handed to render.
    QCOMPARE(result.warningDetail, QStringLiteral("2 bcc delivery(s) failed"));
}

void PgpSendClientTest::aSentCopyTheRelayDidNotSaveIsReported()
{
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"ok":true,"sentSaved":false,"warning":"email sent but could not be saved to Sent folder"})"));

    const PgpSendResult result = sendAgainst(fake, builtPlan());

    QVERIFY2(result.ok, "the message did go out");
    QVERIFY2(!result.sentSaved, "the user's outbox will not have it and they were not told");
    QVERIFY(result.warned);
}

// A plan that did not build is one whose recipients could not all be
// encrypted to. Sending it and letting the relay refuse would put half a
// send's worth of ciphertext on the wire for nothing.
void PgpSendClientTest::aPlanThatDidNotBuildIsNeverSent()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));

    PgpSendPlan refused;
    refused.status = PgpSendPlanStatus::RecipientWithoutKey;

    const PgpSendResult result = sendAgainst(fake, refused);

    QVERIFY(!result.ok);
    QVERIFY2(fake.receivedRequest().isEmpty(), "a request was made for a send that cannot happen");
}

// Failures on this endpoint arrive as plain text rather than JSON, so the body
// is the only detail there is.
void PgpSendClientTest::aPlainTextErrorBodyBecomesTheDetail()
{
    FakeRelayServer fake(httpResponse(403, "Forbidden",
                                       "delivery 0: not authorized to send as that address",
                                       "text/plain"));

    const PgpSendResult result = sendAgainst(fake, builtPlan());

    QVERIFY(!result.ok);
    QVERIFY(result.error.has_value());
    QVERIFY2(result.detail.contains(QStringLiteral("not authorized")),
             "the relay's reason was thrown away");
}

void PgpSendClientTest::anUndecodableResponseIsNotAnEmptySuccess()
{
    FakeRelayServer fake(httpResponse(200, "OK", "not json"));

    const PgpSendResult result = sendAgainst(fake, builtPlan());

    QVERIFY2(!result.ok, "an unparseable response was read as a successful send");
    QCOMPARE(result.error, std::optional<NetworkError>(NetworkError::Decoding));
}

QTEST_GUILESS_MAIN(PgpSendClientTest)
#include "PgpSendClientTest.moc"
