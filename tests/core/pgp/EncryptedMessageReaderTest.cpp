#include "pgp/EncryptedMessageReader.h"

#include "net/HttpClient.h"
#include "net/PgpPayloadClient.h"
#include "net/RelayAuth.h"

#include "GnupgFixture.h"
#include "../net/FakeRelayServer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>

namespace {

// The relay's response with real armor in it. Built through QJsonDocument
// rather than string-pasted, because armor is multi-line and hand-escaping
// it is how a fixture ends up testing the escaping instead of the reader.
QByteArray payloadResponse(const QByteArray& armored)
{
    const QJsonObject body{
        { QStringLiteral("messageId"), 5 },
        { QStringLiteral("mailbox"), QStringLiteral("INBOX") },
        { QStringLiteral("encryptedPayload"), QString::fromUtf8(armored) },
        { QStringLiteral("body"), QString() },
    };
    return httpResponse(200, "OK", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

} // namespace

class EncryptedMessageReaderTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void anEncryptedMessageComesBackAsPlaintext();
    void oneReadIsOneFetch();
    void mailForAKeyThisMachineLacksIsNotAFailedFetch();
    void garbageArmorIsMalformedRatherThanBlank();
    void aServerCustodyAccountKeepsItsOwnAnswer();
    void nothingToDecryptIsTerminalNotRetryable();
    void theRelayRefusingTheMessageIsTooLarge();
    void aPlaintextOverTheCeilingIsTheSameAnswer();
    void onlyTheRetryableStatusCarriesDetail();

private:
    PgpReadResult readAgainst(FakeRelayServer& fake, const OpenPgpDecryptor& decryptor,
                               const QString& gnupgHome) const;

    GnupgFixture m_fixture;
};

void EncryptedMessageReaderTest::initTestCase()
{
    if (!OpenPgpDecryptor::engineAvailable())
        QSKIP("no usable gpg on this system -- OpenPGP reading is NOT covered");
    if (!m_fixture.build())
        QSKIP("could not build a throwaway GnuPG keyring -- OpenPGP reading is NOT covered");
}

void EncryptedMessageReaderTest::cleanupTestCase()
{
    GnupgFixture::killAgent(m_fixture.path());
}

PgpReadResult EncryptedMessageReaderTest::readAgainst(FakeRelayServer& fake,
                                                       const OpenPgpDecryptor& decryptor,
                                                       const QString& gnupgHome) const
{
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpPayloadClient payloads(http);
    EncryptedMessageReader reader(payloads, decryptor);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    return reader.read(serverBaseUrl, auth, QStringLiteral("INBOX"), QStringLiteral("5"), gnupgHome);
}

// End to end through the real artifacts: a real gpg keyring, real armor, a
// real socket. Nothing here is a stand-in for the thing it is testing.
void EncryptedMessageReaderTest::anEncryptedMessageComesBackAsPlaintext()
{
    const QByteArray secret = "meet me where the relay cannot read it\n";
    const QByteArray armored = m_fixture.encryptToTestKey(secret);
    QVERIFY(!armored.isEmpty());
    QVERIFY2(!armored.contains("meet me where"), "the fixture did not actually encrypt anything");

    FakeRelayServer fake(payloadResponse(armored));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QCOMPARE(result.plaintext, secret);
}

void EncryptedMessageReaderTest::oneReadIsOneFetch()
{
    const QByteArray armored = m_fixture.encryptToTestKey("once\n");
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(payloadResponse(armored));
    QCOMPARE(readAgainst(fake, OpenPgpDecryptor(), m_fixture.path()).status, PgpReadStatus::Decrypted);
    QCOMPARE(fake.connectionCount(), 1);
}

// The common real case: mail addressed to a key the user keeps on another
// machine. It must survive the trip as NoSecretKey -- "you cannot read this
// here" is actionable, and both "corrupt message" and "network problem" are
// lies that send the user somewhere useless.
void EncryptedMessageReaderTest::mailForAKeyThisMachineLacksIsNotAFailedFetch()
{
    const QByteArray armored = m_fixture.encryptToTestKey("not for this machine\n");
    QVERIFY(!armored.isEmpty());

    QTemporaryDir strangerHome;
    QVERIFY(strangerHome.isValid());

    FakeRelayServer fake(payloadResponse(armored));
    const PgpReadResult result =
        readAgainst(fake, OpenPgpDecryptor(), GnupgFixture::emptyHome(strangerHome));

    QCOMPARE(result.status, PgpReadStatus::NoSecretKey);
    QVERIFY(result.plaintext.isEmpty());

    GnupgFixture::killAgent(strangerHome.path());
}

void EncryptedMessageReaderTest::garbageArmorIsMalformedRatherThanBlank()
{
    FakeRelayServer fake(payloadResponse("-----BEGIN PGP MESSAGE-----\nnot base64 at all\n"));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Malformed);
    QVERIFY2(result.plaintext.isEmpty(), "something was handed up for a message that never decrypted");
}

