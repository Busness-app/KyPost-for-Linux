#include "pgp/OpenPgpEncryptor.h"

#include "pgp/OpenPgpDecryptor.h"
#include "pgp/OpenPgpKeyImporter.h"

#include "GnupgFixture.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

// Two real keyrings, arranged the way a send actually is: the sender holds
// their own secret key plus the recipient's PUBLIC key (imported from the
// relay's discovery ladder, with no web-of-trust path to it), and the
// recipient holds their own secret key. Encrypting in one and decrypting in
// the other is the only evidence that matters here.
class OpenPgpEncryptorTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void theRecipientCanReadWhatTheSenderEncrypted();
    void theMessageIsSignedAndTheSignatureVerifies();
    void aStrangerCannotReadIt();
    void everyRecipientOfATwoWayMessageCanReadIt();
    void oneMissingRecipientKeyFailsTheWholeMessage();
    void aSenderWithNoSecretKeySendsNothingRatherThanSendingItUnsigned();
    void thereIsNothingToEncryptWithNoRecipients();
    void emptyInputIsRefused();

private:
    QString importInto(const QString& home, const GnupgFixture& from, const QString& uid) const;

    GnupgFixture m_sender;   // holds sender@example.com (secret)
    GnupgFixture m_recipient; // holds recipient@example.com (secret)
    GnupgFixture m_second;    // holds second@example.com (secret)
    QString m_recipientFingerprint;
    QString m_secondFingerprint;
};

QString OpenPgpEncryptorTest::importInto(const QString& home, const GnupgFixture& from,
                                          const QString& uid) const
{
    const QByteArray armored = from.exportPublicKey(uid);
    if (armored.isEmpty())
        return {};
    // Through the real importer, not a gpg --import shortcut: this is how the
    // key gets there in production, fingerprint check and all.
    const PgpImportResult result = importPublicKey(armored, from.fingerprintOf(uid), home);
    if (result.status != PgpImportStatus::Imported && result.status != PgpImportStatus::Unchanged)
        return {};
    return result.fingerprint;
}

void OpenPgpEncryptorTest::initTestCase()
{
    if (!OpenPgpDecryptor::engineAvailable())
        QSKIP("no usable gpg on this system -- signing and encryption are NOT covered");
    if (!m_sender.build(QStringLiteral("KyPost Sender <sender@example.com>"))
        || !m_recipient.build(QStringLiteral("KyPost Recipient <recipient@example.com>"))
        || !m_second.build(QStringLiteral("KyPost Second <second@example.com>"))) {
        QSKIP("could not build throwaway GnuPG keyrings -- signing and encryption are NOT covered");
    }

    m_recipientFingerprint =
        importInto(m_sender.path(), m_recipient, QStringLiteral("recipient@example.com"));
    m_secondFingerprint = importInto(m_sender.path(), m_second, QStringLiteral("second@example.com"));
    QVERIFY(!m_recipientFingerprint.isEmpty());
    QVERIFY(!m_secondFingerprint.isEmpty());
}

void OpenPgpEncryptorTest::cleanupTestCase()
{
    GnupgFixture::killAgent(m_sender.path());
    GnupgFixture::killAgent(m_recipient.path());
    GnupgFixture::killAgent(m_second.path());
}

// The whole point, end to end, and note what the sender's keyring holds: an
// imported public key with no certification from anyone. That is what every
// key off the discovery ladder looks like, and it is why ALWAYS_TRUST is set.
void OpenPgpEncryptorTest::theRecipientCanReadWhatTheSenderEncrypted()
{
    const QByteArray secret = "the numbers are worse than we said\n";

    const PgpEncryptResult encrypted = signAndEncrypt(
        secret, QStringLiteral("sender@example.com"), { m_recipientFingerprint }, m_sender.path());

    QCOMPARE(encrypted.status, PgpEncryptStatus::Encrypted);
    QVERIFY(encrypted.armoredCiphertext.startsWith(QStringLiteral("-----BEGIN PGP MESSAGE-----")));
    QVERIFY2(!encrypted.armoredCiphertext.contains(QStringLiteral("worse than we said")),
             "the plaintext is sitting in the ciphertext");

    const PgpDecryptResult decrypted =
        OpenPgpDecryptor().decrypt(encrypted.armoredCiphertext.toUtf8(), m_recipient.path());

    QCOMPARE(decrypted.status, PgpDecryptStatus::Decrypted);
    QCOMPARE(decrypted.plaintext, secret);
}

