#pragma once

#include <QtGlobal>

// Failed-PIN-attempt policy, ported from kypost-android's LockoutPolicy so
// both clients punish guessing identically.
//
// Deliberately a pure function of (failedAttempts, now) with no storage and
// no clock of its own: the caller owns both, which is what makes every
// branch here testable without waiting real seconds or touching a keychain.
namespace LockoutPolicy {

// Attempts allowed before the app wipes itself, when the user has not chosen
// otherwise. Reaching this is treated as "someone is guessing", not "the
// owner is having a bad day" -- by this point they have already been made to
// wait through the backoff below.
inline constexpr int kDefaultWipeThreshold = 10;

// The user may move the threshold within this range, or switch the erase off
// entirely with kWipeNever.
//
// The floor is not arbitrary: kFreeAttempts below means the first three
// failures cost no delay at all, so anything at or under 4 would erase the
// device before its owner had been made to wait even once. The ceiling is
// where the setting stops meaning anything -- past it, reaching the
// threshold takes so many relaunches that the backoff has already made
// guessing pointless.
inline constexpr int kWipeNever = 0;
inline constexpr int kMinWipeThreshold = 5;
inline constexpr int kMaxWipeThreshold = 20;

// Consecutive failures IN ONE PROCESS after which guesses are refused
// outright, whatever the wipe threshold is set to -- including "never".
//
// Deliberately separate from the wipe threshold, and deliberately not
// configurable. These were one number, and folding them together meant
// switching the erase off ALSO switched off the only limit that does not
// depend on the system clock: the persisted backoff can be skipped by an
// attacker who holds the machine and moves the clock forward, and this
// counter is what still stops them. Turning off "erase this device" must
// cost the user the erase, not the rate limit.
inline constexpr int kSessionRefuseFloor = 10;

// Attempts allowed before any backoff kicks in at all. Fat-fingering a PIN
// twice shouldn't cost the owner a delay.
inline constexpr int kFreeAttempts = 3;

// Backoff for the Nth consecutive failure, in milliseconds. 0 means "no
// wait". Doubles from 30s at the 4th failure and is capped so the final
// attempts before a wipe stay bounded.
qint64 lockoutDurationMs(int failedAttempts);

// True once the attempt count has reached `threshold`. A threshold of
// kWipeNever (or any non-positive value) never wipes.
//
// The threshold is a parameter rather than read from a store here because
// this namespace is deliberately a pure function of its arguments: that is
// what lets every branch be tested without a keychain or a clock.
bool shouldWipe(int failedAttempts, int threshold);

// True once this PROCESS has seen enough consecutive failures that no
// further guess should be served, regardless of the wipe threshold.
bool shouldRefuseForSession(int sessionFailedAttempts);

// kWipeNever, or a value inside [kMinWipeThreshold, kMaxWipeThreshold].
// Anything else -- a typo from QML, a corrupted or tampered stored value --
// is pulled into range rather than honoured, so no path can end up erasing
// on the second mistyped digit or never erasing because a stored "0" was
// really a failed parse.
int clampWipeThreshold(int threshold);

// Whether a lockout deadline is still in the future. `nowEpochMs` is passed
// in rather than read from the system clock so tests can drive it.
bool isLockedOut(qint64 lockoutUntilEpochMs, qint64 nowEpochMs);

// Whole seconds still to wait, rounded up, or 0 when not locked out. Rounded
// up so a UI countdown never displays "0 seconds remaining" while still
// refusing input.
int remainingLockoutSeconds(qint64 lockoutUntilEpochMs, qint64 nowEpochMs);

} // namespace LockoutPolicy
