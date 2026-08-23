#include "MailPgpHarness.h"

#include "pgp/OpenPgpDecryptor.h"
#include "pgp/OpenPgpKeyImporter.h"

#include "../../core/pgp/GnupgFixture.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

namespace {

const char* const kSecret = "the-body-that-must-not-travel-in-clear";

QByteArray bootstrapResponse(const QString& address)
{
    const QJsonObject body{
        { QStringLiteral("hasIdentity"), true },
        { QStringLiteral("protection"), QStringLiteral("client") },
        { QStringLiteral("suggestedUserIDs"), QJsonArray{ address } },
    };
    return httpResponse(200, "OK", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

QByteArray resolveResponse(const QString& address, const QByteArray& armoredKey,
                            const QString& fingerprint, bool usable = true)
{
    const QJsonObject entry{
        { QStringLiteral("address"), address },
        { QStringLiteral("publicKey"), QString::fromUtf8(armoredKey) },
        { QStringLiteral("fingerprint"), fingerprint },
        { QStringLiteral("tier"), QStringLiteral("contact") },
        { QStringLiteral("usable"), usable },
    };
    const QJsonObject body{ { QStringLiteral("results"), QJsonArray{ entry } } };
    return httpResponse(200, "OK", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

QJsonObject sendRequestBodyOf(const QList<QByteArray>& requests)
{
    for (const QByteArray& request : requests) {
        if (!request.startsWith("POST /api/mail/send-pgp"))
            continue;
        const int headerEnd = request.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return {};
        return QJsonDocument::fromJson(request.mid(headerEnd + 4)).object();
    }
    return {};
}

} // namespace

class MailEncryptedSendTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void theRelayOnlyEverSeesCiphertext();
    void theRecipientCanOpenWhatTheRelayRelayed();
    void aRecipientWithNoUsableKeyStopsTheSendAndIsNamed();
    void aKeyTheRelaySaysIsUnusableIsNotUsedEvenThoughItLooksFine();
    void attachmentsAreRefusedRatherThanDropped();
    void aSendForAReplacedAccountIsNotReportedAsSent();

private:
    GnupgFixture m_sender;
    GnupgFixture m_recipient;
    QString m_recipientFingerprint;
    QByteArray m_recipientKey;
};

void MailEncryptedSendTest::initTestCase()
{
    if (!OpenPgpDecryptor::engineAvailable())
        QSKIP("no usable gpg on this system -- client-encrypted sending is NOT covered");
    if (!m_sender.build(QStringLiteral("Sender <me@example.com>"))
        || !m_recipient.build(QStringLiteral("Recipient <you@example.com>"))) {
        QSKIP("could not build throwaway GnuPG keyrings -- client-encrypted sending is NOT covered");
    }

    // The controller signs with the USER's keyring, which is what GNUPGHOME
    // redirects. Production takes no home-directory argument on that path and
    // should not: it must not be pointable somewhere else.
    qputenv("GNUPGHOME", m_sender.path().toUtf8());

    m_recipientKey = m_recipient.exportPublicKey(QStringLiteral("you@example.com"));
    m_recipientFingerprint = m_recipient.fingerprintOf(QStringLiteral("you@example.com"));
    QVERIFY(!m_recipientKey.isEmpty());
    QVERIFY(!m_recipientFingerprint.isEmpty());
}

void MailEncryptedSendTest::cleanupTestCase()
{
    GnupgFixture::killAgent(m_sender.path());
    GnupgFixture::killAgent(m_recipient.path());
}

// The whole point of the path. Everything the relay receives about this
// message -- body and subject alike -- must be ciphertext.
void MailEncryptedSendTest::theRelayOnlyEverSeesCiphertext()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true})"));
    fake.setResponseForPath("/api/pgp/bootstrap", bootstrapResponse(QStringLiteral("me@example.com")));
    fake.setResponseForPath("/api/pgp/recipients/resolve",
                             resolveResponse(QStringLiteral("you@example.com"), m_recipientKey,
                                              m_recipientFingerprint));

    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    QSignalSpy completed(harness.controller.get(), &MailController::sendCompleted);
    harness.controller->sendClientEncrypted(QStringLiteral("you@example.com"), QString(), QString(),
                                             QStringLiteral("Redundancies confirmed"),
                                             QString::fromUtf8(kSecret), {});
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() == 1, 30000);
    QVERIFY2(completed.at(0).at(1).toBool(), qPrintable(harness.controller->lastError()));

    const QJsonObject body = sendRequestBodyOf(fake.receivedRequests());
    QVERIFY2(!body.isEmpty(), "no send request reached the relay");

    const QByteArray raw = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QVERIFY2(!raw.contains(kSecret), "the body reached the relay in clear");
    QVERIFY2(!raw.contains("Redundancies"), "the real subject reached the relay in clear");
    QCOMPARE(body.value(QStringLiteral("subject")).toString(),
             QStringLiteral("[Encrypted] Email Sent by KyPost"));
    QCOMPARE(body.value(QStringLiteral("deliveries")).toArray().size(), 1);
}

