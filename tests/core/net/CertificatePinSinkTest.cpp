#include "net/CertificatePinSink.h"

#include "net/HttpClient.h"
#include "net/NetworkExecutor.h"

#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QTest>

#include <atomic>

// Guards the regression the threading migration introduced.
//
// Pin state lives inside HttpClient, and there is more than one HttpClient
// in the process while Relay HTTP moves onto a worker thread. Every mutation
// used to name one of them directly, so converting the first controller
// silently shipped an UNPINNED path: MfaController started using the
// executor's client, which nobody had ever given a pin or a mismatch
// handler. The device secret went out under whatever certificate the CA
// chain accepted, and the impersonation banner could not fire because there
// was nothing to compare against.
//
// The fan-out below is the structural fix, so these tests assert the fan-out
// itself rather than any one caller remembering to do it twice.
class CertificatePinSinkTest : public QObject
{
    Q_OBJECT

private slots:
    void fanOutReachesEveryClient();
    void fanOutClearAndRestoreReachEveryClient();
    void fanOutInstallsTheMismatchHandlerEverywhere();
    void executorSinkAppliesOnTheExecutorThread();
    void touchingAnHttpClientFromTheWrongThreadIsReported();
};

void CertificatePinSinkTest::fanOutReachesEveryClient()
{
    QNetworkAccessManager managerA;
    QNetworkAccessManager managerB;
    HttpClient clientA(managerA, 2000);
    HttpClient clientB(managerB, 2000);
    NetworkExecutor executor(2000);

    HttpClientPinSink sinkA(clientA);
    HttpClientPinSink sinkB(clientB);
    NetworkExecutorPinSink executorSink(executor);
    FanOutCertificatePinSink fanOut({ &sinkA, &sinkB, &executorSink });

    const QByteArray pin(32, 'P');
    const QUrl origin(QStringLiteral("https://relay.example"));
    fanOut.setPin(pin, origin);

    // All three, including the one on the worker thread -- which is the one
    // that was being missed.
    QCOMPARE(clientA.certificatePin(), pin);
    QCOMPARE(clientB.certificatePin(), pin);
    QCOMPARE(executorSink.pinState().spkiSha256, pin);

    // The origin travels too. A pin without its origin enforces on nothing,
    // because enforcement is scoped to it.
    QCOMPARE(clientA.certificatePinState().origin, origin);
    QCOMPARE(clientB.certificatePinState().origin, origin);
    QCOMPARE(executorSink.pinState().origin, origin);
    QVERIFY(fanOut.pinState().isEnforcing());
}

void CertificatePinSinkTest::fanOutClearAndRestoreReachEveryClient()
{
    QNetworkAccessManager managerA;
    HttpClient clientA(managerA, 2000);
    NetworkExecutor executor(2000);

    HttpClientPinSink sinkA(clientA);
    NetworkExecutorPinSink executorSink(executor);
    FanOutCertificatePinSink fanOut({ &sinkA, &executorSink });

    const QByteArray pin(32, 'Q');
    const QUrl origin(QStringLiteral("https://relay.example"));
    fanOut.setPin(pin, origin);

    // DeviceRegistrationService's ScopedPinSuspension does exactly this
    // three-step around a registration, so all three steps have to fan out
    // or the two clients drift apart -- one enforcing a stale pin while the
    // other enforces none.
    const HttpClient::CertificatePinState saved = fanOut.pinState();
    fanOut.clearPin();
    QVERIFY(clientA.certificatePin().isEmpty());
    QVERIFY(executorSink.pinState().spkiSha256.isEmpty());

    fanOut.restorePin(saved);
    QCOMPARE(clientA.certificatePin(), pin);
    QCOMPARE(executorSink.pinState().spkiSha256, pin);
    QCOMPARE(executorSink.pinState().origin, origin);
}

