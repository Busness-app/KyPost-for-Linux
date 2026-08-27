#pragma once

// A throwaway GnuPG home with one key in it, shared by the OpenPGP tests.
//
// Lifted out of OpenPgpDecryptorTest.cpp when EncryptedMessageReaderTest
// needed the same thing, on the same reasoning as tests/core/net/
// FakeRelayServer.h: one copy rather than a second hand-edited one that
// drifts.
//
// Hermetic on purpose -- see the class comment below.

#include <QByteArray>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

// A throwaway GnuPG home with one key in it.
//
// Hermetic on purpose: these tests must not read, write or depend on the
// developer's own keyring, and must not leave anything in it. Everything
// lives under a QTemporaryDir that goes away with the test.
class GnupgFixture
{
public:
    // The uid defaults to the original single-key fixture, so every existing
    // caller is unaffected; tests that need two parties name their own.
    bool build(const QString& uid = QStringLiteral("KyPost Test <test@example.com>"))
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
                    uid, QStringLiteral("default"),
                    QStringLiteral("default"), QStringLiteral("never") })) {
            return false;
        }
        return true;
    }

    // The layout that actually has a signing SUBKEY: a certify-only [C]
    // primary with separate [S] and [E] subkeys.
    //
    // What build() makes is an [SC] primary that signs with itself, so a
    // signature's fingerprint and the key's are the same string there and no
    // test using it can tell the two apart. This one is what a hardware token
    // holds, and what Sequoia and Proton generate by default.
    bool buildWithSigningSubkey(const QString& uid)
    {
        if (!m_home.isValid())
            return false;
        QFile::setPermissions(m_home.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner);
        if (!run(QStringLiteral("gpg"),
                  { QStringLiteral("--batch"), QStringLiteral("--pinentry-mode"),
                    QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString(),
                    QStringLiteral("--quick-generate-key"), uid, QStringLiteral("default"),
                    QStringLiteral("cert"), QStringLiteral("never") })) {
            return false;
        }
        const QString primary = firstFingerprint(m_home.path(), uid);
        if (primary.isEmpty())
            return false;
        for (const QString& usage : { QStringLiteral("sign"), QStringLiteral("encr") }) {
            if (!run(QStringLiteral("gpg"),
                      { QStringLiteral("--batch"), QStringLiteral("--pinentry-mode"),
                        QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString(),
                        QStringLiteral("--quick-add-key"), primary, QStringLiteral("default"), usage,
                        QStringLiteral("never") })) {
                return false;
            }
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

    // A key that is ALREADY EXPIRED, for the case gpg holds a key and still
    // refuses to encrypt to it.
    //
    // Generated under a faked clock rather than with a short lifetime and a
    // wait: a test that sleeps for a key to expire is slow and depends on the
    // wall clock, and this one has to be deterministic to be worth having.
    bool buildExpired(const QString& uid)
    {
        if (!m_home.isValid())
            return false;
        QFile::setPermissions(m_home.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner);
        return run(QStringLiteral("gpg"),
                    { QStringLiteral("--batch"), QStringLiteral("--faked-system-time"),
                      QStringLiteral("20200101T000000"), QStringLiteral("--pinentry-mode"),
                      QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString(),
                      QStringLiteral("--quick-generate-key"), uid, QStringLiteral("default"),
                      QStringLiteral("default"), QStringLiteral("1d") });
    }

    // Generates a SECOND identity in this same home, so a test has two keys to
    // tell apart.
    bool generateKey(const QString& uid) const
    {
        return run(QStringLiteral("gpg"),
                    { QStringLiteral("--batch"), QStringLiteral("--pinentry-mode"),
                      QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString(),
                      QStringLiteral("--quick-generate-key"), uid, QStringLiteral("default"),
                      QStringLiteral("default"), QStringLiteral("never") });
    }

    // The armored PUBLIC key for a uid -- what the relay would hand a client.
    QByteArray exportPublicKey(const QString& uid) const
    {
        QProcess gpg;
        gpg.setProcessEnvironment(environment());
        gpg.start(QStringLiteral("gpg"),
                   { QStringLiteral("--batch"), QStringLiteral("--armor"),
                     QStringLiteral("--export"), uid });
        if (!gpg.waitForStarted(10000) || !gpg.waitForFinished(30000) || gpg.exitCode() != 0)
            return {};
        return gpg.readAllStandardOutput();
    }

    // SEVERAL keys in ONE armored blob -- what a hostile relay returns when
    // it wants an extra key carried in beside the one that was asked for.
    QByteArray exportPublicKeys(const QStringList& uids) const
    {
        return runGpg(QStringList{ QStringLiteral("--batch"), QStringLiteral("--armor"),
                                    QStringLiteral("--export") }
                       + uids);
    }

    // The armored PRIVATE key(s) -- what the enrollment path imports. One uid
    // or several; the several-uid form is the smuggling case for that path.
    QByteArray exportSecretKeys(const QStringList& uids) const
    {
        return runGpg(QStringList{ QStringLiteral("--batch"), QStringLiteral("--pinentry-mode"),
                                    QStringLiteral("loopback"), QStringLiteral("--passphrase"),
                                    QString(), QStringLiteral("--armor"),
                                    QStringLiteral("--export-secret-keys") }
                       + uids);
    }

    QString fingerprintOf(const QString& uid) const { return firstFingerprint(m_home.path(), uid); }

    // The same ciphertext with one byte of the encrypted packet flipped, and a
    // VALID armor checksum.
    //
    // Re-armoring matters. Editing the armor text directly trips the CRC, and
    // a CRC failure is not the property under test -- an attacker recomputes
    // it. This tampers the binary packet and lets gpg write the armor, which
    // is what a modified message actually looks like on arrival.
    QByteArray tamperedCopyOf(const QByteArray& armored) const
    {
        QByteArray binary = runWithInput(QStringLiteral("gpg"), { QStringLiteral("--dearmor") }, armored);
        if (binary.size() < 24)
            return {};
        // Well inside the symmetrically-encrypted data packet, near its end.
        binary[binary.size() - 20] = static_cast<char>(binary.at(binary.size() - 20) ^ 0x01);

        QByteArray rearmored =
            runWithInput(QStringLiteral("gpg"), { QStringLiteral("--enarmor") }, binary);
        // --enarmor labels it a generic armored file; the label is what a
        // reader dispatches on.
        rearmored.replace("ARMORED FILE", "MESSAGE");
        return rearmored;
    }


    // Adds a user ID to an existing key, so a test has a genuinely NEWER copy
    // of the same key to import over the old one.
    bool addUid(const QString& uid, const QString& extraUid) const
    {
        return run(QStringLiteral("gpg"),
                    { QStringLiteral("--batch"), QStringLiteral("--pinentry-mode"),
                      QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString(),
                      QStringLiteral("--quick-add-uid"), uid, extraUid });
    }

    // Every PRIMARY key fingerprint in a keyring, so a test can assert that
    // importing one key did not disturb another.
    //
    // Primary only. In --with-colons output an `fpr:` record follows both
    // `pub:` and `sub:`, so collecting every one of them counts a modern key
    // twice -- it has an encryption subkey -- and "one key was imported" then
    // reads as two.
    static QStringList fingerprintsIn(const QString& home)
    {
        QProcess gpg;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GNUPGHOME"), home);
        gpg.setProcessEnvironment(env);
        gpg.start(QStringLiteral("gpg"), { QStringLiteral("--batch"), QStringLiteral("--with-colons"),
                                            QStringLiteral("--fingerprint"),
                                            QStringLiteral("--list-keys") });
        if (!gpg.waitForStarted(10000) || !gpg.waitForFinished(30000))
            return {};
        QStringList fingerprints;
        bool afterPrimary = false;
        const QString output = QString::fromUtf8(gpg.readAllStandardOutput());
        for (const QString& line : output.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("pub:")))
                afterPrimary = true;
            else if (line.startsWith(QStringLiteral("sub:")))
                afterPrimary = false;
            else if (afterPrimary && line.startsWith(QStringLiteral("fpr:"))) {
                fingerprints.append(line.section(QLatin1Char(':'), 9, 9));
                afterPrimary = false;
            }
        }
        fingerprints.removeDuplicates();
        return fingerprints;
    }

    // The user IDs on a key, so the merge test can show the old one survived.
    static QStringList uidsIn(const QString& home, const QString& fingerprint)
    {
        QProcess gpg;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GNUPGHOME"), home);
        gpg.setProcessEnvironment(env);
        gpg.start(QStringLiteral("gpg"), { QStringLiteral("--batch"), QStringLiteral("--with-colons"),
                                            QStringLiteral("--list-keys"), fingerprint });
        if (!gpg.waitForStarted(10000) || !gpg.waitForFinished(30000))
            return {};
        QStringList uids;
        const QString output = QString::fromUtf8(gpg.readAllStandardOutput());
        for (const QString& line : output.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("uid:")))
                uids.append(line.section(QLatin1Char(':'), 9, 9));
        }
        return uids;
    }

    static QString firstFingerprint(const QString& home, const QString& uid)
    {
        QProcess gpg;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GNUPGHOME"), home);
        gpg.setProcessEnvironment(env);
        gpg.start(QStringLiteral("gpg"), { QStringLiteral("--batch"), QStringLiteral("--with-colons"),
                                            QStringLiteral("--fingerprint"),
                                            QStringLiteral("--list-keys"), uid });
        if (!gpg.waitForStarted(10000) || !gpg.waitForFinished(30000) || gpg.exitCode() != 0)
            return {};
        const QString output = QString::fromUtf8(gpg.readAllStandardOutput());
        for (const QString& line : output.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("fpr:")))
                return line.section(QLatin1Char(':'), 9, 9);
        }
        return {};
    }

    // gpg-agent starts on demand against a throwaway home and would
    // otherwise outlive the directory it is watching.
    static void killAgent(const QString& home)
    {
        QProcess::execute(QStringLiteral("gpgconf"),
                           { QStringLiteral("--homedir"), home, QStringLiteral("--kill"),
                             QStringLiteral("all") });
    }

    // A SECOND, empty keyring -- the "somebody else's mail" case.
    static QString emptyHome(QTemporaryDir& dir)
    {
        QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner);
        return dir.path();
    }

private:
    QByteArray runGpg(const QStringList& args) const
    {
        QProcess gpg;
        gpg.setProcessEnvironment(environment());
        gpg.start(QStringLiteral("gpg"), args);
        if (!gpg.waitForStarted(10000) || !gpg.waitForFinished(30000) || gpg.exitCode() != 0)
            return {};
        return gpg.readAllStandardOutput();
    }

    QByteArray runWithInput(const QString& program, const QStringList& args,
                             const QByteArray& input) const
    {
        QProcess process;
        process.setProcessEnvironment(environment());
        process.start(program, args);
        if (!process.waitForStarted(10000))
            return {};
        process.write(input);
        process.closeWriteChannel();
        if (!process.waitForFinished(30000))
            return {};
        return process.readAllStandardOutput();
    }

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
