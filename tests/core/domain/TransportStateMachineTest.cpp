#include "domain/TransportStateMachine.h"

#include "db/Database.h"
#include "db/PushDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "domain/PushRepository.h"
#include "models/PushNotification.h"
#include "net/HttpClient.h"
#include "net/PushNotificationClient.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include "../net/FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Owns every real dependency PushRepository needs (matches the wiring
// pattern in PushRepositoryTest.cpp) so TransportStateMachine's polling
// tier can be exercised against a real PushRepository rather than a stub.
// pair(), when called, points the stored pairing at `fake` so pullOnce()
// actually reaches it; tests that never expect a network call (the
// distributor tier) simply leave the repository unpaired, since
// PushRepository::pullOnce() returns an empty vector without making a
// request when there is no stored pairing.
struct RepoHarness
{
    Database db;
    QTemporaryDir cursorDir;
    QTemporaryDir secureDir;
    QTemporaryDir settingsDir;
    CursorStore cursorStore;
    SecureStoreFile secureStore;
    PairingStore pairingStore;
    SettingsStore settingsStore;
    QNetworkAccessManager manager;
    HttpClient http;
    PushNotificationClient client;
    PushDao pushDao;
    PushRepository repository;

    RepoHarness()
        : cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")))
        , secureStore(secureDir.path())
        , pairingStore(secureStore)
        , settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")))
        , http(manager)
        , client(http)
        , pushDao(db.handle())
        , repository(pushDao, cursorStore, client, pairingStore, settingsStore)
    {
        db.open(QStringLiteral(":memory:"));
    }

    void pair(quint16 fakeRelayPort)
    {
        DevicePairing pairing;
        pairing.subscriberId = QStringLiteral("sub-1");
        pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fakeRelayPort);
        pairing.registrationUrl = pairing.serverBaseUrl + QStringLiteral("/api/notifications/native/register");
        pairing.pairingToken = QStringLiteral("pair-tok");
        pairing.deviceId = QStringLiteral("device-1");
        pairing.deviceName = QStringLiteral("My Linux Desktop");
        pairing.deviceSecret = QStringLiteral("secret-1");
        pairingStore.save(pairing);
    }
};

} // namespace

// The embedded ntfy subscriber tier this suite used to cover (a FakeNtfyServer
// harness plus six tests spanning foreground/background transitions, the
// connection-loss failure budget and the promote-back retry timer) went away
// with that tier on 2026-07-26 -- see core/domain/TransportStateMachine.h.
// What remains is the two-tier decision and the polling fetch path.
class TransportStateMachineTest : public QObject
{
    Q_OBJECT

private slots:
    void startsInPolling();
    void distributorAvailableEntersDistributorAndStopsPolling();
    void distributorUnavailableReturnsToPolling();
    void enterTierIdempotencyEmitsTierChangedOnce();
    void pollTimerFetchesAndEmitsPollTick();
};

void TransportStateMachineTest::startsInPolling()
{
    RepoHarness repo; // left unpaired -- pullOnce() is a no-op either way

    TransportStateMachine machine(repo.repository);

    QCOMPARE(machine.currentTier(), TransportTier::Polling);
}

void TransportStateMachineTest::distributorAvailableEntersDistributorAndStopsPolling()
{
    RepoHarness repo; // unpaired: pullOnce() is a no-op, so any stray poll tick would
                       // still carry an empty result rather than crash -- pollSpy's
                       // count is what actually proves the timer stopped

    // Short interval: proves the poll timer is genuinely stopped (not just
    // slow) by giving it many chances to fire during the wait window below.
    TransportStateMachine machine(repo.repository, nullptr, /*pollIntervalMs=*/20);
    QSignalSpy tierSpy(&machine, &TransportStateMachine::tierChanged);
    QSignalSpy pollSpy(&machine, &TransportStateMachine::pollTick);

    machine.setDistributorAvailable(true);

    QCOMPARE(machine.currentTier(), TransportTier::Distributor);
    QCOMPARE(tierSpy.size(), 1);
    QCOMPARE(tierSpy.at(0).at(0).value<TransportTier>(), TransportTier::Distributor);

    // ~15 missed 20ms intervals if the timer were still running.
    QTest::qWait(300);
    QCOMPARE(pollSpy.size(), 0);
}

void TransportStateMachineTest::distributorUnavailableReturnsToPolling()
{
    RepoHarness repo;

    TransportStateMachine machine(repo.repository, nullptr, /*pollIntervalMs=*/20);
    machine.setDistributorAvailable(true);
    QCOMPARE(machine.currentTier(), TransportTier::Distributor);

    QSignalSpy pollSpy(&machine, &TransportStateMachine::pollTick);
    machine.setDistributorAvailable(false);

    QCOMPARE(machine.currentTier(), TransportTier::Polling);
    // The poll timer is restarted by the transition, not merely left stopped.
    QVERIFY(pollSpy.wait());
}

void TransportStateMachineTest::enterTierIdempotencyEmitsTierChangedOnce()
{
    RepoHarness repo;

    TransportStateMachine machine(repo.repository, nullptr, /*pollIntervalMs=*/20);
    QSignalSpy tierSpy(&machine, &TransportStateMachine::tierChanged);

    machine.setDistributorAvailable(true);
    machine.setDistributorAvailable(true);
    machine.setDistributorAvailable(true);

    QCOMPARE(machine.currentTier(), TransportTier::Distributor);
    QCOMPARE(tierSpy.size(), 1);
}

// Proves the polling tier's fetch path is actually wired end to end: a real
// PushRepository pulling from a real FakeRelayServer, invoked by the poll
// timer firing (not by calling pullOnce() directly), with the result
// surfaced through pollTick. pollIntervalMs is overridden to a short value
// (see the constructor's doc comment) so this doesn't have to wait out the
// real 90s production cadence.
void TransportStateMachineTest::pollTimerFetchesAndEmitsPollTick()
{
    const QByteArray body = R"({"deliveryMode":"pull","cursor":7,"notifications":[)"
                             R"({"seq":7,"title":"Hi","body":"Body","data":{"messageId":"msg-1"}}]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    RepoHarness repo;
    repo.pair(fake.port());

    TransportStateMachine machine(repo.repository, nullptr, /*pollIntervalMs=*/20);
    QSignalSpy pollSpy(&machine, &TransportStateMachine::pollTick);

    QCOMPARE(machine.currentTier(), TransportTier::Polling); // starts here by default
    QVERIFY(pollSpy.wait());

    QCOMPARE(pollSpy.size(), 1);
    const QVector<PushNotification> delivered = pollSpy.at(0).at(0).value<QVector<PushNotification>>();
    QCOMPARE(delivered.size(), 1);
    QCOMPARE(delivered.first().messageId, QStringLiteral("msg-1"));
    QVERIFY(fake.receivedRequest().contains("GET"));
}

QTEST_GUILESS_MAIN(TransportStateMachineTest)
#include "TransportStateMachineTest.moc"
