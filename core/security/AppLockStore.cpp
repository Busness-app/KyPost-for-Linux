#include "security/AppLockStore.h"

#include "security/CredentialCipher.h"
#include "security/LockoutPolicy.h"
#include "stores/SecureStore.h"

#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QPasswordDigestor>
#include <QRandomGenerator>

#include <argon2.h>

namespace {

// Fixed, documented key names, same convention PairingStore establishes.
const QString kLockEnabled = QStringLiteral("applock.enabled");
const QString kPinSalt = QStringLiteral("applock.pinSalt");
const QString kPinHash = QStringLiteral("applock.pinHash");
// Salt and hash as ONE value, so a reader can never observe them
// half-updated. Written as "a2:<saltB64>:<hashB64>". kPinSalt/kPinHash above
// are the pre-2026-07-27 layout, still READ so that an install which
// already has a PIN keeps working; they are removed on the next setPin().
const QString kPinRecord = QStringLiteral("applock.pinRecord");
const QString kFailedAttempts = QStringLiteral("applock.failedAttempts");
const QString kLockoutUntil = QStringLiteral("applock.lockoutUntilEpochMs");
const QString kWipeAfterAttempts = QStringLiteral("applock.wipeAfterAttempts");
const QString kBackgroundGrace = QStringLiteral("applock.backgroundGraceSeconds");
const QString kCredentialGate = QString::fromLatin1(AppLockStore::kCredentialGateKey);

// Version marker for the Argon2id record. Base64 contains no ':', so a
// legacy "<saltB64>:<hashB64>" record can never begin with this and a
// prefixed record can never be mistaken for one -- and the split kPinSalt /
// kPinHash pair holds bare base64, so it is always PBKDF2.
//
// A build predating the marker reads "a2" as the salt and fails to verify,
// which is the fail-closed direction: a downgrade refuses the PIN rather
// than accepting it.
const QString kArgon2Prefix = QStringLiteral("a2:");

// Domain separation. CredentialCipher derives its session KEY from the bare
// PIN with these same parameters; the verifier derives from a prefixed input
// so the two values cannot coincide even if a salt were ever reused.
const QByteArray kVerifierDomain = QByteArrayLiteral("kypost.applock.pin-verifier.v1|");

// Matches Android's PBKDF2WithHmacSHA256(pin, salt, 150_000, 256-bit).
constexpr int kPbkdf2Iterations = 150000;
constexpr int kHashBytes = 32;
constexpr int kSaltBytes = 16;

// The KDF for every verifier written from now on.
//
// PBKDF2 here defeated the seal next door. Both cover the SAME six-digit PIN
// and live in the SAME keyring, so an offline attacker never touched
// CredentialCipher's memory-hard blob: they walked the 10^6 keyspace against
// this verifier at ~1.5e11 HMACs -- parallel, tiny working set, GPU-hours --
// and then ran Argon2id exactly once. Same parameters as the seal, or the
// cheaper of the two is the one that gets attacked. See
// CredentialCipher::kMagicArgon2id.
//
// Empty on failure (out of memory, most plausibly); every caller checks the
// size, so a record is never written from a derivation that did not run.
QByteArray hashPinArgon2id(const QString& pin, const QByteArray& salt)
{
    const QByteArray input = kVerifierDomain + pin.toUtf8();
    QByteArray out(kHashBytes, Qt::Uninitialized);

    const int rc = argon2id_hash_raw(
        CredentialCipher::kArgon2Iterations, CredentialCipher::kArgon2MemoryKiB,
        CredentialCipher::kArgon2Parallelism, input.constData(), static_cast<size_t>(input.size()),
        salt.constData(), static_cast<size_t>(salt.size()), out.data(),
        static_cast<size_t>(out.size()));

    if (rc != ARGON2_OK) {
        qWarning("AppLockStore: argon2id pin derivation failed (%s)", argon2_error_message(rc));
        return QByteArray();
    }
    return out;
}

// The pre-Argon2id verifier. Used to VERIFY records written before the
// marker existed, never to write one -- verifyPin() upgrades in place.
QByteArray hashPinPbkdf2Legacy(const QString& pin, const QByteArray& salt)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, pin.toUtf8(), salt,
                                               kPbkdf2Iterations, kHashBytes);
}

QByteArray freshSalt()
{
    QByteArray salt(kSaltBytes, Qt::Uninitialized);
    static_assert(kSaltBytes % 4 == 0, "generate() fills whole 32-bit words");
    // system() is the CSPRNG; the default global generator is not.
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(salt.data()),
                                          reinterpret_cast<quint32*>(salt.data() + salt.size()));
    return salt;
}

