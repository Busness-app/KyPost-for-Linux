#include "pgp/OpenPgpKeyImporter.h"

#include "pgp/OpenPgpDecryptor.h"

#include "GnupgFixture.h"

#include <QTemporaryDir>
#include <QTest>

// Importing modifies the USER'S OWN GnuPG keyring, which is a side effect of
// sending mail and the reason this file is as careful as it is. Every test
// here works against a throwaway home; none of them can touch a real one.
class OpenPgpKeyImporterTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void aRecipientKeyBecomesUsableToGpg();
    void importingTheSameKeyTwiceChangesNothing();
    void aFingerprintThatDoesNotMatchIsRefusedBeforeTheKeyringIsTouched();
    void rubbishIsRefused();
    void emptyInputIsRefused();
    void importingOneKeyDoesNotDisturbAnother();
    void aNewerCopyOfAKeyMergesRatherThanReplaces();
    void withNoExpectedFingerprintTheKeyStillReportsItsOwn();

private:
    // The "sender" keyring: where the recipient's key comes FROM, standing in
    // for whatever the relay's discovery ladder found.
    GnupgFixture m_donor;
};

void OpenPgpKeyImporterTest::initTestCase()
{
    if (!OpenPgpDecryptor::engineAvailable())
        QSKIP("no usable gpg on this system -- key import is NOT covered");
    if (!m_donor.build())
        QSKIP("could not build a throwaway GnuPG keyring -- key import is NOT covered");
}

void OpenPgpKeyImporterTest::cleanupTestCase()
{
    GnupgFixture::killAgent(m_donor.path());
}

void OpenPgpKeyImporterTest::aRecipientKeyBecomesUsableToGpg()
{
    const QByteArray armored = m_donor.exportPublicKey(QStringLiteral("test@example.com"));
    QVERIFY(!armored.isEmpty());
    const QString fingerprint = m_donor.fingerprintOf(QStringLiteral("test@example.com"));
    QVERIFY(!fingerprint.isEmpty());

    QTemporaryDir target;
    QVERIFY(target.isValid());
    const QString home = GnupgFixture::emptyHome(target);

    const PgpImportResult result = importPublicKey(armored, fingerprint, home);

    QCOMPARE(result.status, PgpImportStatus::Imported);
    QCOMPARE(result.fingerprint, fingerprint);
    // And gpg really has it -- the return value is not the evidence.
    QVERIFY2(GnupgFixture::fingerprintsIn(home).contains(fingerprint),
             "the key is not in the keyring afterwards");

    GnupgFixture::killAgent(home);
}

// A resend must not churn the keyring. gpg merges, so a second import of the
// same bytes is a no-op and reports so.
void OpenPgpKeyImporterTest::importingTheSameKeyTwiceChangesNothing()
{
    const QByteArray armored = m_donor.exportPublicKey(QStringLiteral("test@example.com"));
    const QString fingerprint = m_donor.fingerprintOf(QStringLiteral("test@example.com"));
    QVERIFY(!armored.isEmpty());

    QTemporaryDir target;
    QVERIFY(target.isValid());
    const QString home = GnupgFixture::emptyHome(target);

    QCOMPARE(importPublicKey(armored, fingerprint, home).status, PgpImportStatus::Imported);
    QCOMPARE(importPublicKey(armored, fingerprint, home).status, PgpImportStatus::Unchanged);
    QCOMPARE(GnupgFixture::fingerprintsIn(home).size(), 1);

    GnupgFixture::killAgent(home);
}

// The relay's key and the relay's claim about that key disagreeing is not a
// reason to pick one of them. The user's keyring must come out untouched --
// which is why the check runs in a scratch home first rather than importing
// and then deleting.
void OpenPgpKeyImporterTest::aFingerprintThatDoesNotMatchIsRefusedBeforeTheKeyringIsTouched()
{
    const QByteArray armored = m_donor.exportPublicKey(QStringLiteral("test@example.com"));
    QVERIFY(!armored.isEmpty());

    QTemporaryDir target;
    QVERIFY(target.isValid());
    const QString home = GnupgFixture::emptyHome(target);

    const PgpImportResult result = importPublicKey(
        armored, QStringLiteral("0000000000000000000000000000000000000000"), home);

    QCOMPARE(result.status, PgpImportStatus::Rejected);
    QCOMPARE(result.detail, QStringLiteral("fingerprint mismatch"));
    // The whole point: nothing entered the keyring, not even briefly.
    QVERIFY2(GnupgFixture::fingerprintsIn(home).isEmpty(),
             "a key with a mismatched fingerprint reached the user's keyring");
    // And it still reports what gpg actually computed, which is what a log
    // line needs to be useful.
    QVERIFY(!result.fingerprint.isEmpty());

    GnupgFixture::killAgent(home);
}

