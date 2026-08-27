#include "platform/SecureStoreKeychain.h"

#include <QElapsedTimer>
#include <QTimer>
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
    void readsFallBackToTheLegacyServiceAndCopyForward();
    void removeClearsTheLegacyServiceToo();
    void aFailedLegacyRemovalDoesNotSilenceTheNextKey();
    void anAbsentLegacyServiceIsNotReportedAsFailure();
    void theCallingThreadIsReleasedByTheTimeoutNotByDBus();
    void theCallingThreadKeepsProcessingEventsWhileAJobRuns();

private:
    // Writes `value` under `service`, or QSKIPs if no Secret Service backend
    // will accept it. Same escape hatch as roundTripsSetGetContainsRemove(),
    // hoisted so the fallback tests below do not each re-derive it.
    void seedOrSkip(const QString& service, const QString& key, const QString& value);

    QString m_service;
    // The stand-in for the pre-rename com.urlxl.mail service. Unique per run
    // for the same reason m_service is: these tests write real entries into
    // whatever keyring is live on this machine.
    QString m_legacyService;
    // Well above the ~25s Qt D-Bus floor a wedged Secret Service imposes, so
    // a genuinely-stuck backend still terminates here, but far below QtTest's
    // 300s watchdog so a failure is reported as a failure rather than a kill.
    static constexpr int kTestTimeoutMs = 40000;
};

