#include "net/NetworkExecutor.h"

#include "net/HttpClient.h"

#include "FakeRelayServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>

#include <atomic>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>

// The point of NetworkExecutor is not speed, it is that the GUI thread is
// never inside HttpClient's nested QEventLoop. These tests assert that
// directly -- that the work runs somewhere else, that the caller is not
// blocked, and above all that nothing is dispatched onto the calling thread
// while a request is in flight.
class NetworkExecutorTest : public QObject
{
    Q_OBJECT

private slots:
    void workRunsOffTheCallingThread();
    void resultIsDeliveredOnTheReceiversThread();
    void runReturnsBeforeTheRequestCompletes();
    void theCallingThreadIsNotReenteredWhileARequestIsInFlight();
    void shutdownBeforeReceiverDestructionIsWhatMakesItSafe();
    void configureIsAppliedOnTheExecutorThread();
    void shutdownWaitsForInFlightWork();
    void shutdownDiscardsWorkThatHasNotStarted();
    void workSubmittedAfterShutdownIsDroppedNotRunInline();
    void realHttpCallRoundTrips();
};

void NetworkExecutorTest::workRunsOffTheCallingThread()
{
    NetworkExecutor executor(2000);
    // Atomics, not plain locals: these are written on the executor thread
    // and read here. A bare bool would be a data race in the test itself,
    // which ThreadSanitizer duly reported.
    std::atomic<QThread*> workThread{ nullptr };
    std::atomic<bool> done{ false };

    executor.run(
        this, [&workThread](HttpClient&) { workThread = QThread::currentThread(); return 1; },
        [&done](int) { done = true; });

    QTRY_VERIFY(done.load());
    QVERIFY(workThread.load() != nullptr);
    QVERIFY(workThread.load() != QThread::currentThread());
}

void NetworkExecutorTest::resultIsDeliveredOnTheReceiversThread()
{
    NetworkExecutor executor(2000);
    QThread* handlerThread = nullptr;
    int received = 0;

    executor.run(
        this, [](HttpClient&) { return 42; },
        [&](int value) {
            handlerThread = QThread::currentThread();
            received = value;
        });

    QTRY_COMPARE(received, 42);
    // Handlers touch models and emit QML-bound signals, so they must arrive
    // where those live.
    QCOMPARE(handlerThread, QThread::currentThread());
}

void NetworkExecutorTest::runReturnsBeforeTheRequestCompletes()
{
    NetworkExecutor executor(2000);
    std::atomic<bool> done{ false };

    QElapsedTimer timer;
    timer.start();
    executor.run(
        this,
        [](HttpClient&) {
            QThread::msleep(300);
            return 1;
        },
        [&done](int) { done = true; });
    const qint64 elapsedInCall = timer.elapsed();

    // The whole contract: control comes straight back. The old shape
    // returned only once the network round trip had finished.
    QVERIFY2(elapsedInCall < 100, qPrintable(QStringLiteral("run() blocked for %1 ms").arg(elapsedInCall)));
    QVERIFY(!done.load());
    QTRY_VERIFY_WITH_TIMEOUT(done.load(), 5000);
}

// The regression this class exists for.
//
// HttpClient::waitForReply drives a nested QEventLoop. Run that on the GUI
// thread and Qt keeps dispatching queued calls, timers and input INTO the
// half-finished caller -- which is how a window minimise could reach
// AppLock.lockNow() from inside PairingStore::save(), and how a second
// composer's Send could start an inner sendMail. Here the calling thread
// must stay outside any event dispatch for the whole request.
void NetworkExecutorTest::theCallingThreadIsNotReenteredWhileARequestIsInFlight()
{
    NetworkExecutor executor(2000);

    std::atomic<bool> reenteredDuringRequest{ false };
    std::atomic<bool> requestFinished{ false };

    // A timer that would fire immediately if -- and only if -- this thread
    // entered an event loop while the request was running. Under the old
    // synchronous shape this fired inside the call, every time.
    QTimer probe;
    probe.setInterval(0);
    probe.setSingleShot(false);
    connect(&probe, &QTimer::timeout, this, [&]() {
        if (!requestFinished.load())
            reenteredDuringRequest = true;
    });
    probe.start();

    executor.run(
        this,
        [](HttpClient&) {
            QThread::msleep(200);
            return 1;
        },
        [&](int) { requestFinished = true; });

    // Deliberately a plain sleep, not QTest::qWait: qWait spins an event
    // loop, which is exactly the thing being measured. This blocks the way
    // ordinary straight-line calling code does.
    QThread::msleep(120);
    QVERIFY(!reenteredDuringRequest.load());
    QVERIFY(!requestFinished.load());

    // Now let the event loop run normally and the result arrives.
    QTRY_VERIFY_WITH_TIMEOUT(requestFinished.load(), 5000);
    probe.stop();
}

