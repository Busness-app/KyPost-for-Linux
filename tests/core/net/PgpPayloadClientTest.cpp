#include "net/PgpPayloadClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTest>

namespace {

const QString kMailbox = QStringLiteral("INBOX");
const QString kMessageId = QStringLiteral("5");

// Real armor, not a placeholder: the whole point of this client is that what
// comes back is handed to a decryptor verbatim, so the fixture is shaped like
// the thing -- multi-line, which is where a careless parser loses bytes.
const QByteArray kArmored = "-----BEGIN PGP MESSAGE-----\n\nhQEMA0abc123\n=xYz1\n-----END PGP MESSAGE-----";
const QByteArray kArmoredJson =
    R"("-----BEGIN PGP MESSAGE-----\n\nhQEMA0abc123\n=xYz1\n-----END PGP MESSAGE-----")";

PgpPayloadResult fetchAgainst(FakeRelayServer& fake)
{
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpPayloadClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    return client.fetch(serverBaseUrl, auth, kMailbox, kMessageId);
}

} // namespace

class PgpPayloadClientTest : public QObject
{
    Q_OBJECT

private slots:
    void handsBackTheArmoredCiphertextVerbatim();
    void asksTheRightEndpointWithPairingAuth();
    void ignoresTheFieldsNoVerifierCanUseYet();
    void serverCustodyIsItsOwnAnswer();
    void aMessageWithNoOpenPgpPayloadIsTerminal();
    void signedButNotEncryptedCarriesNoCiphertext();
    void whitespaceOnlyArmorIsNotCiphertext();
    void theServerRefusingToHoldTheMessageIsTooLarge();
    void thisClientRefusingToAllocateIsAlsoTooLarge();
    void anUndecodableResponseIsNotAnEmptySuccess();
    void transportFailureStaysRetryable();
};

void PgpPayloadClientTest::handsBackTheArmoredCiphertextVerbatim()
{
    FakeRelayServer fake(httpResponse(
        200, "OK", R"({"messageId":5,"mailbox":"INBOX","encryptedPayload":)" + kArmoredJson + "}"));

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::Fetched);
    QCOMPARE(result.encryptedPayload.toUtf8(), kArmored);
    QVERIFY(!result.error.has_value());
}

// A wrong query-parameter name does not fail loudly -- the endpoint answers
// 400 and every message reads as broken. Pin the shape of the request.
void PgpPayloadClientTest::asksTheRightEndpointWithPairingAuth()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"encryptedPayload":"-----BEGIN PGP MESSAGE-----\nx\n-----END PGP MESSAGE-----"})"));

    QCOMPARE(fetchAgainst(fake).status, PgpPayloadStatus::Fetched);

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.startsWith("GET /api/mail/pgp-payload?"));
    QVERIFY(request.contains("mailbox=INBOX"));
    QVERIFY(request.contains("messageId=5"));
    // withMailAuth on the server side: per-device headers, no session cookie.
    QVERIFY(request.contains("X-Kypost-Device-Id: device-1"));
    QVERIFY(request.contains("X-Kypost-Device-Secret: secret-1"));
}

// signerKeys/signaturePayload/sender/resolvedSender are real fields this
// client deliberately does not parse until there is a verifier to bind them
// (see the header). Their presence must not disturb it, and nothing here may
// quietly start depending on them.
void PgpPayloadClientTest::ignoresTheFieldsNoVerifierCanUseYet()
{
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"encryptedPayload":"-----BEGIN PGP MESSAGE-----\nx\n-----END PGP MESSAGE-----",)"
        R"("signaturePayload":"-----BEGIN PGP SIGNATURE-----","signedPartBase64":"aGk=",)"
        R"("signerKeys":[{"publicKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----","addresses":["eve@evil.example"]}],)"
        R"("sender":"bob@example.com <eve@evil.example>","resolvedSender":"eve@evil.example",)"
        R"("body":"","somethingAddedLater":42})"));

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::Fetched);
    QVERIFY(result.encryptedPayload.startsWith(QStringLiteral("-----BEGIN PGP MESSAGE-----")));
}

