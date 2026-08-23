#include "mail/MailController.h"

#include "db/Database.h"
#include "db/EmailDao.h"
#include "db/FolderDao.h"
#include "domain/DevicePairing.h"
#include "domain/FolderRepository.h"
#include "domain/KeywordRepository.h"
#include "domain/MailRepository.h"
#include "domain/PairingStore.h"
#include "net/FolderClient.h"
#include "net/HttpClient.h"
#include "net/NetworkExecutor.h"
#include "net/PgpBootstrapClient.h"
#include "net/PgpRecipientChecker.h"
#include "net/RelayMailSource.h"
#include "pgp/OpenPgpDecryptor.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include "../../core/net/FakeRelayServer.h"
#include "../../core/pgp/GnupgFixture.h"
#include "MailPgpHarness.h"
#include "../ExecutorShutdownGuard.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QTest>

namespace {

// The string the whole file is about. If this ever appears in the database,
// the app has demoted an end-to-end encrypted message to the protection level
// of every other mail in the cache.
const char* const kCanary = "canary-plaintext-must-never-be-written";

// One client-protected row: pgpEncrypted, no body, no decrypt error. That
// exact shape is what PgpMessageState calls ClientProtected.
QByteArray inboxWithOneEncryptedMessage()
{
    return R"({
      "tabs": ["Uncategorized"],
      "byTab": {
        "Uncategorized": [
          {
            "messageId": "5",
            "sender": "sender@example.com",
            "sentTo": "me@example.com",
            "cc": "", "bcc": "",
            "subject": "Encrypted",
            "status": "unread",
            "atUtc": "2026-08-23T00:00:00Z",
            "hasAttachments": false,
            "label": "",
            "pgpEncrypted": true
          }
        ]
      }
    })";
}

QByteArray payloadResponse(const QByteArray& armored)
{
    const QJsonObject body{
        { QStringLiteral("messageId"), 5 },
        { QStringLiteral("mailbox"), QStringLiteral("INBOX") },
        { QStringLiteral("encryptedPayload"), QString::fromUtf8(armored) },
    };
    return httpResponse(200, "OK", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

// Every value in every column of every table, as one blob to search.
//
// Deliberately not "check the emails table": the point is that the plaintext
// is nowhere, and a targeted check would keep passing if some later change
// started writing it somewhere else.
QString everythingInTheDatabase(QSqlDatabase database)
{
    QString dump;
    QSqlQuery tables(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"), database);
    while (tables.next()) {
        const QString table = tables.value(0).toString();
        QSqlQuery rows(QStringLiteral("SELECT * FROM \"%1\"").arg(table), database);
        while (rows.next()) {
            const QSqlRecord record = rows.record();
            for (int i = 0; i < record.count(); ++i)
                dump += record.value(i).toString() + QLatin1Char('\n');
        }
    }
    return dump;
}

} // namespace

class MailDecryptionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void decryptingAClientProtectedMessageShowsItsText();
    void aDecryptedMessageNeverReachesTheDatabase();
    void forgettingDropsTheHeldPlaintext();
    void serverCustodyIsExplainedRatherThanRetried();
    void anOutageIsTheOneRetryableFailure();
    void aReplyForAReplacedAccountIsNeverShown();

private:
    GnupgFixture m_fixture;
};

void MailDecryptionTest::initTestCase()
{
    if (!OpenPgpDecryptor::engineAvailable())
        QSKIP("no usable gpg on this system -- client-side decryption is NOT covered");
    if (!m_fixture.build())
        QSKIP("could not build a throwaway GnuPG keyring -- client-side decryption is NOT covered");

    // The controller decrypts against the USER's keyring by design -- there
    // is no home-directory argument on that path and there should not be, so
    // production cannot be pointed somewhere else. GNUPGHOME is how the test
    // redirects it, and gpg reads it when it is spawned, so the ordering
    // relative to gpgme's own initialisation does not matter.
    qputenv("GNUPGHOME", m_fixture.path().toUtf8());
}

void MailDecryptionTest::cleanupTestCase()
{
    GnupgFixture::killAgent(m_fixture.path());
}

void MailDecryptionTest::decryptingAClientProtectedMessageShowsItsText()
{
    const QByteArray entity = QByteArray("Content-Type: text/plain; charset=utf-8\r\n\r\n") + kCanary + "\r\n";
    const QByteArray armored = m_fixture.encryptToTestKey(entity);
    QVERIFY(!armored.isEmpty());
    QVERIFY2(!armored.contains(kCanary), "the fixture did not actually encrypt anything");

    FakeRelayServer fake(httpResponse(200, "OK", inboxWithOneEncryptedMessage()));
    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    harness.controller->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->isBusy(), 5000);

    // The row really is the state this feature exists for.
    const QVariantMap row = harness.controller->findByMessageId(QStringLiteral("5"));
    QCOMPARE(row.value(QStringLiteral("pgpState")).toInt(), 1); // ClientProtected
    QVERIFY2(row.value(QStringLiteral("body")).toString().isEmpty(),
             "a client-protected row arrived with a body");

    fake.setResponse(payloadResponse(armored));
    harness.controller->decryptMessage(QStringLiteral("5"));
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->decryptBusy(), 15000);

    QCOMPARE(harness.controller->decryptFailure(), QString());
    QCOMPARE(harness.controller->decryptedMessageId(), QStringLiteral("5"));
    QVERIFY2(harness.controller->decryptedPlain().contains(QString::fromUtf8(kCanary)),
             "the decrypted text did not reach the reader");
    // The MIME headers are parsed away, not shown.
    QVERIFY2(!harness.controller->decryptedPlain().contains(QStringLiteral("Content-Type")),
             "MIME source was handed to the reader as the message");
}