QString makeRecord(const QByteArray& salt, const QByteArray& hash)
{
    return kArgon2Prefix + QString::fromLatin1(salt.toBase64()) + QLatin1Char(':')
        + QString::fromLatin1(hash.toBase64());
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
    const QByteArray salt = freshSalt();
    const QByteArray hash = hashPinArgon2id(pin, salt);
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
    // hash(pin, newSalt) against the OLD hash -- neither the old nor the
    // new PIN verified, with no way back. The user then guessed until the
    // tenth failure, at which point the app's own wipe destroyed the local
    // mail cache and the pairing.
    if (!m_secureStore.set(kPinRecord, makeRecord(salt, hash)))
        return false;

    // Past this line the NEW pin is the one verifyPin() answers to, so every
    // remaining step is a step that cannot be rolled back -- which is exactly
    // why none of them may have its result thrown away.
    //
    // Enabling comes first, so a store that dies part-way leaves the lock ON
    // with a working pin rather than off with one.
    bool replaced = m_secureStore.set(kLockEnabled, QStringLiteral("1"));

    // Then the pre-2026-07-27 salt/hash pair. This is the OLD pin's material
    // and it is left readable in the keyring if these are skipped: verifyPin()
    // prefers the record so THIS build is unaffected, but any build that
    // predates the record -- a downgrade, a distro package one release behind,
    // a restored home directory -- reads the pair and unlocks on the pin the
    // user just replaced. Discarding these two results reported that as a
    // clean pin change. remove() returns true for an absent key in both
    // backends, so the common no-legacy-material path still returns true.
    replaced = m_secureStore.remove(kPinSalt) && replaced;
    replaced = m_secureStore.remove(kPinHash) && replaced;
    return replaced;
}

bool AppLockStore::verifyPin(const QString& pin, bool* couldNotEvaluate)
{
    if (couldNotEvaluate != nullptr)
        *couldNotEvaluate = false;

    // The combined record wins; the split pair is the pre-2026-07-27 layout
    // and is still honoured so an existing PIN survives the upgrade.
    QString saltB64;
    QString hashB64;
    bool legacyKdf = true;
    if (const std::optional<QString> record = m_secureStore.get(kPinRecord); record.has_value()) {
        QStringView body(*record);
        if (body.startsWith(kArgon2Prefix)) {
            legacyKdf = false;
            body = body.sliced(kArgon2Prefix.size());
        }
        const qsizetype sep = body.indexOf(QLatin1Char(':'));
        if (sep < 0)
            return false;
        saltB64 = body.left(sep).toString();
        hashB64 = body.sliced(sep + 1).toString();
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
    // KDF output, not the PIN), but constant-time costs nothing.
    const QByteArray actual = legacyKdf ? hashPinPbkdf2Legacy(pin, salt) : hashPinArgon2id(pin, salt);
    // A derivation that failed returns empty. Still fails CLOSED -- but it is
    // reported as its own answer, because the alternative is worse than a
    // refused unlock: argon2id's 64 MiB working set is genuinely refused on a
    // cgroup-capped or low-memory session, deterministically, so a user
    // entering the CORRECT pin would otherwise walk the failed-attempt
    // counter to ten and have their mail, pairing and lock destroyed.
    if (actual.isEmpty()) {
        if (couldNotEvaluate != nullptr)
            *couldNotEvaluate = true;
        return false;
    }
    if (actual.size() != expected.size())
        return false;
    quint8 diff = 0;
    for (int i = 0; i < actual.size(); ++i)
        diff |= static_cast<quint8>(actual[i]) ^ static_cast<quint8>(expected[i]);
    if (diff != 0)
        return false;

    if (legacyKdf)
        upgradePinVerifier(pin);
    return true;
}

// A PBKDF2 verifier that just proved correct is rewritten under Argon2id
// here, because leaving it IS the vulnerability: a cheap verifier for the
// same PIN beside CredentialCipher's memory-hard seal is the one an offline
// attacker attacks. The PIN is only in hand on this path, so this is the
// only place the rewrite can happen.
//
// Every failure is logged and swallowed. The user typed the RIGHT pin;
// refusing them because a rewrite did not land would be a worse bug than the
// weak verifier it replaces.
void AppLockStore::upgradePinVerifier(const QString& pin)
{
    const QByteArray salt = freshSalt();
    const QByteArray hash = hashPinArgon2id(pin, salt);
    if (hash.size() != kHashBytes) {
        qWarning("AppLockStore: pin verifier upgrade skipped, argon2id derivation failed");
        return;
    }

    if (!m_secureStore.set(kPinRecord, makeRecord(salt, hash))) {
        qWarning("AppLockStore: pin verifier upgrade failed, the pbkdf2 record stands");
        return;
    }

    // Only after the new record has landed: the split pair is this same pin's
    // material, and removing it first would leave nothing to verify against.
    if (!m_secureStore.remove(kPinSalt) || !m_secureStore.remove(kPinHash))
        qWarning("AppLockStore: pin verifier upgrade left the pre-2026-07-27 keys behind");
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
    // Same reasoning: a grace period the previous owner chose must not carry
    // into the next setup on this machine.
    ok = m_secureStore.set(kBackgroundGrace,
                            QString::number(LockoutPolicy::kDefaultBackgroundGraceSeconds)) && ok;
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

int AppLockStore::backgroundGraceSeconds() const
{
    const std::optional<QString> stored = m_secureStore.get(kBackgroundGrace);
    if (!stored.has_value())
        return LockoutPolicy::kDefaultBackgroundGraceSeconds;

    bool parsed = false;
    const int value = stored->toInt(&parsed);
    if (!parsed)
        return LockoutPolicy::kDefaultBackgroundGraceSeconds;

    return LockoutPolicy::clampBackgroundGraceSeconds(value);
}

bool AppLockStore::setBackgroundGraceSeconds(int seconds)
{
    return m_secureStore.set(kBackgroundGrace,
                              QString::number(LockoutPolicy::clampBackgroundGraceSeconds(seconds)));
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
