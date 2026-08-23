#include "security/AppLockStore.h"

#include "security/LockoutPolicy.h"
#include "stores/SecureStore.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QRandomGenerator>

namespace {

// Fixed, documented key names, same convention PairingStore establishes.
const QString kLockEnabled = QStringLiteral("applock.enabled");
const QString kPinSalt = QStringLiteral("applock.pinSalt");
const QString kPinHash = QStringLiteral("applock.pinHash");
// Salt and hash as ONE value, so a reader can never observe them
// half-updated. Written as "<saltB64>:<hashB64>". kPinSalt/kPinHash above
// are the pre-2026-07-27 layout, still READ so that an install which
// already has a PIN keeps working; they are removed on the next setPin().
const QString kPinRecord = QStringLiteral("applock.pinRecord");
const QString kFailedAttempts = QStringLiteral("applock.failedAttempts");
const QString kLockoutUntil = QStringLiteral("applock.lockoutUntilEpochMs");
const QString kWipeAfterAttempts = QStringLiteral("applock.wipeAfterAttempts");
const QString kCredentialGate = QString::fromLatin1(AppLockStore::kCredentialGateKey);

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
    const SecureStore::ReadResult enabled = m_secureStore.read(kLockEnabled);
    // Fail CLOSED. This used to be a get().value_or(QString()) == "1", which
    // reads an unreachable keyring as "no lock configured" -- so stopping
    // gnome-keyring, or renaming ~/.local/share/keyrings, removed the PIN
    // screen, the lock overlay and the credential gate outright.
    // AppLockManager's constructor seeds m_locked from this, so the process
    // simply started unlocked.
    //
    // Reporting the lock as ON when the store cannot be read costs a user
    // with a genuinely broken keyring an unlock screen they cannot satisfy
    // -- recoverable, and AppLockManager::storeUnavailable() exists so the
    // UI can say why. Reporting it OFF costs them the lock.
    if (enabled.failed())
        return true;
    return enabled.value == QStringLiteral("1");
}

bool AppLockStore::storeReadable() const
{
    // One probe against a key that is always present once a lock exists and
    // is cheap to read. Absent is a fine answer here -- it means the store
    // answered -- so only Failed reports unreadable.
    return !m_secureStore.read(kLockEnabled).failed();
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

    // One write, one failure mode. As two writes this was only safe for the
    // FIRST PIN: on a change, applock.enabled is already "1", so a salt that
    // landed followed by a hash that did not left verifyPin() comparing
    // PBKDF2(pin, newSalt) against the OLD hash -- neither the old nor the
    // new PIN verified, with no way back. The user then guessed until the
    // tenth failure, at which point the app's own wipe destroyed the local
    // mail cache and the pairing.
    const QString record =
        QString::fromLatin1(salt.toBase64()) + QLatin1Char(':') + QString::fromLatin1(hash.toBase64());
    if (!m_secureStore.set(kPinRecord, record))
        return false;
    // Only once the new record is durable. A failure here is harmless: the
    // record already wins over the legacy pair in verifyPin().
    m_secureStore.remove(kPinSalt);
    m_secureStore.remove(kPinHash);
    return m_secureStore.set(kLockEnabled, QStringLiteral("1"));
}

bool AppLockStore::verifyPin(const QString& pin) const
{
    // The combined record wins; the split pair is the pre-2026-07-27 layout
    // and is still honoured so an existing PIN survives the upgrade.
    QString saltB64;
    QString hashB64;
    if (const std::optional<QString> record = m_secureStore.get(kPinRecord); record.has_value()) {
        const qsizetype sep = record->indexOf(QLatin1Char(':'));
        if (sep < 0)
            return false;
        saltB64 = record->left(sep);
        hashB64 = record->mid(sep + 1);
    } else {
        const std::optional<QString> legacySalt = m_secureStore.get(kPinSalt);
        const std::optional<QString> legacyHash = m_secureStore.get(kPinHash);
        // Fail closed: a store that lost its keys must not unlock the app.
        if (!legacySalt.has_value() || !legacyHash.has_value())
            return false;
        saltB64 = *legacySalt;
        hashB64 = *legacyHash;
    }

    const QByteArray salt = QByteArray::fromBase64(saltB64.toLatin1());
    const QByteArray expected = QByteArray::fromBase64(hashB64.toLatin1());
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
    ok = m_secureStore.remove(kPinRecord) && ok;
    ok = m_secureStore.set(kFailedAttempts, QStringLiteral("0")) && ok;
    ok = m_secureStore.set(kLockoutUntil, QStringLiteral("0")) && ok;
    ok = m_secureStore.set(kCredentialGate, QStringLiteral("0")) && ok;
    // Back to the default, not left as the user had it. clear() runs when the
    // lock is switched off and on the wipe path, and in both cases the next
    // person to set up a lock here is starting fresh -- a "never erase"
    // choice must not survive silently into their setup. Written explicitly
    // rather than removed so an absent-key removal cannot report a failure
    // this function would have to explain.
    ok = m_secureStore.set(kWipeAfterAttempts,
                            QString::number(LockoutPolicy::kDefaultWipeThreshold)) && ok;
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

int AppLockStore::wipeAfterAttempts() const
{
    const std::optional<QString> stored = m_secureStore.get(kWipeAfterAttempts);
    if (!stored.has_value())
        return LockoutPolicy::kDefaultWipeThreshold;

    bool parsed = false;
    const int value = stored->toInt(&parsed);
    if (!parsed)
        return LockoutPolicy::kDefaultWipeThreshold;

    // Clamped on the way out as well as the way in. A value that got here by
    // some route other than setWipeAfterAttempts() -- an older build, an
    // edited store -- must not be able to widen the policy.
    return LockoutPolicy::clampWipeThreshold(value);
}

bool AppLockStore::setWipeAfterAttempts(int attempts)
{
    return m_secureStore.set(kWipeAfterAttempts, QString::number(LockoutPolicy::clampWipeThreshold(attempts)));
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
    const SecureStore::ReadResult gate = m_secureStore.read(kCredentialGate);
    // Fail closed, same reasoning as lockEnabled(): an unreadable store
    // reporting "gate off" is what lets a caller take the plaintext branch
    // in PairingStore::storeDeviceSecret().
    if (gate.failed())
        return true;
    return gate.value == QStringLiteral("1");
}

bool AppLockStore::setCredentialPinGateEnabled(bool enabled)
{
    return m_secureStore.set(kCredentialGate, enabled ? QStringLiteral("1") : QStringLiteral("0"));
}