void OpenPgpKeyImporterTest::rubbishIsRefused()
{
    QTemporaryDir target;
    QVERIFY(target.isValid());
    const QString home = GnupgFixture::emptyHome(target);

    const PgpImportResult result =
        importPublicKey(QByteArray("-----BEGIN PGP PUBLIC KEY BLOCK-----\nnot base64\n"), QString(),
                         home);

    QCOMPARE(result.status, PgpImportStatus::Rejected);
    QVERIFY(GnupgFixture::fingerprintsIn(home).isEmpty());

    GnupgFixture::killAgent(home);
}

void OpenPgpKeyImporterTest::emptyInputIsRefused()
{
    QCOMPARE(importPublicKey(QByteArray(), QString(), QString()).status, PgpImportStatus::Rejected);
}

// Non-destructive, part one: importing a second recipient's key leaves the
// first alone. gpg merges rather than replaces, and this proves it for this
// code path rather than taking gpg's word for it.
void OpenPgpKeyImporterTest::importingOneKeyDoesNotDisturbAnother()
{
    QVERIFY(m_donor.generateKey(QStringLiteral("Second Person <second@example.com>")));

    const QByteArray first = m_donor.exportPublicKey(QStringLiteral("test@example.com"));
    const QByteArray second = m_donor.exportPublicKey(QStringLiteral("second@example.com"));
    const QString firstFpr = m_donor.fingerprintOf(QStringLiteral("test@example.com"));
    const QString secondFpr = m_donor.fingerprintOf(QStringLiteral("second@example.com"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    QVERIFY(firstFpr != secondFpr);

    QTemporaryDir target;
    QVERIFY(target.isValid());
    const QString home = GnupgFixture::emptyHome(target);

    QCOMPARE(importPublicKey(first, firstFpr, home).status, PgpImportStatus::Imported);
    QCOMPARE(importPublicKey(second, secondFpr, home).status, PgpImportStatus::Imported);

    const QStringList present = GnupgFixture::fingerprintsIn(home);
    QVERIFY2(present.contains(firstFpr), "importing a second key removed the first");
    QVERIFY2(present.contains(secondFpr), "the second key is missing");

    GnupgFixture::killAgent(home);
}

// Non-destructive, part two, and the one that actually matters: a NEWER copy
// of a key the keyring already holds must gain what is new without losing what
// was there. "gpg merges" is the claim the custody decision rests on, so it is
// measured.
void OpenPgpKeyImporterTest::aNewerCopyOfAKeyMergesRatherThanReplaces()
{
    const QByteArray before = m_donor.exportPublicKey(QStringLiteral("test@example.com"));
    const QString fingerprint = m_donor.fingerprintOf(QStringLiteral("test@example.com"));
    QVERIFY(!before.isEmpty());

    QTemporaryDir target;
    QVERIFY(target.isValid());
    const QString home = GnupgFixture::emptyHome(target);
    QCOMPARE(importPublicKey(before, fingerprint, home).status, PgpImportStatus::Imported);

    const QStringList uidsBefore = GnupgFixture::uidsIn(home, fingerprint);
    QVERIFY(!uidsBefore.isEmpty());

    // The same key, now carrying an extra identity.
    QVERIFY(m_donor.addUid(QStringLiteral("test@example.com"),
                            QStringLiteral("KyPost Test Alias <alias@example.com>")));
    const QByteArray after = m_donor.exportPublicKey(QStringLiteral("test@example.com"));
    QVERIFY(after != before);

    QCOMPARE(importPublicKey(after, fingerprint, home).status, PgpImportStatus::Imported);

    const QStringList uidsAfter = GnupgFixture::uidsIn(home, fingerprint);
    QVERIFY2(uidsAfter.size() > uidsBefore.size(), "the newer copy added nothing");
    for (const QString& uid : uidsBefore) {
        QVERIFY2(uidsAfter.contains(uid),
                 "a user ID the keyring already held was lost when a newer copy was imported");
    }

    GnupgFixture::killAgent(home);
}

void OpenPgpKeyImporterTest::withNoExpectedFingerprintTheKeyStillReportsItsOwn()
{
    const QByteArray armored = m_donor.exportPublicKey(QStringLiteral("test@example.com"));
    const QString fingerprint = m_donor.fingerprintOf(QStringLiteral("test@example.com"));

    QTemporaryDir target;
    QVERIFY(target.isValid());
    const QString home = GnupgFixture::emptyHome(target);

    const PgpImportResult result = importPublicKey(armored, QString(), home);

    QCOMPARE(result.status, PgpImportStatus::Imported);
    QCOMPARE(result.fingerprint, fingerprint);

    GnupgFixture::killAgent(home);
}

QTEST_GUILESS_MAIN(OpenPgpKeyImporterTest)
#include "OpenPgpKeyImporterTest.moc"