// Combined sign+encrypt, not encrypt-only. The reading path verifies an
// inline signature, so producing an unsigned message would be a silent
// downgrade that no test of the plaintext would notice.
void OpenPgpEncryptorTest::theMessageIsSignedAndTheSignatureVerifies()
{
    const PgpEncryptResult encrypted =
        signAndEncrypt("signed and sealed\n", QStringLiteral("sender@example.com"),
                        { m_recipientFingerprint }, m_sender.path());
    QCOMPARE(encrypted.status, PgpEncryptStatus::Encrypted);

    // The recipient needs the sender's public key to check the signature --
    // which is exactly the arrangement on a real reply.
    QVERIFY(!importInto(m_recipient.path(), m_sender, QStringLiteral("sender@example.com")).isEmpty());

    QProcess gpg;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GNUPGHOME"), m_recipient.path());
    gpg.setProcessEnvironment(env);
    gpg.start(QStringLiteral("gpg"), { QStringLiteral("--batch"), QStringLiteral("--status-fd"),
                                        QStringLiteral("1"), QStringLiteral("--decrypt") });
    QVERIFY(gpg.waitForStarted(10000));
    gpg.write(encrypted.armoredCiphertext.toUtf8());
    gpg.closeWriteChannel();
    QVERIFY(gpg.waitForFinished(30000));

    const QByteArray status = gpg.readAllStandardOutput();
    QVERIFY2(status.contains("GOODSIG"), "the message carried no good signature");
    QVERIFY2(status.contains("VALIDSIG"), "the signature did not validate");
}

void OpenPgpEncryptorTest::aStrangerCannotReadIt()
{
    const PgpEncryptResult encrypted =
        signAndEncrypt("not for you\n", QStringLiteral("sender@example.com"),
                        { m_recipientFingerprint }, m_sender.path());
    QCOMPARE(encrypted.status, PgpEncryptStatus::Encrypted);

    // m_second holds its own secret key and was not a recipient of this one.
    const PgpDecryptResult decrypted =
        OpenPgpDecryptor().decrypt(encrypted.armoredCiphertext.toUtf8(), m_second.path());

    QCOMPARE(decrypted.status, PgpDecryptStatus::NoSecretKey);
    QVERIFY(decrypted.plaintext.isEmpty());
}

void OpenPgpEncryptorTest::everyRecipientOfATwoWayMessageCanReadIt()
{
    const QByteArray secret = "both of you\n";

    const PgpEncryptResult encrypted =
        signAndEncrypt(secret, QStringLiteral("sender@example.com"),
                        { m_recipientFingerprint, m_secondFingerprint }, m_sender.path());
    QCOMPARE(encrypted.status, PgpEncryptStatus::Encrypted);

    QCOMPARE(OpenPgpDecryptor().decrypt(encrypted.armoredCiphertext.toUtf8(), m_recipient.path()).plaintext,
             secret);
    QCOMPARE(OpenPgpDecryptor().decrypt(encrypted.armoredCiphertext.toUtf8(), m_second.path()).plaintext,
             secret);
}

// gpgme will encrypt to the recipients it can and report the rest as invalid.
// Taking that would send a message the sender believes went to two people and
// that only one can open -- and on a Bcc split the failing delivery is the one
// nobody sees.
void OpenPgpEncryptorTest::oneMissingRecipientKeyFailsTheWholeMessage()
{
    const QString absent = QStringLiteral("0000000000000000000000000000000000000000");

    const PgpEncryptResult encrypted =
        signAndEncrypt("half a message\n", QStringLiteral("sender@example.com"),
                        { m_recipientFingerprint, absent }, m_sender.path());

    QCOMPARE(encrypted.status, PgpEncryptStatus::RecipientKeyUnusable);
    QVERIFY2(encrypted.armoredCiphertext.isEmpty(),
             "ciphertext was produced for a message that could not reach everyone");
    QVERIFY2(encrypted.unusableRecipients.contains(absent),
             "the user is not told WHICH recipient could not be encrypted to");
}

// Falling back to an unsigned message would be a downgrade the recipient
// cannot see, on an account that normally signs.
void OpenPgpEncryptorTest::aSenderWithNoSecretKeySendsNothingRatherThanSendingItUnsigned()
{
    const PgpEncryptResult encrypted =
        signAndEncrypt("who am i\n", QStringLiteral("nobody@example.com"),
                        { m_recipientFingerprint }, m_sender.path());

    QCOMPARE(encrypted.status, PgpEncryptStatus::NoSigningKey);
    QVERIFY(encrypted.armoredCiphertext.isEmpty());
}

void OpenPgpEncryptorTest::thereIsNothingToEncryptWithNoRecipients()
{
    const PgpEncryptResult encrypted =
        signAndEncrypt("hello\n", QStringLiteral("sender@example.com"), {}, m_sender.path());

    QCOMPARE(encrypted.status, PgpEncryptStatus::Failed);
    QVERIFY(encrypted.armoredCiphertext.isEmpty());
}

void OpenPgpEncryptorTest::emptyInputIsRefused()
{
    QCOMPARE(signAndEncrypt(QByteArray(), QStringLiteral("sender@example.com"),
                             { m_recipientFingerprint }, m_sender.path())
                 .status,
             PgpEncryptStatus::Failed);
}

QTEST_GUILESS_MAIN(OpenPgpEncryptorTest)
#include "OpenPgpEncryptorTest.moc"
