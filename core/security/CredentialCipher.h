#pragma once

#include <QByteArray>
#include <QString>
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
// Format produced by seal(): salt(16) || iv(12) || ciphertext || tag(16),
// base64-encoded so it can live in a SecureStore string value. The salt and
// IV are stored alongside because both must be recovered before the key can
// be re-derived; neither is secret, and the PIN is what protects the whole
// blob.
namespace CredentialCipher {

inline constexpr int kSaltBytes = 16;
inline constexpr int kIvBytes = 12;  // AES-GCM's standard nonce length
inline constexpr int kTagBytes = 16; // full-length GCM tag
inline constexpr int kKeyBytes = 32; // AES-256
// Same cost as the PIN hash (see AppLockStore) and as Android's
// PBKDF2WithHmacSHA256 usage, so a device that can unlock can also unwrap
// without a second, different delay.
inline constexpr int kPbkdf2Iterations = 150000;

// The PBKDF2 output and the salt it was derived from, handed back by
// openWithKey() so a caller holding an unlocked session can re-seal a
// ROTATED secret without retaining the PIN itself. See sealWithKey().
struct SessionKey
{
    QByteArray key;  // kKeyBytes
    QByteArray salt; // kSaltBytes, the one embedded in the blob it came from

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
