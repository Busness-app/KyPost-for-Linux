#include "security/LockoutPolicy.h"

#include <algorithm>

namespace LockoutPolicy {

int clampBackgroundGraceSeconds(int seconds)
{
    if (seconds <= 0)
        return 0;
    return std::min(seconds, kMaxBackgroundGraceSeconds);
}

qint64 lockoutDurationMs(int failedAttempts)
{
    if (failedAttempts <= kFreeAttempts)
        return 0;

    // 4th failure -> 30s, 5th -> 60s, 6th -> 120s, ... capped at 15 minutes.
    // The cap matters: without it the 9th failure would be a ~32-minute wait,
    // which punishes the owner who mistyped far more than it slows an
    // attacker who is already rate-limited into irrelevance.
    static constexpr qint64 kBaseMs = 30LL * 1000;
    static constexpr qint64 kCapMs = 15LL * 60 * 1000;

    const int step = failedAttempts - kFreeAttempts - 1; // 0 for the 4th failure
    // Shift is bounded by the cap check below, but clamp the exponent too so
    // a corrupted attempt count can't shift by more than 63 (UB).
    const int boundedStep = std::min(step, 20);
    const qint64 scaled = kBaseMs << boundedStep;
    return std::min(scaled, kCapMs);
}

bool shouldWipe(int failedAttempts, int threshold)
{
    if (threshold <= kWipeNever)
        return false; // the user switched the erase off
    return failedAttempts >= threshold;
}

bool shouldRefuseForSession(int sessionFailedAttempts)
{
    return sessionFailedAttempts >= kSessionRefuseFloor;
}

int clampWipeThreshold(int threshold)
{
    if (threshold <= kWipeNever)
        return kWipeNever;
    return std::clamp(threshold, kMinWipeThreshold, kMaxWipeThreshold);
}

bool isLockedOut(qint64 lockoutUntilEpochMs, qint64 nowEpochMs)
{
    return lockoutUntilEpochMs > nowEpochMs;
}

int remainingLockoutSeconds(qint64 lockoutUntilEpochMs, qint64 nowEpochMs)
{
    if (!isLockedOut(lockoutUntilEpochMs, nowEpochMs))
        return 0;
    const qint64 remainingMs = lockoutUntilEpochMs - nowEpochMs;
    return static_cast<int>((remainingMs + 999) / 1000); // round up
}

} // namespace LockoutPolicy