void SecureStoreKeychainTest::init()
{
    m_service = QStringLiteral("kypost-securestore-test-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_legacyService = QStringLiteral("kypost-securestore-legacy-test-%1")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void SecureStoreKeychainTest::cleanup()
{
    SecureStoreKeychain store(m_service, kTestTimeoutMs);
    store.remove(QStringLiteral("sub"));
    // No legacy service configured on either store here: cleanup must reach
    // each service explicitly rather than relying on the very fallback these
    // tests exercise, or a broken fallback would also leak the entries it
    // failed to migrate.
    SecureStoreKeychain legacy(m_legacyService, kTestTimeoutMs);
    legacy.remove(QStringLiteral("sub"));
}

void SecureStoreKeychainTest::seedOrSkip(const QString& service, const QString& key,
                                         const QString& value)
{
    SecureStoreKeychain seed(service, kTestTimeoutMs);
    if (!seed.set(key, value)) {
        const SecureStore::ReadResult probe = seed.read(key);
        QVERIFY2(probe.status != SecureStore::ReadStatus::Found,
                 "the write reported failure but the value is readable -- that is a real bug, not "
                 "an absent backend");
        QSKIP("No usable Secret Service backend reachable (write failed)");
    }
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

// The app-id rename's whole point: a device paired under com.urlxl.mail must
// not look unpaired under com.kysecurity.mail. Reads find the old value, and
// copy it forward so the fallback stops firing.
void SecureStoreKeychainTest::readsFallBackToTheLegacyServiceAndCopyForward()
{
    seedOrSkip(m_legacyService, QStringLiteral("sub"), QStringLiteral("legacy-subscriber"));

    SecureStoreKeychain store(m_service, kTestTimeoutMs, m_legacyService);

    const SecureStore::ReadResult result = store.read(QStringLiteral("sub"));
    QCOMPARE(result.status, SecureStore::ReadStatus::Found);
    QCOMPARE(result.value, QStringLiteral("legacy-subscriber"));

    // Copied forward, not merely proxied: a store with NO legacy fallback
    // configured must now find it under the new service on its own.
    SecureStoreKeychain migrated(m_service, kTestTimeoutMs);
    QCOMPARE(migrated.get(QStringLiteral("sub")).value(), QStringLiteral("legacy-subscriber"));

    // Copied, not moved -- docs/RENAME_NOTES.md's rule that a bad migration
    // stays recoverable by hand.
    SecureStoreKeychain legacy(m_legacyService, kTestTimeoutMs);
    QCOMPARE(legacy.get(QStringLiteral("sub")).value(), QStringLiteral("legacy-subscriber"));
}

// A wipe must not be undone by the next launch reading the credential back
// out of the pre-rename service.
void SecureStoreKeychainTest::removeClearsTheLegacyServiceToo()
{
    seedOrSkip(m_legacyService, QStringLiteral("sub"), QStringLiteral("legacy-subscriber"));

    SecureStoreKeychain store(m_service, kTestTimeoutMs, m_legacyService);
    QVERIFY(store.remove(QStringLiteral("sub")));

    SecureStoreKeychain legacy(m_legacyService, kTestTimeoutMs);
    QVERIFY2(!legacy.contains(QStringLiteral("sub")),
             "remove() left the credential in the pre-rename service, where the next read would "
             "resurrect it");
    QVERIFY(!store.contains(QStringLiteral("sub")));
}

// The ten-failed-PIN wipe is eleven remove() calls over ONE store, and it used
// to skip the legacy service for keys 2..N as soon as key 1's legacy delete
// failed -- reporting a clean wipe while com.urlxl.mail still held the pairing
// secret and the PIN record, which the next read() would copy forward.
//
// timeoutMs=0 forces every job to fail, the same deterministic lever
// aTimedOutReadIsFailedNeverAbsent() uses. The assertion is the per-key
// warning: ignoreMessage() fails the test if the message never arrives, so the
// second key going unattempted is a failure rather than a silent pass.
void SecureStoreKeychainTest::aFailedLegacyRemovalDoesNotSilenceTheNextKey()
{
    SecureStoreKeychain store(m_service, /*timeoutMs=*/0, m_legacyService);

    for (const auto& key : { QStringLiteral("pairing.deviceSecret"),
                             QStringLiteral("applock.pinRecord") }) {
        QTest::ignoreMessage(QtWarningMsg,
                             qPrintable(QStringLiteral("SecureStoreKeychain: could not clear '%1' "
                                                       "from the pre-rename service; a copy may "
                                                       "remain in the keyring")
                                            .arg(key)));
        QVERIFY2(!store.remove(key),
                 "a removal that could not reach the legacy service must report false");
    }
}

// The overwhelmingly common case: a fresh install with no pre-rename profile
// at all. The legacy service holds nothing, which is an answer -- Absent --
// and must not be dressed up as a failure that would fail the app closed.
void SecureStoreKeychainTest::anAbsentLegacyServiceIsNotReportedAsFailure()
{
    // Establishes that a backend is reachable, so that an Absent below is the
    // backend answering rather than the no-keyring path.
    seedOrSkip(m_service, QStringLiteral("sub"), QStringLiteral("present"));

    SecureStoreKeychain store(m_service, kTestTimeoutMs, m_legacyService);

    const SecureStore::ReadResult result = store.read(QStringLiteral("never-written"));
    QCOMPARE(result.status, SecureStore::ReadStatus::Absent);
}

// THE FIX OF 2026-08-24, and the only test here that can tell whether it is
// still in place.
//
// The jobs run on their own thread now. Before that they ran on the calling
// thread, inside a synchronous D-Bus call that did not process events -- so
// `timeoutMs` could not be delivered until that call returned on its own at
// Qt's ~25 s D-Bus default, and the caller was pinned for the whole time. For
// every startup caller that thread is the GUI thread, which is why a broken
// keyring made KyPost look dead rather than slow.
//
// So: a one-second timeout must release the CALLER in about one second, even
// when the backend behind it is going to sit there for twenty-five. This test
// is meaningful only on a machine whose Secret Service does not answer
// promptly -- exactly the machine the bug was about -- and is trivially true
// on one that does, which is why it asserts a ceiling rather than a window.
void SecureStoreKeychainTest::theCallingThreadIsReleasedByTheTimeoutNotByDBus()
{
    SecureStoreKeychain store(m_service, /*timeoutMs=*/1000);

    QElapsedTimer elapsed;
    elapsed.start();
    const SecureStore::ReadResult result = store.read(QStringLiteral("never-written"));
    const qint64 blockedFor = elapsed.elapsed();

    // Absent (a fast, working keyring) or Failed (the timeout fired). Both are
    // correct answers; what is not correct is taking 25 s to give either.
    QVERIFY(result.status == SecureStore::ReadStatus::Absent
            || result.status == SecureStore::ReadStatus::Failed);
    QVERIFY2(blockedFor < 10000,
             qPrintable(QStringLiteral("read() pinned the calling thread for %1 ms against a 1000 ms "
                                        "timeout -- the QKeychain job is back on the caller's thread")
                            .arg(blockedFor)));
}

// The other half of the same fix, and the half main() depends on.
//
// Releasing the caller on time is not enough on its own: KyPost puts a startup
// window up before it opens the secret store, and that window can only paint
// if the GUI thread is still delivering its own events while the store is
// being consulted. Before the jobs moved off this thread, it was not -- it sat
// inside a synchronous D-Bus call, so no timer fired, nothing repainted, and
// the application looked dead rather than busy.
//
// Asserted conditionally, because it can only be observed on a machine whose
// Secret Service is slow. Where the store answers immediately there is no
// window during which anything could have been starved, and the test says so
// rather than pretending to have checked.
void SecureStoreKeychainTest::theCallingThreadKeepsProcessingEventsWhileAJobRuns()
{
    SecureStoreKeychain store(m_service, /*timeoutMs=*/2000);

    int ticks = 0;
    QTimer heartbeat;
    heartbeat.setInterval(50);
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&ticks]() { ++ticks; });
    heartbeat.start();

    QElapsedTimer elapsed;
    elapsed.start();
    store.read(QStringLiteral("never-written"));
    const qint64 blockedFor = elapsed.elapsed();
    heartbeat.stop();

    if (blockedFor < 200)
        QSKIP("this Secret Service answers immediately -- there is no starvation window to observe");

    QVERIFY2(ticks > 0,
             qPrintable(QStringLiteral("the calling thread processed no events during a %1 ms "
                                        "keychain call -- a startup window could not paint")
                            .arg(blockedFor)));
}

QTEST_GUILESS_MAIN(SecureStoreKeychainTest)
#include "SecureStoreKeychainTest.moc"
