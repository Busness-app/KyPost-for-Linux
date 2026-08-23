#include "security/LockoutPolicy.h"

#include <QTest>

class LockoutPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void firstAttemptsAreFree();
    void backoffDoublesThenCaps();
    void wipesAtTenAttempts();
    void aConfiguredThresholdMovesTheWipeButNotTheSessionFloor();
    void anOutOfRangeThresholdIsPulledIntoRange();
    void lockoutWindowUsesTheSuppliedClock();
    void remainingSecondsRoundsUp();
};

void LockoutPolicyTest::firstAttemptsAreFree()
{
    // Mistyping a 6-digit PIN once or twice must not cost the owner a delay.
    for (int attempts = 0; attempts <= LockoutPolicy::kFreeAttempts; ++attempts)
        QCOMPARE(LockoutPolicy::lockoutDurationMs(attempts), 0LL);
}

void LockoutPolicyTest::backoffDoublesThenCaps()
{
    QCOMPARE(LockoutPolicy::lockoutDurationMs(4), 30LL * 1000);
    QCOMPARE(LockoutPolicy::lockoutDurationMs(5), 60LL * 1000);
    QCOMPARE(LockoutPolicy::lockoutDurationMs(6), 120LL * 1000);
    QCOMPARE(LockoutPolicy::lockoutDurationMs(7), 240LL * 1000);
    QCOMPARE(LockoutPolicy::lockoutDurationMs(8), 480LL * 1000);

    // Capped at 15 minutes rather than continuing to double: past this point
    // the extra delay punishes a forgetful owner far more than an attacker
    // who is already rate-limited into irrelevance.
    static constexpr qint64 kCapMs = 15LL * 60 * 1000;
    QCOMPARE(LockoutPolicy::lockoutDurationMs(9), kCapMs);
    QCOMPARE(LockoutPolicy::lockoutDurationMs(20), kCapMs);
    // A corrupted/absurd attempt count must not shift past 63 bits (UB) or
    // wrap to something negative.
    QCOMPARE(LockoutPolicy::lockoutDurationMs(1000), kCapMs);
    QVERIFY(LockoutPolicy::lockoutDurationMs(1000) > 0);
}

void LockoutPolicyTest::wipesAtTenAttempts()
{
    const int threshold = LockoutPolicy::kDefaultWipeThreshold;
    QVERIFY(!LockoutPolicy::shouldWipe(9, threshold));
    QVERIFY(LockoutPolicy::shouldWipe(10, threshold));
    QVERIFY(LockoutPolicy::shouldWipe(11, threshold));
}

void LockoutPolicyTest::lockoutWindowUsesTheSuppliedClock()
{
    QVERIFY(LockoutPolicy::isLockedOut(2000, 1000));
    QVERIFY(!LockoutPolicy::isLockedOut(1000, 1000)); // deadline reached, not still locked
    QVERIFY(!LockoutPolicy::isLockedOut(1000, 2000));
    QVERIFY(!LockoutPolicy::isLockedOut(0, 0));       // never locked out
}

void LockoutPolicyTest::remainingSecondsRoundsUp()
{
    QCOMPARE(LockoutPolicy::remainingLockoutSeconds(0, 1000), 0);
    QCOMPARE(LockoutPolicy::remainingLockoutSeconds(1000, 2000), 0); // already past
    QCOMPARE(LockoutPolicy::remainingLockoutSeconds(3000, 1000), 2);
    // Rounded up so a countdown never shows "0 seconds" while input is still
    // being refused.
    QCOMPARE(LockoutPolicy::remainingLockoutSeconds(1001, 1000), 1);
    QCOMPARE(LockoutPolicy::remainingLockoutSeconds(2999, 1000), 2);
}

// The erase is the user's to decline; the rate limit is not.
//
// These were one number. Making the threshold configurable without splitting
// them would have meant "never erase this device" ALSO switched off the only
// limit an attacker cannot defeat by moving the system clock forward -- the
// per-process failure counter. That trade was never on offer.
void LockoutPolicyTest::aConfiguredThresholdMovesTheWipeButNotTheSessionFloor()
{
    // Lowered.
    QVERIFY(!LockoutPolicy::shouldWipe(4, 5));
    QVERIFY(LockoutPolicy::shouldWipe(5, 5));

    // Raised.
    QVERIFY(!LockoutPolicy::shouldWipe(19, 20));
    QVERIFY(LockoutPolicy::shouldWipe(20, 20));

    // Off. No attempt count wipes.
    QVERIFY(!LockoutPolicy::shouldWipe(10, LockoutPolicy::kWipeNever));
    QVERIFY(!LockoutPolicy::shouldWipe(1000, LockoutPolicy::kWipeNever));
    // A negative threshold is "off" too, not "wipe on the first failure",
    // which is what a bare >= comparison would have made of it.
    QVERIFY(!LockoutPolicy::shouldWipe(1, -1));

    // And through all of that the session floor is unmoved.
    QVERIFY(!LockoutPolicy::shouldRefuseForSession(LockoutPolicy::kSessionRefuseFloor - 1));
    QVERIFY(LockoutPolicy::shouldRefuseForSession(LockoutPolicy::kSessionRefuseFloor));
    QVERIFY(LockoutPolicy::shouldRefuseForSession(LockoutPolicy::kSessionRefuseFloor + 1));
}

void LockoutPolicyTest::anOutOfRangeThresholdIsPulledIntoRange()
{
    // Below the floor. kFreeAttempts means the first three failures cost no
    // delay, so a threshold of 1 or 2 would erase the device before its owner
    // had been made to wait even once.
    QCOMPARE(LockoutPolicy::clampWipeThreshold(1), LockoutPolicy::kMinWipeThreshold);
    QCOMPARE(LockoutPolicy::clampWipeThreshold(4), LockoutPolicy::kMinWipeThreshold);

    // Above the ceiling.
    QCOMPARE(LockoutPolicy::clampWipeThreshold(1000), LockoutPolicy::kMaxWipeThreshold);

    // Inside, untouched.
    QCOMPARE(LockoutPolicy::clampWipeThreshold(7), 7);

    // Off stays off -- clamping must not turn a deliberate "never" into the
    // minimum, which would erase a device whose owner asked it not to.
    QCOMPARE(LockoutPolicy::clampWipeThreshold(LockoutPolicy::kWipeNever), LockoutPolicy::kWipeNever);
    QCOMPARE(LockoutPolicy::clampWipeThreshold(-5), LockoutPolicy::kWipeNever);
}

QTEST_APPLESS_MAIN(LockoutPolicyTest)
#include "LockoutPolicyTest.moc"