// The rule the decrypt* properties document, enforced rather than asserted in
// a comment. Scans EVERY column of EVERY table, so it keeps holding if some
// later change starts writing the body somewhere other than the emails table.
void MailDecryptionTest::aDecryptedMessageNeverReachesTheDatabase()
{
    const QByteArray entity = QByteArray("Content-Type: text/plain; charset=utf-8\r\n\r\n") + kCanary + "\r\n";
    const QByteArray armored = m_fixture.encryptToTestKey(entity);
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(httpResponse(200, "OK", inboxWithOneEncryptedMessage()));
    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    harness.controller->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->isBusy(), 5000);

    fake.setResponse(payloadResponse(armored));
    harness.controller->decryptMessage(QStringLiteral("5"));
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->decryptBusy(), 15000);

    // It really did decrypt -- otherwise "the plaintext is not in the
    // database" would be true for the uninteresting reason.
    QVERIFY2(harness.controller->decryptedPlain().contains(QString::fromUtf8(kCanary)),
             "nothing was decrypted, so this test proves nothing");

    const QString stored = everythingInTheDatabase(harness.db.handle());
    QVERIFY2(!stored.contains(QString::fromUtf8(kCanary)),
             "the decrypted message was written to the database");
    QVERIFY2(!stored.contains(QStringLiteral("BEGIN PGP MESSAGE")),
             "the ciphertext was written to the database");

    // And a re-read of the cached row still has no body: the cache was not
    // quietly updated in memory either.
    QVERIFY(harness.controller->findByMessageId(QStringLiteral("5"))
                .value(QStringLiteral("body"))
                .toString()
                .isEmpty());
}

void MailDecryptionTest::forgettingDropsTheHeldPlaintext()
{
    const QByteArray entity = QByteArray("Content-Type: text/plain; charset=utf-8\r\n\r\n") + kCanary + "\r\n";
    const QByteArray armored = m_fixture.encryptToTestKey(entity);
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(httpResponse(200, "OK", inboxWithOneEncryptedMessage()));
    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    harness.controller->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->isBusy(), 5000);
    fake.setResponse(payloadResponse(armored));
    harness.controller->decryptMessage(QStringLiteral("5"));
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->decryptBusy(), 15000);
    QVERIFY(!harness.controller->decryptedPlain().isEmpty());

    // This is what the app lock calls. Nothing may survive it.
    harness.controller->forgetDecrypted();

    QCOMPARE(harness.controller->decryptedMessageId(), QString());
    QCOMPARE(harness.controller->decryptedPlain(), QString());
    QCOMPARE(harness.controller->decryptedHtml(), QString());
    QCOMPARE(harness.controller->decryptFailure(), QString());
}