// 409 is the one outcome with an instruction attached -- finish the key
// migration. Flattening it into FetchFailed would offer Retry for a
// condition that no amount of retrying resolves.
void EncryptedMessageReaderTest::aServerCustodyAccountKeepsItsOwnAnswer()
{
    FakeRelayServer fake(httpResponse(409, "Conflict",
                                       R"({"error":"migrate your key","migrationNeeded":true})"));
    QCOMPARE(readAgainst(fake, OpenPgpDecryptor(), m_fixture.path()).status,
             PgpReadStatus::ServerCustody);
}

void EncryptedMessageReaderTest::nothingToDecryptIsTerminalNotRetryable()
{
    FakeRelayServer fake(
        httpResponse(404, "Not Found", R"({"error":"message carries no OpenPGP payload"})"));
    QCOMPARE(readAgainst(fake, OpenPgpDecryptor(), m_fixture.path()).status,
             PgpReadStatus::NoCiphertext);
}

void EncryptedMessageReaderTest::theRelayRefusingTheMessageIsTooLarge()
{
    FakeRelayServer fake(httpResponse(413, "Request Entity Too Large",
                                       R"({"error":"message exceeds the maximum size"})"));
    QCOMPARE(readAgainst(fake, OpenPgpDecryptor(), m_fixture.path()).status, PgpReadStatus::TooLarge);
}

// The other mechanism, the same answer. A small ciphertext can expand
// without limit, so this one is refused as the plaintext is produced rather
// than by any bound on the wire -- but the reader is in the identical
// position either way, and so is the user.
void EncryptedMessageReaderTest::aPlaintextOverTheCeilingIsTheSameAnswer()
{
    const QByteArray armored = m_fixture.encryptToTestKey(QByteArray(256 * 1024, 'A'));
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(payloadResponse(armored));
    // Well under the plaintext and well under what a compressed 256 KB of one
    // repeated byte occupies on the wire, so this is the DECRYPTED size being
    // refused -- not the response.
    const OpenPgpDecryptor bounded(64 * 1024);
    const PgpReadResult result = readAgainst(fake, bounded, m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::TooLarge);
    QVERIFY2(result.plaintext.isEmpty(), "a partial plaintext was kept after the ceiling was hit");
}

// detail is relay-authored text. It reaches the caller only on the retryable
// status, where it explains an outage; on the terminal ones the status
// already says everything true, and passing the sentence through invites a
// UI that prints attacker-influenced text for a message we could not read.
void EncryptedMessageReaderTest::onlyTheRetryableStatusCarriesDetail()
{
    FakeRelayServer outage(httpResponse(503, "Service Unavailable", "unavailable", "text/plain"));
    const PgpReadResult failed = readAgainst(outage, OpenPgpDecryptor(), m_fixture.path());
    QCOMPARE(failed.status, PgpReadStatus::FetchFailed);
    QVERIFY2(!failed.detail.isEmpty(), "a retryable failure explained nothing");

    FakeRelayServer custody(httpResponse(409, "Conflict", R"({"error":"migrate your key"})"));
    const PgpReadResult terminal = readAgainst(custody, OpenPgpDecryptor(), m_fixture.path());
    QCOMPARE(terminal.status, PgpReadStatus::ServerCustody);
    QVERIFY2(terminal.detail.isEmpty(), "relay prose leaked out on a terminal status");
}

QTEST_GUILESS_MAIN(EncryptedMessageReaderTest)
#include "EncryptedMessageReaderTest.moc"
