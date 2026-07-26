#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

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

} // namespace CredentialCipher
