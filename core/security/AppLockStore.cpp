#include "security/AppLockStore.h"

#include "stores/SecureStore.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QRandomGenerator>

namespace {

// Fixed, documented key names, same convention PairingStore establishes.
const QString kLockEnabled = QStringLiteral("applock.enabled");
const QString kPinSalt = QStringLiteral("applock.pinSalt");
const QString kPinHash = QStringLiteral("applock.pinHash");
const QString kFailedAttempts = QStringLiteral("applock.failedAttempts");
const QString kLockoutUntil = QStringLiteral("applock.lockoutUntilEpochMs");
const QString kCredentialGate = QStringLiteral("applock.credentialPinGateEnabled");

// Matches Android's PBKDF2WithHmacSHA256(pin, salt, 150_000, 256-bit).
constexpr int kIterations = 150000;
constexpr int kHashBytes = 32;
constexpr int kSaltBytes = 16;

QByteArray hashPin(const QString& pin, const QByteArray& salt)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, pin.toUtf8(), salt, kIterations,
                                               kHashBytes);
}

} // namespace

AppLockStore::AppLockStore(SecureStore& secureStore)
    : m_secureStore(secureStore)
{
}

bool AppLockStore::lockEnabled() const
{
    return m_secureStore.get(kLockEnabled).value_or(QString()) == QStringLiteral("1");
}

bool AppLockStore::setPin(const QString& pin)
{
    QByteArray salt(kSaltBytes, Qt::Uninitialized);
    static_assert(kSaltBytes % 4 == 0, "generate() fills whole 32-bit words");
    // system() is the CSPRNG; the default global generator is not.
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(salt.data()),
                                          reinterpret_cast<quint32*>(salt.data() + salt.size()));

    const QByteArray hash = hashPin(pin, salt);
    if (hash.size() != kHashBytes)
        return false;

    // Counters first, and CHECKED. These two used to run last with their
    // results discarded, so a failed write left a brand-new PIN sitting
    // behind a lockout deadline from the OLD one -- the user sets a new PIN
    // and is then refused for up to fifteen minutes by a backoff that
    // logically no longer exists. Doing them before the credential material
    // means a failure here changes nothing at all, which is what makes
    // returning false honest.
    if (!setFailedAttemptCount(0))
        return false;
    if (!setLockoutUntilEpochMs(0))
        return false;

    // Order matters: write the credential material before flipping the
    // enabled flag, so a failure part-way through can never leave the app
    // "locked" with no way to verify a PIN.
    if (!m_secureStore.set(kPinSalt, QString::fromLatin1(salt.toBase64())))
        return false;
    if (!m_secureStore.set(kPinHash, QString::fromLatin1(hash.toBase64())))
        return false;
    return m_secureStore.set(kLockEnabled, QStringLiteral("1"));
}

bool AppLockStore::verifyPin(const QString& pin) const
{
    const std::optional<QString> saltB64 = m_secureStore.get(kPinSalt);
    const std::optional<QString> hashB64 = m_secureStore.get(kPinHash);
    // Fail closed: a store that lost its keys must not unlock the app.
    if (!saltB64.has_value() || !hashB64.has_value())
        return false;

    const QByteArray salt = QByteArray::fromBase64(saltB64->toLatin1());
    const QByteArray expected = QByteArray::fromBase64(hashB64->toLatin1());
    if (salt.isEmpty() || expected.size() != kHashBytes)
        return false;

    // QByteArray::operator== short-circuits on the first differing byte.
    // The timing signal that leaks is tiny here (the comparison is over a
    // PBKDF2 output, not the PIN), but constant-time costs nothing.
    const QByteArray actual = hashPin(pin, salt);
    if (actual.size() != expected.size())
        return false;
    quint8 diff = 0;
    for (int i = 0; i < actual.size(); ++i)
        diff |= static_cast<quint8>(actual[i]) ^ static_cast<quint8>(expected[i]);
    return diff == 0;
}

bool AppLockStore::clear()
{
    // Clear the enabled flag first: if a later remove() fails, the app is
    // unlocked-but-with-stale-material rather than locked-with-no-PIN.
    bool ok = m_secureStore.set(kLockEnabled, QStringLiteral("0"));
    ok = m_secureStore.remove(kPinSalt) && ok;
    ok = m_secureStore.remove(kPinHash) && ok;
    ok = m_secureStore.set(kFailedAttempts, QStringLiteral("0")) && ok;
    ok = m_secureStore.set(kLockoutUntil, QStringLiteral("0")) && ok;
    ok = m_secureStore.set(kCredentialGate, QStringLiteral("0")) && ok;
    return ok;
}

int AppLockStore::failedAttemptCount() const
{
    return m_secureStore.get(kFailedAttempts).value_or(QStringLiteral("0")).toInt();
}

bool AppLockStore::setFailedAttemptCount(int count)
{
    return m_secureStore.set(kFailedAttempts, QString::number(count));
}

qint64 AppLockStore::lockoutUntilEpochMs() const
{
    return m_secureStore.get(kLockoutUntil).value_or(QStringLiteral("0")).toLongLong();
}

bool AppLockStore::setLockoutUntilEpochMs(qint64 epochMs)
{
    return m_secureStore.set(kLockoutUntil, QString::number(epochMs));
}

bool AppLockStore::credentialPinGateEnabled() const
{
    return m_secureStore.get(kCredentialGate).value_or(QString()) == QStringLiteral("1");
}

bool AppLockStore::setCredentialPinGateEnabled(bool enabled)
{
    return m_secureStore.set(kCredentialGate, enabled ? QStringLiteral("1") : QStringLiteral("0"));
}