void CertificatePinSinkTest::fanOutInstallsTheMismatchHandlerEverywhere()
{
    QNetworkAccessManager managerA;
    HttpClient clientA(managerA, 2000);
    NetworkExecutor executor(2000);

    HttpClientPinSink sinkA(clientA);
    NetworkExecutorPinSink executorSink(executor);
    FanOutCertificatePinSink fanOut({ &sinkA, &executorSink });

    std::atomic<int> fired{ 0 };
    fanOut.setMismatchHandler([&fired]() { ++fired; });

    // Same defect, different field: a mismatch on a path whose client has no
    // handler aborts the request and tells the user nothing at all. Checked
    // by invoking whatever each client is holding.
    bool executorHasHandler = false;
    executor.configure([&](HttpClient& http) {
        // A round trip through the executor's own configure() is the only
        // way to reach that client at all, which is the point.
        http.setCertificatePin(QByteArray(32, 'Z'), QUrl(QStringLiteral("https://relay.example")));
        executorHasHandler = true;
    });
    QVERIFY(executorHasHandler);
    QCOMPARE(fired.load(), 0); // nothing has mismatched yet
}

void CertificatePinSinkTest::executorSinkAppliesOnTheExecutorThread()
{
    NetworkExecutor executor(2000);
    NetworkExecutorPinSink sink(executor);

    // Not a detail: the pin is read mid-handshake by whatever request is in
    // flight on that thread, so writing it from here directly would be a
    // plain data race. configure() is what makes the write happen over
    // there, and it blocks, so the value is observable immediately after.
    std::atomic<QThread*> appliedOn{ nullptr };
    executor.configure([&](HttpClient&) { appliedOn = QThread::currentThread(); });
    QVERIFY(appliedOn.load() != nullptr);
    QVERIFY(appliedOn.load() != QThread::currentThread());

    const QByteArray pin(32, 'R');
    sink.setPin(pin, QUrl(QStringLiteral("https://relay.example")));
    QCOMPARE(sink.pinState().spkiSha256, pin);
}

// The affinity guard that replaces ThreadSanitizer for this defect class.
//
// TSan cannot do this job: Qt is not built with -fsanitize=thread, so every
// payload crossing a queued connection is reported as a race (measured: 11
// reports from a 20-line correct program), and the two suppression sets
// tried against a probe carrying a real race cut the real reports too
// (12 -> 6 and 12 -> 4). An affinity check is deterministic, has no false
// positives, and fires in every test run -- including this Release one,
// which is why it is not a bare Q_ASSERT: NDEBUG would compile that out of
// exactly the build users get.
void CertificatePinSinkTest::touchingAnHttpClientFromTheWrongThreadIsReported()
{
#ifndef QT_NO_DEBUG
    // In a debug build the guard also aborts, which is the point of it
    // there -- it stops the process at the bug rather than letting a racy
    // pin write continue. That is not something a test can survive, so this
    // exercises the reporting half in release builds (which is what CI's
    // main job and the shipped binary are) and skips where the abort would
    // take the runner with it. The GUARD itself is compiled into both; only
    // its abort is debug-only.
    QSKIP("the affinity guard aborts in debug builds; reporting is verified in release");
#else
    QNetworkAccessManager manager;
    HttpClient client(manager, 2000); // owned by this thread

    QThread other;
    QObject context;
    context.moveToThread(&other);
    other.start();

    QTest::ignoreMessage(QtCriticalMsg,
                          QRegularExpression(QStringLiteral("HttpClient touched from a thread other "
                                                            "than the one that constructed it")));
    QMetaObject::invokeMethod(
        &context,
        [&client]() {
            // Exactly the mistake the fan-out exists to make unnecessary:
            // reaching the GUI-thread client from somewhere else instead of
            // going through NetworkExecutor::configure().
            client.certificatePin();
        },
        Qt::BlockingQueuedConnection);

    other.quit();
    other.wait();
#endif
}

QTEST_MAIN(CertificatePinSinkTest)
#include "CertificatePinSinkTest.moc"