// The receiver-lifetime contract, stated as a test.
//
// run() deliberately does NOT try to detect a receiver destroyed
// mid-request. It used to, via a QPointer checked on the executor thread --
// which is not thread-safe, races with ~QObject, and can report "alive" for
// an object already being torn down, after which delivery writes to freed
// memory. ThreadSanitizer flags exactly that. A guard that is wrong under
// the one condition it exists for is worse than a stated precondition.
//
// So the rule is: shutdown() first, THEN destroy receivers. shutdown() is
// what makes that safe, and this pins it.
void NetworkExecutorTest::shutdownBeforeReceiverDestructionIsWhatMakesItSafe()
{
    auto executor = std::make_unique<NetworkExecutor>(2000);
    auto receiver = std::make_unique<QObject>();
    std::atomic<bool> handlerRan{ false };
    QSemaphore started;

    executor->run(
        receiver.get(),
        [&started](HttpClient&) {
            started.release();
            QThread::msleep(150);
            return 1;
        },
        [&handlerRan](int) { handlerRan = true; });

    QVERIFY(started.tryAcquire(1, 5000));

    // The required order. After this returns, the in-flight task has
    // finished and nothing further can be delivered...
    executor->shutdown();

    // ...so destroying the receiver now cannot be raced by a callback.
    receiver.reset();
    QTest::qWait(200);
    QVERIFY(!handlerRan.load());
}

void NetworkExecutorTest::configureIsAppliedOnTheExecutorThread()
{
    NetworkExecutor executor(2000);

    std::atomic<QThread*> configureThread{ nullptr };
    const QByteArray pin(32, 'P');
    executor.configure([&](HttpClient& http) {
        configureThread = QThread::currentThread();
        http.setCertificatePin(pin, QUrl(QStringLiteral("https://relay.example")));
    });

    // Blocking, so it has already been applied by the time configure()
    // returns -- and applied where the HttpClient lives, not from here. The
    // certificate pin is read mid-handshake by a request on that thread;
    // writing it from the GUI thread would be a plain data race.
    QVERIFY(configureThread.load() != nullptr);
    QVERIFY(configureThread.load() != QThread::currentThread());

    QByteArray observed;
    std::atomic<bool> done{ false };
    executor.run(
        this, [](HttpClient& http) { return http.certificatePin(); },
        [&](QByteArray value) {
            observed = value;
            done = true;
        });
    QTRY_VERIFY(done.load());
    QCOMPARE(observed, pin);
}

void NetworkExecutorTest::shutdownWaitsForInFlightWork()
{
    NetworkExecutor executor(2000);
    QSemaphore started;
    std::atomic<bool> workFinished{ false };

    executor.run(
        this,
        [&](HttpClient&) {
            started.release();
            QThread::msleep(200);
            workFinished = true;
            return 1;
        },
        [](int) {});

    // Wait until the task is genuinely EXECUTING before shutting down.
    // Without this the task is merely queued, and a queued task is
    // discarded rather than run -- see shutdownDiscardsWorkThatHasNotStarted
    // below. Asserting "workFinished" without this wait was testing a
    // guarantee shutdown() does not make, and it duly failed.
    QVERIFY(started.tryAcquire(1, 5000));

    executor.shutdown();
    // A barrier, not a request to stop: shutdown() must not return while a
    // task is still touching state its receiver is about to destroy.
    QVERIFY(workFinished.load());

    // Idempotent.
    executor.shutdown();
}

// The other half of the contract, stated so nobody later "fixes" it into a
// drain. At teardown, running more network work against stores and
// controllers that are already being destroyed is worse than dropping a
// request whose answer nobody will see.
void NetworkExecutorTest::shutdownDiscardsWorkThatHasNotStarted()
{
    NetworkExecutor executor(2000);
    QSemaphore firstStarted;
    std::atomic<bool> secondRan{ false };

    executor.run(
        this,
        [&](HttpClient&) {
            firstStarted.release();
            QThread::msleep(200);
            return 1;
        },
        [](int) {});
    // Queued behind the first, so it cannot have started yet.
    executor.run(
        this, [&](HttpClient&) { secondRan = true; return 2; }, [](int) {});

    QVERIFY(firstStarted.tryAcquire(1, 5000));
    executor.shutdown();

    QVERIFY(!secondRan.load());
}

// Work submitted after shutdown is dropped, and specifically is NOT run on
// the caller's thread as a fallback -- that would silently reintroduce the
// nested-loop re-entrancy this class exists to remove.
void NetworkExecutorTest::workSubmittedAfterShutdownIsDroppedNotRunInline()
{
    NetworkExecutor executor(2000);
    executor.shutdown();

    std::atomic<bool> ran{ false };
    std::atomic<bool> handled{ false };
    QTest::ignoreMessage(QtWarningMsg, "NetworkExecutor: dropping work submitted after shutdown");
    executor.run(
        this, [&](HttpClient&) { ran = true; return 1; }, [&](int) { handled = true; });

    QTest::qWait(200);
    QVERIFY(!ran.load());
    QVERIFY(!handled.load());
}

void NetworkExecutorTest::realHttpCallRoundTrips()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));

    NetworkExecutor executor(2000);
    HttpClient::HttpResult result;
    std::atomic<bool> done{ false };

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/health").arg(fake.port()));
    executor.run(
        this, [url](HttpClient& http) { return http.get(url, {}, {}); },
        [&](HttpClient::HttpResult r) {
            result = std::move(r);
            done = true;
        });

    QTRY_VERIFY_WITH_TIMEOUT(done.load(), 5000);
    QVERIFY(!result.error.has_value());
    QCOMPARE(result.statusCode, 200);
    QCOMPARE(result.body, QByteArray(R"({"ok":true})"));
}

QTEST_MAIN(NetworkExecutorTest)
#include "NetworkExecutorTest.moc"