// And it is not merely opaque -- the intended reader can actually open it.
void MailEncryptedSendTest::theRecipientCanOpenWhatTheRelayRelayed()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true})"));
    fake.setResponseForPath("/api/pgp/bootstrap", bootstrapResponse(QStringLiteral("me@example.com")));
    fake.setResponseForPath("/api/pgp/recipients/resolve",
                             resolveResponse(QStringLiteral("you@example.com"), m_recipientKey,
                                              m_recipientFingerprint));

    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    QSignalSpy completed(harness.controller.get(), &MailController::sendCompleted);
    harness.controller->sendClientEncrypted(QStringLiteral("you@example.com"), QString(), QString(),
                                             QStringLiteral("Subject"), QString::fromUtf8(kSecret), {});
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() == 1, 30000);
    QVERIFY(completed.at(0).at(1).toBool());

    const QJsonObject body = sendRequestBodyOf(fake.receivedRequests());
    const QString delivery =
        body.value(QStringLiteral("deliveries")).toArray().at(0).toObject()
            .value(QStringLiteral("ciphertext")).toString();
    const QByteArray bytes = delivery.toUtf8();

    const qsizetype begin = bytes.indexOf("-----BEGIN PGP MESSAGE-----");
    const qsizetype end = bytes.indexOf("-----END PGP MESSAGE-----");
    QVERIFY2(begin >= 0 && end > begin, "the delivery carries no OpenPGP message");
    const QByteArray armor = bytes.mid(begin, end - begin + qstrlen("-----END PGP MESSAGE-----"));

    const PgpDecryptResult opened = OpenPgpDecryptor().decrypt(armor, m_recipient.path());
    QCOMPARE(opened.status, PgpDecryptStatus::Decrypted);
    QVERIFY2(opened.plaintext.contains(kSecret), "the recipient cannot read the message sent to them");
}

// No downgrade, and no anonymous failure: the user is told who could not be
// written to. Sending it unencrypted is not offered -- the relay's own
// plaintext fallback stores the message on the relay, which is what this mode
// exists to prevent.
void MailEncryptedSendTest::aRecipientWithNoUsableKeyStopsTheSendAndIsNamed()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));
    fake.setResponseForPath("/api/pgp/bootstrap", bootstrapResponse(QStringLiteral("me@example.com")));
    fake.setResponseForPath("/api/pgp/recipients/resolve",
                             resolveResponse(QStringLiteral("stranger@example.com"), {}, {},
                                              /*usable=*/false));

    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    QSignalSpy completed(harness.controller.get(), &MailController::sendCompleted);
    harness.controller->sendClientEncrypted(QStringLiteral("stranger@example.com"), QString(),
                                             QString(), QStringLiteral("Subject"),
                                             QString::fromUtf8(kSecret), {});
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() == 1, 30000);

    QVERIFY2(!completed.at(0).at(1).toBool(), "a message with no usable recipient key was sent");
    QVERIFY2(harness.controller->lastError().contains(QStringLiteral("stranger@example.com")),
             "the user is not told which recipient has no key");
    QVERIFY2(sendRequestBodyOf(fake.receivedRequests()).isEmpty(),
             "a send request was made for a message that cannot be encrypted");
}