// 409 has an instruction attached and no amount of retrying satisfies it.
void MailDecryptionTest::serverCustodyIsExplainedRatherThanRetried()
{
    FakeRelayServer fake(httpResponse(200, "OK", inboxWithOneEncryptedMessage()));
    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    harness.controller->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->isBusy(), 5000);

    fake.setResponse(httpResponse(409, "Conflict", R"({"error":"migrate","migrationNeeded":true})"));
    harness.controller->decryptMessage(QStringLiteral("5"));
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->decryptBusy(), 15000);

    QVERIFY2(!harness.controller->decryptFailure().isEmpty(), "the user was told nothing");
    QVERIFY2(!harness.controller->decryptRetryable(),
             "Retry was offered for a condition retrying cannot fix");
    QCOMPARE(harness.controller->decryptedMessageId(), QString());
}

void MailDecryptionTest::anOutageIsTheOneRetryableFailure()
{
    FakeRelayServer fake(httpResponse(200, "OK", inboxWithOneEncryptedMessage()));
    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    harness.controller->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->isBusy(), 5000);

    fake.setResponse(httpResponse(503, "Service Unavailable", "down", "text/plain"));
    harness.controller->decryptMessage(QStringLiteral("5"));
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->decryptBusy(), 15000);

    QVERIFY(!harness.controller->decryptFailure().isEmpty());
    QVERIFY2(harness.controller->decryptRetryable(), "an outage was reported as permanent");
}

// The stale-reply defect, at the site where the window is widest. pinentry
// waits for the user -- indefinitely, if the key is on a hardware token they
// have to walk across the room for -- so an account replacement landing
// mid-decrypt is far more plausible here than on any ordinary request. No
// table carries a subscriber column, so a plaintext shown after the swap is
// the previous account's mail in the new account's reader.
//
// Deterministic, not racy: decryptMessage() returns immediately and the
// reply is delivered through the event queue, so a pairing change made
// synchronously after the call is guaranteed to happen first. The test only
// enters the event loop at QTRY_VERIFY below.
void MailDecryptionTest::aReplyForAReplacedAccountIsNeverShown()
{
    const QByteArray entity = QByteArray("Content-Type: text/plain; charset=utf-8\r\n\r\n") + kCanary + "\r\n";
    const QByteArray armored = m_fixture.encryptToTestKey(entity);
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(httpResponse(200, "OK", inboxWithOneEncryptedMessage()));
    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    harness.controller->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->isBusy(), 5000);

    fake.setResponse(payloadResponse(armored));
    harness.controller->decryptMessage(QStringLiteral("5"));

    // A different account, on the same relay -- what pairing a second
    // subscriber on this device looks like.
    DevicePairing replacement;
    replacement.subscriberId = QStringLiteral("sub-2");
    replacement.deviceSecret = QStringLiteral("secret-2");
    replacement.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    replacement.deviceId = QStringLiteral("dev-2");
    QVERIFY(harness.pairingStore->save(replacement));

    QTRY_VERIFY_WITH_TIMEOUT(!harness.controller->decryptBusy(), 15000);

    QVERIFY2(!harness.controller->decryptedPlain().contains(QString::fromUtf8(kCanary)),
             "the previous account's decrypted mail was shown to the new account");
    QCOMPARE(harness.controller->decryptedMessageId(), QString());
    QCOMPARE(harness.controller->decryptedHtml(), QString());
}

// GUILESS, like MailControllerTest over the same sources. Nothing here draws
// anything, and QTEST_MAIN builds a QApplication that aborts on a headless
// runner before QtTest prints a single line -- which is exactly how this
// arrived in CI: "Subprocess aborted" with no output to read.
QTEST_GUILESS_MAIN(MailDecryptionTest)
#include "MailDecryptionTest.moc"
