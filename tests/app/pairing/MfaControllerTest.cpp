#include "pairing/MfaController.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/HttpClient.h"
#include "net/MfaResponseClient.h"
#include "net/NetworkExecutor.h"
#include "stores/SecureStoreFile.h"

#include "../../core/net/FakeRelayServer.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

// MfaController is the reference conversion to the async, off-GUI-thread
// shape (see docs/THREADING.md and core/net/NetworkExecutor.h), so these
// tests are also the reference for how a converted controller is tested:
// drive the slot, then QTRY_* on the state it publishes, rather than
// inspecting a return value that no longer exists.
class MfaControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void respondNotPairedShortCircuitsWithNoNetworkCall();
    void respondSuccessSendsStoredCredentialsAndSetsDone();
    void respondRejectedWithStatusDistinguishesAlreadyResolved();
    void respondUnauthorizedTellsTheUserToUnlockOrRepair();
    void respondFailureSetsDetailMessage();
    void resetReturnsToIdle();

    // Behaviour that only exists once the call is asynchronous.
    void respondReturnsImmediatelyWithoutBlocking();
    void aSecondRespondWhileOneIsInFlightIsIgnored();

private:
    static void savePairing(PairingStore& pairingStore, quint16 port);
};

void MfaControllerTest::savePairing(PairingStore& pairingStore, quint16 port)
{
    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    pairing.registrationUrl = pairing.serverBaseUrl + QStringLiteral("/api/notifications/native/register");
    pairing.pairingToken = QStringLiteral("pair-tok");
    pairing.deviceId = QStringLiteral("dev-1");
    pairing.deviceName = QStringLiteral("Test Device");
    pairing.deviceSecret = QStringLiteral("secret-1");
    QVERIFY(pairingStore.save(pairing));
}

void MfaControllerTest::respondNotPairedShortCircuitsWithNoNetworkCall()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"status":"approved"})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- not paired

    NetworkExecutor executor(3000);
    MfaController controller(executor, pairingStore);
    QSignalSpy stateSpy(&controller, &MfaController::respondStateChanged);

    controller.respond(QStringLiteral("chal-1"), true);

    // Still SYNCHRONOUS, deliberately: the pairing is read on the calling
    // thread before anything is dispatched, so "not paired" needs no round
    // trip and no waiting. Only the network half became asynchronous.
    QCOMPARE(controller.respondState(), QStringLiteral("failed"));
    QCOMPARE(controller.resultMessage(), QStringLiteral("Not paired"));
    QCOMPARE(stateSpy.count(), 1); // idle -> failed directly, no "sending" in between
    QVERIFY(!controller.inFlight());
    QVERIFY(fake.receivedRequest().isEmpty());
}

void MfaControllerTest::respondSuccessSendsStoredCredentialsAndSetsDone()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"status":"approved"})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    NetworkExecutor executor(3000);
    MfaController controller(executor, pairingStore);
    QSignalSpy stateSpy(&controller, &MfaController::respondStateChanged);

    controller.respond(QStringLiteral("chal-42"), true, QStringLiteral("47"));

    // Immediately, before any waiting: the call has already returned and the
    // UI already has something to show. Under the old synchronous shape
    // "sending" was unobservable -- the call did not return until the round
    // trip was over, so the state went straight to "done".
    QCOMPARE(controller.respondState(), QStringLiteral("sending"));
    QVERIFY(controller.inFlight());

    QTRY_COMPARE_WITH_TIMEOUT(controller.respondState(), QStringLiteral("done"), 5000);
    QCOMPARE(controller.resultMessage(), QStringLiteral("Approved"));
    QVERIFY(!controller.inFlight());
    QVERIFY(stateSpy.count() >= 2); // sending -> done

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("POST /api/mfa/push/respond HTTP/1.1"));
    QVERIFY(request.contains("X-Kypost-Device-Id: dev-1"));
    QVERIFY(request.contains("X-Kypost-Device-Secret: secret-1"));
    const QJsonObject sent = fake.receivedJsonBody();
    QCOMPARE(sent.value(QStringLiteral("challengeId")).toString(), QStringLiteral("chal-42"));
    QVERIFY(!sent.contains(QStringLiteral("subscriberId")));
    QVERIFY(!sent.contains(QStringLiteral("subscriberHash")));
    QVERIFY(!sent.contains(QStringLiteral("deviceId")));
    QCOMPARE(sent.value(QStringLiteral("approve")).toBool(), true);
    // Threaded through to the wire: the server refuses an approval without it.
    QCOMPARE(sent.value(QStringLiteral("matchDigits")).toString(), QStringLiteral("47"));
}