// 409 means the account never migrated to client custody. The server refuses
// ciphertext AND no longer decrypts for it, so this is not "try again" and
// not "no payload" -- it is the one outcome with an action attached.
void PgpPayloadClientTest::serverCustodyIsItsOwnAnswer()
{
    FakeRelayServer fake(httpResponse(409, "Conflict",
                                       R"({"error":"migrate your key","migrationNeeded":true})"));

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::ServerCustody);
    QVERIFY(result.encryptedPayload.isEmpty());
}

void PgpPayloadClientTest::aMessageWithNoOpenPgpPayloadIsTerminal()
{
    FakeRelayServer fake(
        httpResponse(404, "Not Found", R"({"error":"message carries no OpenPGP payload"})"));

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::NoCiphertext);
    QVERIFY(result.encryptedPayload.isEmpty());
}

// A signed-but-not-encrypted message answers 200 with a blank
// encryptedPayload. Reporting that as Fetched would hand a decryptor an
// empty string and turn a perfectly readable message into "malformed".
void PgpPayloadClientTest::signedButNotEncryptedCarriesNoCiphertext()
{
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"encryptedPayload":"","signaturePayload":"-----BEGIN PGP SIGNATURE-----","signedPartBase64":"aGk="})"));

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::NoCiphertext);
    QVERIFY(result.encryptedPayload.isEmpty());
}

// Armor is whitespace-delimited, so a payload of nothing but newlines is not
// one -- and it must not reach the decryptor as if it were.
void PgpPayloadClientTest::whitespaceOnlyArmorIsNotCiphertext()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"encryptedPayload":"  \n\t \n "})"));

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::NoCiphertext);
    QVERIFY(result.encryptedPayload.isEmpty());
}

void PgpPayloadClientTest::theServerRefusingToHoldTheMessageIsTooLarge()
{
    FakeRelayServer fake(httpResponse(413, "Request Entity Too Large",
                                       R"({"error":"message exceeds the maximum size"})"));

    QCOMPARE(fetchAgainst(fake).status, PgpPayloadStatus::TooLarge);
}

// The other half of the same user-facing fact: the relay is inside the threat
// model, so a response claiming to be larger than this client will allocate
// is refused here rather than trusted. Driven by an inflated Content-Length
// so the test does not have to produce 25 MB.
void PgpPayloadClientTest::thisClientRefusingToAllocateIsAlsoTooLarge()
{
    QByteArray response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: application/json\r\n";
    response += "Content-Length: " + QByteArray::number(HttpClient::kMaxAttachmentResponseBytes + 1) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += "{";
    FakeRelayServer fake(response);

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::TooLarge);
    QCOMPARE(result.error, std::optional<NetworkError>(NetworkError::ResponseTooLarge));
    QVERIFY(result.encryptedPayload.isEmpty());
}

void PgpPayloadClientTest::anUndecodableResponseIsNotAnEmptySuccess()
{
    FakeRelayServer fake(httpResponse(200, "OK", "not json at all"));

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::Failed);
    QCOMPARE(result.error, std::optional<NetworkError>(NetworkError::Decoding));
}

// Failed is the retryable bucket; the three terminal statuses above must not
// absorb an ordinary outage, or the UI stops offering Retry when retrying is
// exactly what would work.
void PgpPayloadClientTest::transportFailureStaysRetryable()
{
    FakeRelayServer fake(httpResponse(503, "Service Unavailable", "unavailable", "text/plain"));

    const PgpPayloadResult result = fetchAgainst(fake);

    QCOMPARE(result.status, PgpPayloadStatus::Failed);
    QVERIFY(result.error.has_value());
    QVERIFY(!result.detail.isEmpty());
}

QTEST_GUILESS_MAIN(PgpPayloadClientTest)
#include "PgpPayloadClientTest.moc"