// The `usable` flag is the relay's verdict on a key it CAN produce -- revoked,
// expired, wrong capability. The key here is valid and imports cleanly, so
// nothing downstream would object to it; only obeying the flag stops it.
//
// The sibling test above uses an absent key, which the importer rejects on its
// own, so it cannot tell whether this flag is doing anything. This one can.
void MailEncryptedSendTest::aKeyTheRelaySaysIsUnusableIsNotUsedEvenThoughItLooksFine()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));
    fake.setResponseForPath("/api/pgp/bootstrap", bootstrapResponse(QStringLiteral("me@example.com")));
    fake.setResponseForPath("/api/pgp/recipients/resolve",
                             resolveResponse(QStringLiteral("you@example.com"), m_recipientKey,
                                              m_recipientFingerprint, /*usable=*/false));

    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    QSignalSpy completed(harness.controller.get(), &MailController::sendCompleted);
    harness.controller->sendClientEncrypted(QStringLiteral("you@example.com"), QString(), QString(),
                                             QStringLiteral("Subject"), QString::fromUtf8(kSecret), {});
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() == 1, 30000);

    QVERIFY2(!completed.at(0).at(1).toBool(), "a key the relay marked unusable was encrypted to");
    QVERIFY2(sendRequestBodyOf(fake.receivedRequests()).isEmpty(), "the message was sent anyway");
}

// Enforced in C++, not only in Compose.qml. Sending the message without the
// files and reporting success is a data loss the sender never sees.
void MailEncryptedSendTest::attachmentsAreRefusedRatherThanDropped()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));
    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    const quint64 token = harness.controller->sendClientEncrypted(
        QStringLiteral("you@example.com"), QString(), QString(), QStringLiteral("Subject"),
        QString::fromUtf8(kSecret), { QStringLiteral("/tmp/whatever.pdf") });

    QCOMPARE(token, quint64(0));
    QVERIFY(!harness.controller->lastError().isEmpty());
    QVERIFY2(fake.receivedRequests().isEmpty(), "a request was made for a refused send");
}

// pinentry waits for the user indefinitely, so this window is wider here than
// anywhere else. Reporting the old account's send into the new account's
// compose screen would read as "your message was sent".
void MailEncryptedSendTest::aSendForAReplacedAccountIsNotReportedAsSent()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true})"));
    fake.setResponseForPath("/api/pgp/bootstrap", bootstrapResponse(QStringLiteral("me@example.com")));
    fake.setResponseForPath("/api/pgp/recipients/resolve",
                             resolveResponse(QStringLiteral("you@example.com"), m_recipientKey,
                                              m_recipientFingerprint));

    DecryptHarness harness;
    QVERIFY(harness.build(fake));

    QSignalSpy completed(harness.controller.get(), &MailController::sendCompleted);
    harness.controller->sendClientEncrypted(QStringLiteral("you@example.com"), QString(), QString(),
                                             QStringLiteral("Subject"), QString::fromUtf8(kSecret), {});

    // Synchronously, before the event loop runs: the reply is queued, so this
    // is guaranteed to land first.
    DevicePairing replacement;
    replacement.subscriberId = QStringLiteral("sub-2");
    replacement.deviceSecret = QStringLiteral("secret-2");
    replacement.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    replacement.deviceId = QStringLiteral("dev-2");
    QVERIFY(harness.pairingStore->save(replacement));

    QTRY_VERIFY_WITH_TIMEOUT(completed.count() == 1, 30000);
    QVERIFY2(!completed.at(0).at(1).toBool(),
             "a send belonging to a replaced account was reported as sent");
}

QTEST_GUILESS_MAIN(MailEncryptedSendTest)
#include "MailEncryptedSendTest.moc"
