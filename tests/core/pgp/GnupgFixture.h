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