void MfaControllerTest::respondRejectedWithStatusDistinguishesAlreadyResolved()
{
    FakeRelayServer fake(httpResponse(409, "Conflict", R"({"error":"already resolved","status":"denied"})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    NetworkExecutor executor(3000);
    MfaController controller(executor, pairingStore);
    controller.respond(QStringLiteral("chal-1"), false);

    QTRY_COMPARE_WITH_TIMEOUT(controller.respondState(), QStringLiteral("failed"), 5000);
    QVERIFY(controller.resultMessage().contains(QStringLiteral("already resolved")));
    QVERIFY(controller.resultMessage().contains(QStringLiteral("denied")));
}

// A 401 must not be reported as "already handled or denied".
//
// That message was not just imprecise, it was wrong in the single most
// common case: with the credential PIN gate on and the app locked,
// PairingStore::load() returns an empty deviceSecret by design, so the
// request is guaranteed to 401. The user was told their approval had
// already been dealt with -- so they would not retry -- when in fact
// nothing had been sent and unlocking would have fixed it.
void MfaControllerTest::respondUnauthorizedTellsTheUserToUnlockOrRepair()
{
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "Unauthorized\n"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    NetworkExecutor executor(3000);
    MfaController controller(executor, pairingStore);
    controller.respond(QStringLiteral("chal-1"), true, QStringLiteral("47"));

    QTRY_COMPARE_WITH_TIMEOUT(controller.respondState(), QStringLiteral("failed"), 5000);
    // The actionable half.
    QVERIFY(controller.resultMessage().contains(QStringLiteral("Unlock")));
    // And explicitly NOT the old, false wording.
    QVERIFY(!controller.resultMessage().contains(QStringLiteral("already")));
}

void MfaControllerTest::respondFailureSetsDetailMessage()
{
    FakeRelayServer fake(httpResponse(500, "Internal Server Error", "boom"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    NetworkExecutor executor(3000);
    MfaController controller(executor, pairingStore);
    controller.respond(QStringLiteral("chal-1"), true);

    QTRY_COMPARE_WITH_TIMEOUT(controller.respondState(), QStringLiteral("failed"), 5000);
    QVERIFY(!controller.resultMessage().isEmpty());
}

void MfaControllerTest::resetReturnsToIdle()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // not paired

    NetworkExecutor executor(3000);
    MfaController controller(executor, pairingStore);
    controller.respond(QStringLiteral("chal-1"), true);
    QCOMPARE(controller.respondState(), QStringLiteral("failed"));

    controller.reset();

    QCOMPARE(controller.respondState(), QStringLiteral("idle"));
    QVERIFY(controller.resultMessage().isEmpty());
}

// The whole point of the conversion, asserted directly: respond() hands
// control straight back instead of sitting inside HttpClient's nested event
// loop for the length of a round trip.
void MfaControllerTest::respondReturnsImmediatelyWithoutBlocking()
{
    // A server that accepts the connection and never answers, so the request
    // runs until the executor's transfer timeout. The old synchronous
    // respond() would not have returned for that whole period.
    FakeRelayServer fake(QByteArray{});

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    NetworkExecutor executor(1500);
    MfaController controller(executor, pairingStore);

    QElapsedTimer timer;
    timer.start();
    controller.respond(QStringLiteral("chal-1"), true, QStringLiteral("47"));
    const qint64 elapsed = timer.elapsed();

    QVERIFY2(elapsed < 200, qPrintable(QStringLiteral("respond() blocked for %1 ms").arg(elapsed)));
    QCOMPARE(controller.respondState(), QStringLiteral("sending"));

    // And it does eventually resolve, rather than being lost.
    QTRY_COMPARE_WITH_TIMEOUT(controller.respondState(), QStringLiteral("failed"), 10000);
}

// Replaces what ReentrancyGuard did on this path, for a different reason.
// There is no nested event loop left to be re-entered through, so this is
// not a memory-safety guard any more -- it is coalescing, so two answers to
// the same challenge cannot race to set respondState.
void MfaControllerTest::aSecondRespondWhileOneIsInFlightIsIgnored()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"status":"approved"})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    NetworkExecutor executor(3000);
    MfaController controller(executor, pairingStore);

    controller.respond(QStringLiteral("chal-1"), true, QStringLiteral("47"));
    QVERIFY(controller.inFlight());
    // Second call while the first is still out: dropped, and specifically
    // does not reset the state machine back to "sending" from underneath the
    // first one's completion handler.
    controller.respond(QStringLiteral("chal-2"), false);
    QCOMPARE(controller.respondState(), QStringLiteral("sending"));

    QTRY_COMPARE_WITH_TIMEOUT(controller.respondState(), QStringLiteral("done"), 5000);
    // The FIRST challenge is what was sent.
    QCOMPARE(fake.receivedJsonBody().value(QStringLiteral("challengeId")).toString(),
             QStringLiteral("chal-1"));
    QVERIFY(!controller.inFlight());
}

QTEST_GUILESS_MAIN(MfaControllerTest)
#include "MfaControllerTest.moc"
