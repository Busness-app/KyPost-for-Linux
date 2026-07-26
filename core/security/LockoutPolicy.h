#pragma once

#include <QtGlobal>

// Failed-PIN-attempt policy, ported from kypost-android's LockoutPolicy so
// both clients punish guessing identically.
//
// Deliberately a pure function of (failedAttempts, now) with no storage and
// no clock of its own: the caller owns both, which is what makes every
// branch here testable without waiting real seconds or touching a keychain.
namespace LockoutPolicy {

// Attempts allowed before the app wipes itself. Reaching this is treated as
// "someone is guessing", not "the owner is having a bad day" -- by this
// point they have already been made to wait through the backoff below.
inline constexpr int kWipeThreshold = 10;

// Attempts allowed before any backoff kicks in at all. Fat-fingering a PIN
// twice shouldn't cost the owner a delay.
inline constexpr int kFreeAttempts = 3;

// Backoff for the Nth consecutive failure, in milliseconds. 0 means "no
// wait". Doubles from 30s at the 4th failure and is capped so the final
// attempts before a wipe stay bounded.
qint64 lockoutDurationMs(int failedAttempts);

// True once the attempt count has reached the wipe threshold.
bool shouldWipe(int failedAttempts);

// Whether a lockout deadline is still in the future. `nowEpochMs` is passed
// in rather than read from the system clock so tests can drive it.
bool isLockedOut(qint64 lockoutUntilEpochMs, qint64 nowEpochMs);

// Whole seconds still to wait, rounded up, or 0 when not locked out. Rounded
// up so a UI countdown never displays "0 seconds remaining" while still
// refusing input.
int remainingLockoutSeconds(qint64 lockoutUntilEpochMs, qint64 nowEpochMs);

} // namespace LockoutPolicy
