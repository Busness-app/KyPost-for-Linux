#pragma once

#include <QString>

// What counts as an acceptable app-lock PIN.
//
// Lives in core/ and is enforced in AppLockManager (C++), not only in
// Settings.qml: QML is a presentation layer, not a security boundary -- a
// QML mistake, a future second entry point, or anything reaching AppLock
// through the QML engine must not be able to install a one-character PIN.
//
// This matters more than a normal "password strength" rule because the PIN
// is not only compared against a stored hash: with the credential gate on
// it is the key-encryption key for the relay device secret
// (core/security/CredentialCipher.h). An attacker holding a copy of the
// keychain entry can attack it offline, where the runtime backoff and
// wipe-after-10 policy (core/security/LockoutPolicy.h) do not exist.
namespace PinPolicy {

// Six digits, matching the sibling clients.
//
// This file used to claim six digits was "the point at which an offline
// PBKDF2-150k search stops being trivially cheap". It was not: 10^6
// candidates at 150k iterations is ~1.5e11 HMACs of a function that is
// embarrassingly parallel and needs almost no memory, which one consumer GPU
// walks in hours. Length was doing far less work than that sentence claimed.
//
// The fix was the KDF, not the floor -- the seal now derives its key with
// Argon2id at 64 MiB per guess, which caps an attacker's parallelism at
// however many spare gigabytes their hardware has. See
// CredentialCipher::kMagicArgon2id. Six digits remains the minimum for
// parity with Android and iOS; a longer or alphanumeric PIN still helps and
// nothing here prevents one (see kMaximumLength).
inline constexpr int kMinimumLength = 6;
inline constexpr int kMaximumLength = 64;

enum class Rejection {
    Ok,
    TooShort,
    TooLong,
    // "111111" / "000000" -- one guess covers the whole class.
    AllSameCharacter,
    // "123456" / "654321" -- likewise.
    Sequential,
};

// Ok when `pin` may be installed as the app-lock PIN.
Rejection validate(const QString& pin);

inline bool isAcceptable(const QString& pin)
{
    return validate(pin) == Rejection::Ok;
}

} // namespace PinPolicy
