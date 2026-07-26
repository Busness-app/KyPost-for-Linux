#include "domain/TransportStateMachine.h"

#include "db/Database.h"
#include "db/PushDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "domain/PushRepository.h"
#include "models/PushNotification.h"
#include "net/HttpClient.h"
#include "net/NtfySubscriber.h"
#include "net/PushNotificationClient.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include "../net/FakeRelayServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Minimal raw QTcpServer harness for a fake ntfy long-poll endpoint,
// test-local rather than a shared header per Task 25's brief (mirrors
// NtfySubscriberTest.cpp's NtfyStreamServer, trimmed to what this test
// needs). Two Qt QNetworkAccessManager quirks documented there apply here
// too: the response must use Transfer-Encoding: chunked (a Content-Length-
// less "read until close" body was observed to make QNAM treat the reply as
// already finished with a 0-length body), and writes must be broadcast to
// every currently-open connection rather than just the latest one (QNAM was
// observed to non-deterministically open a second physical TCP connection
// for a single logical GET against a fresh loopback host).
class FakeNtfyServer : public QObject
{
public:
    FakeNtfyServer()
    {
        m_server.listen(QHostAddress::LocalHost);
        m_port = m_server.serverPort();
        connect(&m_server, &QTcpServer::newConnection, this, &FakeNtfyServer::onNewConnection);
    }

    // Cached: stopAccepting() closes the listening socket, after which
    // QTcpServer::serverPort() reports 0.
    quint16 port() const { return m_port; }

    // Takes the listening socket down entirely, so further connection
    // attempts are refused outright rather than hanging. That is what makes
    // NtfySubscriber's reconnects fail fast enough to spend the failure
    // budget in a test.
    void stopAccepting() { m_server.close(); }

    // Brings it back on the same port, so the subscriber's next retry
    // succeeds and TransportStateMachine can climb back to the
    // EmbeddedSubscriber tier on its own.
    bool resumeAccepting() { return m_server.listen(QHostAddress::LocalHost, m_port); }

    int readyRequestCount() const
    {
        int n = 0;
        for (bool sent : m_headersSent) {
            if (sent)
                ++n;
        }
        return n;
    }

    bool waitForAtLeastConnections(int count, int timeoutMs = 2000)
    {
        QElapsedTimer timer;
        timer.start();
        while (readyRequestCount() < count) {
            if (timer.hasExpired(timeoutMs))
                return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return true;
    }

    // True once every accepted connection has been closed from this side --
    // the observable proxy for "NtfySubscriber::stop() actually aborted the
    // in-flight request", since stop() deliberately does not emit
    // connectionLost (see NtfySubscriber::onFinished's m_stopped guard).
    bool waitForAllDisconnected(int timeoutMs = 2000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!allDisconnected()) {
            if (timer.hasExpired(timeoutMs))
                return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return true;
    }

    void closeAll()
    {
        for (QTcpSocket* socket : std::as_const(m_sockets)) {
            if (socket->state() == QAbstractSocket::ConnectedState)
                socket->disconnectFromHost();
        }
    }

private:
    bool allDisconnected() const
    {
        if (m_sockets.isEmpty())
            return false;
        for (QTcpSocket* socket : m_sockets) {
            if (socket->state() == QAbstractSocket::ConnectedState)
                return false;
        }
        return true;
    }

    void onNewConnection()
    {
        while (QTcpSocket* socket = m_server.nextPendingConnection()) {
            const int index = m_sockets.size();
            m_sockets.append(socket);
            m_headersSent.append(false);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, index]() {
                m_requestBuffers[index] += socket->readAll();
                if (!m_headersSent[index] && m_requestBuffers[index].contains("\r\n\r\n")) {
                    m_headersSent[index] = true;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                                   "Transfer-Encoding: chunked\r\n\r\n");
                    socket->flush();
                }
            });
            m_requestBuffers.append(QByteArray());
        }
    }

    QTcpServer m_server;
    quint16 m_port = 0;
    QList<QTcpSocket*> m_sockets;
    QList<QByteArray> m_requestBuffers;
    QList<bool> m_headersSent;
};

// Owns every real dependency PushRepository needs (matches the wiring
// pattern in PushRepositoryTest.cpp) so TransportStateMachine's polling
// tier can be exercised against a real PushRepository rather than a stub,
// per Task 25's brief. pair(), when called, points the stored pairing at
// `fake` so pullOnce() actually reaches it; tests that never expect a
// network call (distributor/embedded-subscriber tiers) simply leave the
// repository unpaired, since PushRepository::pullOnce() returns an empty
// vector without making a request when there is no stored pairing.
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

