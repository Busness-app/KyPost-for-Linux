#include "pgp/OpenPgpDecryptor.h"

#include "GnupgFixture.h"

#include <QByteArray>
#include <QFile>
#include <QProcessEnvironment>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>


class OpenPgpDecryptorTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void aMessageEncryptedToOurKeyComesBack();
    void aMessageForSomebodyElseReportsNoSecretKey();
    void rubbishIsReportedAsMalformedRatherThanShown();
    void emptyInputIsMalformed();
    void aPlaintextOverTheCeilingIsRefusedWholesale();

private:
    GnupgFixture m_fixture;
};

void OpenPgpDecryptorTest::initTestCase()
{
    if (!OpenPgpDecryptor::engineAvailable())
        QSKIP("no usable gpg on this system -- OpenPGP decryption is NOT covered");
    if (!m_fixture.build())
        QSKIP("could not build a throwaway GnuPG keyring -- OpenPGP decryption is NOT covered");
}

void OpenPgpDecryptorTest::cleanupTestCase()
{
    // gpg-agent starts on demand against the throwaway home and would
    // otherwise outlive the directory it is watching.
    QProcess::execute(QStringLiteral("gpgconf"),
                       { QStringLiteral("--homedir"), m_fixture.path(), QStringLiteral("--kill"),
                         QStringLiteral("all") });
}

void OpenPgpDecryptorTest::aMessageEncryptedToOurKeyComesBack()
{
    const QByteArray secret = "the quick brown fox reads its own mail\n";
    const QByteArray ciphertext = m_fixture.encryptToTestKey(secret);
    QVERIFY(!ciphertext.isEmpty());
    QVERIFY2(!ciphertext.contains("quick brown fox"), "the fixture did not actually encrypt anything");

    const PgpDecryptResult result = OpenPgpDecryptor().decrypt(ciphertext, m_fixture.path());
    QCOMPARE(result.status, PgpDecryptStatus::Decrypted);
    QCOMPARE(result.plaintext, secret);
}

// The most common real case after "it worked": mail encrypted to a key this
// device does not have. It must be its own answer -- "you cannot read this
// here" is actionable, "corrupt message" is a lie.
void OpenPgpDecryptorTest::aMessageForSomebodyElseReportsNoSecretKey()
{
    const QByteArray ciphertext = m_fixture.encryptToTestKey("not for you\n");
    QVERIFY(!ciphertext.isEmpty());

    QTemporaryDir strangerHome;
    QVERIFY(strangerHome.isValid());

    const PgpDecryptResult result =
        OpenPgpDecryptor().decrypt(ciphertext, GnupgFixture::emptyHome(strangerHome));
    QCOMPARE(result.status, PgpDecryptStatus::NoSecretKey);
    QVERIFY(result.plaintext.isEmpty());

    QProcess::execute(QStringLiteral("gpgconf"),
                       { QStringLiteral("--homedir"), strangerHome.path(), QStringLiteral("--kill"),
                         QStringLiteral("all") });
}

// Whatever this is, it is not a message, and it must never be handed up as
// one -- rendering ciphertext or an empty body as though the sender wrote it
// is the failure AGENTS.md 4a names specifically.
void OpenPgpDecryptorTest::rubbishIsReportedAsMalformedRatherThanShown()
{
    const PgpDecryptResult result =
        OpenPgpDecryptor().decrypt(QByteArray("-----BEGIN PGP MESSAGE-----\nnot base64 at all\n"),
                                    m_fixture.path());
    // Pinned to the exact status, not merely "not Decrypted". The loose form
    // is what let gpgme's GPG_ERR_NO_DATA sit mapped to NoSecretKey: garbage
    // armor reported "your key is on another machine" and this test was
    // happy, because that is indeed not Decrypted.
    QCOMPARE(result.status, PgpDecryptStatus::Malformed);
    QVERIFY2(result.plaintext.isEmpty(), "something was handed up for a message that never decrypted");
}

void OpenPgpDecryptorTest::emptyInputIsMalformed()
{
    const PgpDecryptResult result = OpenPgpDecryptor().decrypt(QByteArray(), m_fixture.path());
    QCOMPARE(result.status, PgpDecryptStatus::Malformed);
}

// OpenPGP carries compressed data, so a small ciphertext can expand without
// limit and no bound on the WIRE size constrains it. The ceiling is applied
// as the plaintext is produced, and nothing partial is kept: half a
// decrypted message is indistinguishable from a truncated one the sender
// actually wrote.
void OpenPgpDecryptorTest::aPlaintextOverTheCeilingIsRefusedWholesale()
{
    const QByteArray big(256 * 1024, 'A');
    const QByteArray ciphertext = m_fixture.encryptToTestKey(big);
    QVERIFY(!ciphertext.isEmpty());

    // A ceiling well under the plaintext, and well under what a compressed
    // 256 KB of one repeated byte occupies on the wire -- so this is the
    // decrypted size being refused, not the ciphertext.
    OpenPgpDecryptor bounded(64 * 1024);
    const PgpDecryptResult result = bounded.decrypt(ciphertext, m_fixture.path());

    QCOMPARE(result.status, PgpDecryptStatus::TooLarge);
    QVERIFY2(result.plaintext.isEmpty(), "a partial plaintext was kept after the ceiling was hit");

    // And the same message decrypts fine when it is allowed to.
    QCOMPARE(OpenPgpDecryptor().decrypt(ciphertext, m_fixture.path()).status,
             PgpDecryptStatus::Decrypted);
}

QTEST_GUILESS_MAIN(OpenPgpDecryptorTest)
#include "OpenPgpDecryptorTest.moc"
