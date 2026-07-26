#include "security/LockoutPolicy.h"

#include <QTest>

class LockoutPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void firstAttemptsAreFree();
    void backoffDoublesThenCaps();
    void wipesAtTenAttempts();
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
    QVERIFY(!LockoutPolicy::shouldWipe(9));
    QVERIFY(LockoutPolicy::shouldWipe(10));
    QVERIFY(LockoutPolicy::shouldWipe(11));
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

QTEST_APPLESS_MAIN(LockoutPolicyTest)
#include "LockoutPolicyTest.moc"