class TransportStateMachineTest : public QObject
{
    Q_OBJECT

private slots:
    void startsInPollingWithNoSubscriberConnectionUntilForegrounded();
    void distributorAvailableEntersDistributorAndStopsPolling();
    void distributorUnavailableWithNoForegroundFallsBackToPolling();
    void foregroundedEntersEmbeddedSubscriberAndBackgroundedReturnsToPolling();
    void connectionLostWhileEmbeddedSubscriberRetriesThenDropsToPolling();
    void pollingRetriesTheSubscriberWithoutUserInteraction();
    void enterTierIdempotencyEmitsTierChangedOnce();
    void distributorAlwaysWinsOverForegrounded();
    void pollTimerFetchesAndEmitsPollTick();
};

void TransportStateMachineTest::startsInPollingWithNoSubscriberConnectionUntilForegrounded()
{
    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"));
    RepoHarness repo; // left unpaired -- pullOnce() is a no-op either way

    TransportStateMachine machine(subscriber, repo.repository);

    QCOMPARE(machine.currentTier(), TransportTier::Polling);
    QCOMPARE(ntfyServer.readyRequestCount(), 0);
}

void TransportStateMachineTest::distributorAvailableEntersDistributorAndStopsPolling()
{
    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"));
    RepoHarness repo; // unpaired: pullOnce() is a no-op, so any stray poll tick would
                       // still carry an empty result rather than crash -- pollSpy's
                       // count is what actually proves the timer stopped

    // Short interval: proves the poll timer is genuinely stopped (not just
    // slow) by giving it many chances to fire during the wait window below.
    TransportStateMachine machine(subscriber, repo.repository, nullptr, /*pollIntervalMs=*/20);
    QSignalSpy tierSpy(&machine, &TransportStateMachine::tierChanged);
    QSignalSpy pollSpy(&machine, &TransportStateMachine::pollTick);

    machine.setDistributorAvailable(true);
    QCOMPARE(machine.currentTier(), TransportTier::Distributor);
    QCOMPARE(tierSpy.size(), 1);
    QCOMPARE(qvariant_cast<TransportTier>(tierSpy.at(0).at(0)), TransportTier::Distributor);

    QTest::qWait(200); // 10x the poll interval -- would have fired repeatedly if still running
    QCOMPARE(pollSpy.size(), 0);
    QCOMPARE(ntfyServer.readyRequestCount(), 0);
}

void TransportStateMachineTest::distributorUnavailableWithNoForegroundFallsBackToPolling()
{
    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"));
    RepoHarness repo;

    TransportStateMachine machine(subscriber, repo.repository);
    QSignalSpy tierSpy(&machine, &TransportStateMachine::tierChanged);

    machine.setDistributorAvailable(true);
    QCOMPARE(machine.currentTier(), TransportTier::Distributor);

    machine.setDistributorAvailable(false);
    QCOMPARE(machine.currentTier(), TransportTier::Polling);
    QCOMPARE(tierSpy.size(), 2);
    QCOMPARE(qvariant_cast<TransportTier>(tierSpy.at(1).at(0)), TransportTier::Polling);
}

void TransportStateMachineTest::foregroundedEntersEmbeddedSubscriberAndBackgroundedReturnsToPolling()
{
    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"));
    RepoHarness repo;

    TransportStateMachine machine(subscriber, repo.repository);

    machine.setForegrounded(true);
    QCOMPARE(machine.currentTier(), TransportTier::EmbeddedSubscriber);
    QVERIFY(ntfyServer.waitForAtLeastConnections(1));

    machine.setForegrounded(false);
    QCOMPARE(machine.currentTier(), TransportTier::Polling);
    QVERIFY(ntfyServer.waitForAllDisconnected());
}

