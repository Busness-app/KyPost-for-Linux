#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <optional>
#include <utility>

// PIN-derived authenticated encryption for a single small secret (the device
// pairing secret), backing the "Require unlock to receive push/MFA" setting.
//
// Why OpenSSL rather than Qt: Qt ships no AEAD. QCryptographicHash and
// QPasswordDigestor cover hashing and key derivation, but there is no
// AES-GCM anywhere in Qt's public API, and hand-rolling an
// encrypt-then-MAC construction out of the pieces Qt does provide would be a
// worse answer than linking the library Qt itself already links for TLS.
// libcrypto is present in org.kde.Platform and on every target distro, so
// this costs nothing in packaging.
//
// Format produced by seal(): magic(4) || salt(16) || iv(12) || ciphertext ||
// tag(16), base64-encoded so it can live in a SecureStore string value. The
// salt and IV are stored alongside because both must be recovered before the
// key can be re-derived; neither is secret, and the PIN is what protects the
// whole blob.
//
// The 4-byte magic identifies the key-derivation function. Blobs written
// before it existed have no magic and are PBKDF2 -- see kMagicArgon2id.
namespace CredentialCipher {

inline constexpr int kSaltBytes = 16;
inline constexpr int kIvBytes = 12;  // AES-GCM's standard nonce length
inline constexpr int kTagBytes = 16; // full-length GCM tag
inline constexpr int kKeyBytes = 32; // AES-256

// Version marker, and the reason the format grew one.
//
// This blob is attacked OFFLINE. The threat is not someone guessing at the
// unlock screen -- LockoutPolicy handles that -- it is someone who has a
// copy of the keychain entry and all the time in the world. The PIN is six
// digits by policy, so the entire keyspace is 10^6. PBKDF2-HMAC-SHA256 at
// 150k iterations makes that ~1.5e11 HMACs, and PBKDF2 is embarrassingly
// parallel with a tiny working set: one consumer GPU walks it in hours.
// Raising the iteration count does not change that -- it scales the
// attacker's cost and the user's unlock delay by the same factor.
//
// Argon2id's memory cost does change it: at 64 MiB per guess a GPU can run
// only as many guesses in parallel as it has spare gigabytes, which is the
// property PBKDF2 has never had. Existing blobs are not rewritten in place
// (that would need the PIN, which this layer does not hold); they open under
// the old KDF and are upgraded the next time something re-seals -- a PIN
// change, or the relay rotating the device secret.
inline constexpr char kMagicArgon2id[4] = { 'K', 'Y', 'A', '2' };
inline constexpr int kMagicBytes = 4;

// Argon2id parameters. 64 MiB / t=3 / p=1 is the OWASP-recommended
// second-choice profile and takes roughly 100 ms on the slowest machine this
// app targets -- the same order as the PBKDF2 cost it replaces, so unlock
// does not get noticeably slower while the offline attack gets orders of
// magnitude more expensive.
inline constexpr quint32 kArgon2MemoryKiB = 64 * 1024;
inline constexpr quint32 kArgon2Iterations = 3;
inline constexpr quint32 kArgon2Parallelism = 1;

// Legacy KDF, still used to OPEN blobs written before kMagicArgon2id.
// Never used to seal.
inline constexpr int kPbkdf2Iterations = 150000;

// The PBKDF2 output and the salt it was derived from, handed back by
// openWithKey() so a caller holding an unlocked session can re-seal a
// ROTATED secret without retaining the PIN itself. See sealWithKey().
struct SessionKey
{
    QByteArray key;  // kKeyBytes
    QByteArray salt; // kSaltBytes, the one embedded in the blob it came from
    // Which KDF produced `key` from `salt`. Carried so sealWithKey() re-emits
    // a blob the same key can open: re-sealing a legacy blob's key under an
    // Argon2id header would produce something neither KDF could reproduce.
    // An Argon2id key is not upgraded here either -- the upgrade needs the
    // PIN, and this struct exists precisely so the PIN need not be retained.
    bool legacyPbkdf2 = false;

    bool isValid() const { return key.size() == kKeyBytes && salt.size() == kSaltBytes; }
};

// Encrypts `plaintext` under a key derived from `pin`. Returns std::nullopt
// only if the platform RNG or libcrypto fails -- an empty plaintext is
// legal and round-trips.
std::optional<QString> seal(const QString& pin, const QByteArray& plaintext);

// Returns std::nullopt when the PIN is wrong, the blob is truncated, or the
// GCM tag does not verify. These are deliberately indistinguishable to the
// caller: "wrong PIN" and "tampered blob" should both mean "you don't get
// the secret", and reporting which would tell an attacker whether they had
// found a real ciphertext.
std::optional<QByteArray> open(const QString& pin, const QString& sealed);

// open(), plus the derived key and salt, so the caller can re-seal later in
// the same session. Exists because the relay rotates the device secret on
// every re-registration: without this, PairingStore would have to either
// retain the PIN (a strictly larger secret -- it also authorizes disabling
// the lock) or write the rotated secret out in plaintext, which is exactly
// the bug this pair of functions was added to fix.
std::optional<std::pair<QByteArray, SessionKey>> openWithKey(const QString& pin, const QString& sealed);

// seal(), but reusing a key/salt already derived by openWithKey() rather
// than running PBKDF2 again. A FRESH IV is generated per call -- reusing
// the salt is harmless (it only seeds key derivation, and the key is
// unchanged), but reusing a key/IV pair under GCM would be catastrophic, so
// the IV is never carried over. Returns std::nullopt if `sessionKey` is not
// valid or libcrypto fails.
std::optional<QString> sealWithKey(const SessionKey& sessionKey, const QByteArray& plaintext);

} // namespace CredentialCipher
