#include "pgp/OpenPgpDecryptor.h"

#include <QByteArray>
#include <QFile>
#include <QProcessEnvironment>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {

// A throwaway GnuPG home with one key in it.
//
// Hermetic on purpose: these tests must not read, write or depend on the
// developer's own keyring, and must not leave anything in it. Everything
// lives under a QTemporaryDir that goes away with the test.
class GnupgFixture
{
public:
    bool build()
    {
        if (!m_home.isValid())
            return false;
        // gpg refuses to use a home directory others can read.
        QFile::setPermissions(m_home.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner);

        if (!run(QStringLiteral("gpg"),
                  { QStringLiteral("--batch"), QStringLiteral("--pinentry-mode"),
                    QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString(),
                    QStringLiteral("--quick-generate-key"),
                    QStringLiteral("KyPost Test <test@example.com>"), QStringLiteral("default"),
                    QStringLiteral("default"), QStringLiteral("never") })) {
            return false;
        }
        return true;
    }

    QByteArray encryptToTestKey(const QByteArray& plaintext) const
    {
        QProcess gpg;
        gpg.setProcessEnvironment(environment());
        gpg.start(QStringLiteral("gpg"),
                   { QStringLiteral("--batch"), QStringLiteral("--yes"), QStringLiteral("--trust-model"),
                     QStringLiteral("always"), QStringLiteral("--encrypt"), QStringLiteral("--armor"),
                     QStringLiteral("--recipient"), QStringLiteral("test@example.com") });
        if (!gpg.waitForStarted(10000))
            return {};
        gpg.write(plaintext);
        gpg.closeWriteChannel();
        if (!gpg.waitForFinished(30000) || gpg.exitCode() != 0)
            return {};
        return gpg.readAllStandardOutput();
    }

    QString path() const { return m_home.path(); }

    // A SECOND, empty keyring -- the "somebody else's mail" case.
    static QString emptyHome(QTemporaryDir& dir)
    {
        QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner);
        return dir.path();
    }

private:
    QProcessEnvironment environment() const
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GNUPGHOME"), m_home.path());
        return env;
    }

    bool run(const QString& program, const QStringList& args) const
    {
        QProcess process;
        process.setProcessEnvironment(environment());
        process.start(program, args);
        if (!process.waitForStarted(10000))
            return false;
        if (!process.waitForFinished(60000))
            return false;
        return process.exitCode() == 0;
    }

    QTemporaryDir m_home;
};

} // namespace

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
    QVERIFY(result.status != PgpDecryptStatus::Decrypted);
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