// Behaviour change, deliberate: a SINGLE dropped connection no longer demotes.
//
// It used to. That made a two-second Wi-Fi blip or a suspend/resume cycle
// park a still-foregrounded app on 90-second polling permanently, because
// selectTier() only ran from setDistributorAvailable/setForegrounded and
// neither fires again on its own. On a machine with no UnifiedPush
// distributor -- the exact case this tier exists for -- that was the normal
// steady state after the first network hiccup.
//
// Now NtfySubscriber retries with exponential backoff and reports how many
// consecutive failures it has seen; the tier only drops once that budget is
// spent, and a retry timer arms so even the drop is temporary.
void TransportStateMachineTest::connectionLostWhileEmbeddedSubscriberRetriesThenDropsToPolling()
{
    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    // 10 ms reconnect delay so the failure budget is spent in milliseconds
    // rather than the 5 s production default times three.
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"), nullptr, /*reconnectDelayMs=*/10);
    RepoHarness repo;

    TransportStateMachine machine(subscriber, repo.repository);
    QSignalSpy tierSpy(&machine, &TransportStateMachine::tierChanged);

    machine.setForegrounded(true);
    QCOMPARE(machine.currentTier(), TransportTier::EmbeddedSubscriber);
    QVERIFY(ntfyServer.waitForAtLeastConnections(1));

    // First drop: retried, NOT demoted. This is the regression the budget
    // exists to prevent.
    ntfyServer.closeAll();
    QVERIFY(ntfyServer.waitForAtLeastConnections(2));
    QCOMPARE(machine.currentTier(), TransportTier::EmbeddedSubscriber);

    // Keep dropping until the budget is spent. Each close has to be followed
    // by the reconnect it triggers, or successive closeAll() calls in the
    // same event-loop turn all act on the one already-dead socket and only
    // count as a single failure.
    for (int i = 2; i <= TransportStateMachine::kSubscriberFailureBudget; ++i) {
        QVERIFY(ntfyServer.waitForAtLeastConnections(i));
        ntfyServer.closeAll();
    }
    QVERIFY(QTest::qWaitFor([&]() { return machine.currentTier() == TransportTier::Polling; }, 5000));

    // Reported foreground state itself did not change -- only the
    // subscriber's own repeated connectionLost drove this transition.
    QCOMPARE(tierSpy.size(), 2);
    QCOMPARE(qvariant_cast<TransportTier>(tierSpy.at(0).at(0)), TransportTier::EmbeddedSubscriber);
    QCOMPARE(qvariant_cast<TransportTier>(tierSpy.at(1).at(0)), TransportTier::Polling);

    // Re-reporting the same foregrounded(true) value re-attempts the
    // embedded subscriber rather than being swallowed as a no-op change.
    machine.setForegrounded(true);
    QCOMPARE(machine.currentTier(), TransportTier::EmbeddedSubscriber);
}

// The demotion above must not be permanent: the retry timer promotes back to
// EmbeddedSubscriber on its own, with no user interaction. Previously the
// only route back was the user backgrounding and re-foregrounding the app.
void TransportStateMachineTest::pollingRetriesTheSubscriberWithoutUserInteraction()
{
    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"), nullptr, /*reconnectDelayMs=*/10);
    RepoHarness repo;

    // Long poll interval (so the polling tier's own network call stays out of
    // the way), short subscriber-retry interval.
    TransportStateMachine machine(subscriber, repo.repository, nullptr, /*pollIntervalMs=*/600000,
                                   /*subscriberRetryAfterMs=*/50);

    machine.setForegrounded(true);
    QCOMPARE(machine.currentTier(), TransportTier::EmbeddedSubscriber);
    QVERIFY(ntfyServer.waitForAtLeastConnections(1));

    // Listener down + existing connections closed: every reconnect attempt is
    // refused immediately, so the budget is spent in a few tens of ms.
    ntfyServer.stopAccepting();
    ntfyServer.closeAll();
    QVERIFY(QTest::qWaitFor([&]() { return machine.currentTier() == TransportTier::Polling; }, 5000));

    // Nothing calls setForegrounded/setDistributorAvailable here -- the
    // machine has to climb back on its own.
    QVERIFY(ntfyServer.resumeAccepting());
    QVERIFY(QTest::qWaitFor([&]() { return machine.currentTier() == TransportTier::EmbeddedSubscriber; },
                            5000));
}

void TransportStateMachineTest::enterTierIdempotencyEmitsTierChangedOnce()
{
    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"));
    RepoHarness repo;

    TransportStateMachine machine(subscriber, repo.repository);
    QSignalSpy tierSpy(&machine, &TransportStateMachine::tierChanged);

    machine.setForegrounded(true);
    machine.setForegrounded(true);

    QCOMPARE(machine.currentTier(), TransportTier::EmbeddedSubscriber);
    QCOMPARE(tierSpy.size(), 1);
}

void TransportStateMachineTest::distributorAlwaysWinsOverForegrounded()
{
    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"));
    RepoHarness repo;

    TransportStateMachine machine(subscriber, repo.repository);

    machine.setForegrounded(true);
    QCOMPARE(machine.currentTier(), TransportTier::EmbeddedSubscriber);

    machine.setDistributorAvailable(true);
    QCOMPARE(machine.currentTier(), TransportTier::Distributor);

    // foregrounded is still (redundantly) reported true -- must not budge.
    machine.setForegrounded(true);
    QCOMPARE(machine.currentTier(), TransportTier::Distributor);
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

    FakeNtfyServer ntfyServer;
    QNetworkAccessManager manager;
    NtfySubscriber subscriber(manager, QStringLiteral("http://127.0.0.1:%1").arg(ntfyServer.port()),
                               QStringLiteral("mytopic"));
    RepoHarness repo;
    repo.pair(fake.port());

    TransportStateMachine machine(subscriber, repo.repository, nullptr, /*pollIntervalMs=*/20);
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
