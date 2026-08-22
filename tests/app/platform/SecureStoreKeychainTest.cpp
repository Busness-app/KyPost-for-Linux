#include "platform/SecureStoreKeychain.h"

#include <QElapsedTimer>
#include <QTest>
#include <QUuid>

// Exercises SecureStoreKeychain against whatever Secret Service provider is
// reachable on this machine (gnome-keyring / ksecretd / kwallet all
// implement org.freedesktop.secrets). The service name is unique per test
// run so a stray failure can't collide with a real credential, and the test
// removes its own entry in cleanup() regardless of outcome.
//
// This file used to hang. Not "run slowly" -- hang, indefinitely, against a
// live gnome-keyring with a locked collection, because SecureStoreKeychain
// waited on QKeychain::Job::finished and QKeychain never emits it when its
// underlying D-Bus call gives up. The 300s the run appeared to take was
// QtTest's watchdog. Worse, the QSKIP path below had its own hand-rolled
// unbounded QEventLoop, so the escape hatch hung too. It now goes through the
// store's own bounded read().
class SecureStoreKeychainTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void roundTripsSetGetContainsRemove();
    void aTimedOutReadIsFailedNeverAbsent();
    void everyCallReturnsEvenWhenTheStoreCannotBeConsulted();

private:
    QString m_service;
    // Well above the ~25s Qt D-Bus floor a wedged Secret Service imposes, so
    // a genuinely-stuck backend still terminates here, but far below QtTest's
    // 300s watchdog so a failure is reported as a failure rather than a kill.
    static constexpr int kTestTimeoutMs = 40000;
};

void SecureStoreKeychainTest::init()
{
    m_service = QStringLiteral("kypost-securestore-test-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void SecureStoreKeychainTest::cleanup()
{
    SecureStoreKeychain store(m_service, kTestTimeoutMs);
    store.remove(QStringLiteral("sub"));
}

void SecureStoreKeychainTest::roundTripsSetGetContainsRemove()
{
    SecureStoreKeychain store(m_service, kTestTimeoutMs);

    if (!store.set(QStringLiteral("sub"), QStringLiteral("subscriber-123"))) {
        // Bounded, unlike the raw QKeychain probe this replaced: read() is
        // the same code path under test and it cannot hang.
        const SecureStore::ReadResult probe = store.read(QStringLiteral("sub"));
        QVERIFY2(probe.status != SecureStore::ReadStatus::Found,
                 "the write reported failure but the value is readable -- that is a real bug, not an "
                 "absent backend");
        QSKIP("No usable Secret Service backend reachable (write failed)");
    }

    QVERIFY(store.contains(QStringLiteral("sub")));
    QCOMPARE(store.get(QStringLiteral("sub")).value(), QStringLiteral("subscriber-123"));

    QVERIFY(store.remove(QStringLiteral("sub")));
    QVERIFY(!store.contains(QStringLiteral("sub")));
    QVERIFY(!store.get(QStringLiteral("sub")).has_value());
}

// The security-critical mapping. AppLockStore::lockEnabled() fails CLOSED on
// Failed and treats Absent as "no PIN was ever configured" -- so reporting a
// store that could not be consulted as Absent unlocks the app for whoever is
// holding it. A timeout is the strongest possible evidence of "could not be
// consulted", and it must never come back as absence.
//
// timeoutMs=0 forces the timeout path deterministically: verified 20/20 runs
// against a live keyring, where the same probe at 1ms was racy (15 Absent,
// 5 Failed) and at 3000ms almost never fired.
void SecureStoreKeychainTest::aTimedOutReadIsFailedNeverAbsent()
{
    SecureStoreKeychain store(m_service, /*timeoutMs=*/0);

    const SecureStore::ReadResult result = store.read(QStringLiteral("never-written"));

    QVERIFY2(result.status != SecureStore::ReadStatus::Absent,
             "a read that timed out must not be reported as 'there is no such secret'");
    QCOMPARE(result.status, SecureStore::ReadStatus::Failed);
    QVERIFY(!store.get(QStringLiteral("never-written")).has_value());
}

// The regression itself: termination. Every call must come back, whatever the
// backend is doing. On a machine with no Secret Service this is instant; on
// one with a locked collection it costs the ~25s D-Bus floor and then
// returns. What it must never do is what it used to do, which is never
// return at all.
void SecureStoreKeychainTest::everyCallReturnsEvenWhenTheStoreCannotBeConsulted()
{
    SecureStoreKeychain store(m_service, /*timeoutMs=*/0);

    QElapsedTimer elapsed;
    elapsed.start();

    store.set(QStringLiteral("k"), QStringLiteral("v"));
    store.read(QStringLiteral("k"));
    store.get(QStringLiteral("k"));
    store.contains(QStringLiteral("k"));
    store.remove(QStringLiteral("k"));

    // Five calls. Generous enough to absorb the D-Bus floor on each without
    // being flaky, tight enough to fail long before QtTest's watchdog turns
    // "hung forever" into an unexplained kill.
    QVERIFY2(elapsed.elapsed() < 200000,
             "SecureStoreKeychain calls must terminate; this is the hang this test exists for");
}

QTEST_GUILESS_MAIN(SecureStoreKeychainTest)
#include "SecureStoreKeychainTest.moc"
