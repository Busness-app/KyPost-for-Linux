#include "domain/PushRepository.h"

#include "db/Database.h"
#include "db/PushDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "models/PushNotification.h"
#include "net/HttpClient.h"
#include "net/PushNotificationClient.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include "../net/FakeRelayServer.h"

#include <QDir>
#include <QNetworkAccessManager>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class PushRepositoryTest : public QObject
{
    Q_OBJECT

private slots:
    void differentMessageIdsSameInstantGetDifferentSeqs();
    void sameMessageIdArrivingTwiceReusesOneRow();
    void pullDeduplicatesBySeqAndAdvancesCursorAfterHandoff();
    void storedPullEndpointOverridesDerivedOne();
    void unsetPullEndpointIsDerivedFromServerBaseUrl();
    void pullWithoutPairingReturnsEmptyAndMakesNoRequest();
    void pullDiscardsAReplyTheCurrentPairingDidNotAuthorise();
    void historyReturnsMostRecentFirstUpToLimit();
    void markReadDelegatesToPushDao();
    void nothingIsDeliveredWhenThePullBatchCannotBeStored();
    void nothingIsDeliveredWhenTheCursorCannotBePersisted();

private:
    static void savePairing(PairingStore& pairingStore, const QString& serverBaseUrl);
    static PushNotification samplePayload(const QString& messageId);
};

void PushRepositoryTest::savePairing(PairingStore& pairingStore, const QString& serverBaseUrl)
{
    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("secret-1");
    pairing.serverBaseUrl = serverBaseUrl;
    pairing.registrationUrl = serverBaseUrl + QStringLiteral("/api/notifications/native/register");
    pairing.pairingToken = QStringLiteral("pair-tok");
    pairing.deviceId = QStringLiteral("device-1");
    pairing.deviceName = QStringLiteral("My Linux Desktop");
    QVERIFY(pairingStore.save(pairing));
}

PushNotification PushRepositoryTest::samplePayload(const QString& messageId)
{
    PushNotification payload;
    payload.messageId = messageId;
    payload.sender = QStringLiteral("a@example.com");
    payload.subject = QStringLiteral("Hello");
    payload.title = QStringLiteral("Alice");
    payload.body = QStringLiteral("Hello there");
    return payload;
}

// Proves seq uniqueness is maintained for cursor-ordering correctness even
// when two distinct messages arrive in the same millisecond -- the
// collision-avoidance loop in recordPushArrival() must bump the second one.
void PushRepositoryTest::differentMessageIdsSameInstantGetDifferentSeqs()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    const qint64 instant = 1731000000123;
    const std::optional<PushRecord> first =
        repository.recordPushArrival(samplePayload(QStringLiteral("msg-1")), instant);
    const std::optional<PushRecord> second =
        repository.recordPushArrival(samplePayload(QStringLiteral("msg-2")), instant);

    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QCOMPARE(first->seq, instant);
    QVERIFY(second->seq != first->seq);
    QCOMPARE(second->seq, instant + 1);

    const QVector<PushRecord> history = repository.history();
    QCOMPARE(history.size(), 2);
}

// Deviation from PushTests.swift's pushArrivalsGetUniqueSynthesizedSeqs
// (see PushRepository::recordPushArrival's doc comment): our
// push_notifications table is keyed by message_id, so the *same* messageId
// arriving twice must reuse one row, not create two.
void PushRepositoryTest::sameMessageIdArrivingTwiceReusesOneRow()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    const qint64 instant = 1731000000123;
    QVERIFY(repository.recordPushArrival(samplePayload(QStringLiteral("msg-1")), instant).has_value());
    QVERIFY(repository.recordPushArrival(samplePayload(QStringLiteral("msg-1")), instant).has_value());

    const QVector<PushRecord> history = repository.history();
    QCOMPARE(history.size(), 1);
    QCOMPARE(history.first().messageId, QStringLiteral("msg-1"));
}

void PushRepositoryTest::pullDeduplicatesBySeqAndAdvancesCursorAfterHandoff()
{
    const QByteArray body = R"({"deliveryMode":"pull","cursor":10,"notifications":[)"
                             R"({"seq":3,"title":"Old","body":"Old body","data":{"messageId":"msg-old"}},)"
                             R"({"seq":5,"title":"New","body":"New body","data":{"messageId":"msg-new"}}]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QVERIFY(cursorStore.setNotificationCursor(3));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    const QVector<PushNotification> delivered = repository.pullOnce();

    QCOMPARE(delivered.size(), 1);
    QCOMPARE(delivered.first().messageId, QStringLiteral("msg-new"));

    QVERIFY(!pushDao.findById(QStringLiteral("msg-old")).has_value());
    const std::optional<PushRecord> persisted = pushDao.findById(QStringLiteral("msg-new"));
    QVERIFY(persisted.has_value());
    QCOMPARE(persisted->seq, qint64(5));

    QCOMPARE(cursorStore.notificationCursor(), qint64(10));
    QVERIFY(fake.receivedRequest().contains("after=3"));
}

void PushRepositoryTest::storedPullEndpointOverridesDerivedOne()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"deliveryMode":"pull","cursor":0,"notifications":[]})"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    // serverBaseUrl deliberately points nowhere real -- if the derived
    // fallback were used instead of the stored override, this test's
    // FakeRelayServer would never see a connection.
    savePairing(pairingStore, QStringLiteral("http://127.0.0.1:1"));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    settingsStore.setPullEndpoint(
        QStringLiteral("http://127.0.0.1:%1/custom/pull/path").arg(fake.port()));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);
    repository.pullOnce();

    QVERIFY(fake.receivedRequest().contains("GET /custom/pull/path HTTP/1.1"));
}

void PushRepositoryTest::unsetPullEndpointIsDerivedFromServerBaseUrl()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"deliveryMode":"pull","cursor":0,"notifications":[]})"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    // Trailing slash on purpose -- resolvePullEndpoint() must trim it before
    // appending the derived path.
    savePairing(pairingStore, QStringLiteral("http://127.0.0.1:%1/").arg(fake.port()));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    // pullEndpoint left unset.

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);
    repository.pullOnce();

    QVERIFY(fake.receivedRequest().contains("GET /api/notifications/native/pull HTTP/1.1"));
}

void PushRepositoryTest::pullWithoutPairingReturnsEmptyAndMakesNoRequest()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- not paired

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    const QVector<PushNotification> delivered = repository.pullOnce();
    QVERIFY(delivered.isEmpty());
    QCOMPARE(cursorStore.notificationCursor(), qint64(0));
}

// The account is replaced while the poll is in flight.
//
// This is the polling tier's own path and it needs no unusual timing to hit:
// pullOnce() still blocks the GUI thread on a nested event loop, so anything
// queued to that thread -- including the executor completion that purges the
// previous account's data and stores the replacement pairing -- is delivered
// DURING the request. The singleShot(0) below is not a contrivance; it is
// exactly how the real re-pair arrives.
//
// Two things must not happen afterwards: the previous account's notifications
// must not be written into the emptied table, and the notification cursor
// must not be advanced to the previous account's position -- the cursor is a
// single global value, so moving it makes the NEW account's next poll ask for
// everything AFTER that sequence number and silently skip its own backlog.
void PushRepositoryTest::pullDiscardsAReplyTheCurrentPairingDidNotAuthorise()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({
      "deliveryMode": "pull",
      "cursor": 42,
      "notifications": [
        { "seq": 41, "messageId": "msg-previous-account", "sender": "hr@example.com",
          "subject": "Salary review", "title": "HR", "body": "Confidential" }
      ]
    })"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QVERIFY(cursorStore.setNotificationCursor(7));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    // Fires inside the nested event loop pullOnce() is blocked on, between
    // the request going out and the reply being applied.
    bool replaced = false;
    QTimer::singleShot(0, [&]() {
        DevicePairing replacement;
        replacement.subscriberId = QStringLiteral("sub-2");
        replacement.deviceId = QStringLiteral("device-2");
        replacement.deviceSecret = QStringLiteral("secret-2");
        replacement.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
        replaced = pairingStore.save(replacement);
    });

    const QVector<PushNotification> delivered = repository.pullOnce();

    // Asserted, not assumed: if the timer had not run inside the pull, every
    // assertion below would pass for the wrong reason.
    QVERIFY2(replaced, "the re-pair did not happen during the request -- this test proved nothing");

    QVERIFY(delivered.isEmpty());
    QVERIFY2(!pushDao.findById(QStringLiteral("msg-previous-account")).has_value(),
             "the previous account's notification was written into the new account's history");
    QCOMPARE(cursorStore.notificationCursor(), qint64(7));
}

void PushRepositoryTest::historyReturnsMostRecentFirstUpToLimit()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    QVERIFY(repository.recordPushArrival(samplePayload(QStringLiteral("msg-1")), 100).has_value());
    QVERIFY(repository.recordPushArrival(samplePayload(QStringLiteral("msg-2")), 200).has_value());
    QVERIFY(repository.recordPushArrival(samplePayload(QStringLiteral("msg-3")), 300).has_value());

    const QVector<PushRecord> limited = repository.history(2);
    QCOMPARE(limited.size(), 2);
    QCOMPARE(limited.at(0).messageId, QStringLiteral("msg-3"));
    QCOMPARE(limited.at(1).messageId, QStringLiteral("msg-2"));
}

void PushRepositoryTest::markReadDelegatesToPushDao()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);

    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    QVERIFY(repository.recordPushArrival(samplePayload(QStringLiteral("msg-1")), 100).has_value());
    QVERIFY(repository.markRead(QStringLiteral("msg-1")));

    const std::optional<PushRecord> record = pushDao.findById(QStringLiteral("msg-1"));
    QVERIFY(record.has_value());
    QVERIFY(record->consumed);
}


// A notification shown from a row that never landed is gone from history the
// moment it is dismissed, and the relay will not mention it again once the
// cursor has moved past it. So a batch that cannot be stored is not shown at
// all: the cursor stays put and the next poll, 90 seconds later, asks for the
// same window again.
void PushRepositoryTest::nothingIsDeliveredWhenThePullBatchCannotBeStored()
{
    const QByteArray body = R"({"deliveryMode":"pull","cursor":10,"notifications":[)"
                             R"({"seq":5,"title":"New","body":"New body","data":{"messageId":"msg-new"}}]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QVERIFY(cursorStore.setNotificationCursor(3));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);
    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    // Take the table away, the bluntest honest way to make every write fail.
    {
        QSqlQuery drop(db.handle());
        QVERIFY(drop.exec(QStringLiteral("DROP TABLE push_notifications")));
    }

    QVERIFY2(repository.pullOnce().isEmpty(), "a notification that could not be stored was handed out anyway");
    QCOMPARE(cursorStore.notificationCursor(), qint64(3));
}

// The mirror image: the rows land, the cursor does not. Delivering here would
// show the notification now and again on the next launch, because the cursor
// the next launch reads is the old one.
void PushRepositoryTest::nothingIsDeliveredWhenTheCursorCannotBePersisted()
{
    const QByteArray body = R"({"deliveryMode":"pull","cursor":10,"notifications":[)"
                             R"({"seq":5,"title":"New","body":"New body","data":{"messageId":"msg-new"}}]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    PushDao pushDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    const QString blocked = cursorDir.filePath(QStringLiteral("cursors.ini"));
    QVERIFY(QDir().mkpath(blocked)); // a directory here: no file can be written
    CursorStore cursorStore(blocked);

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PushNotificationClient client(http);
    PushRepository repository(pushDao, cursorStore, client, pairingStore, settingsStore);

    QVERIFY2(repository.pullOnce().isEmpty(),
             "notifications were delivered against a cursor the next launch will not have");
}

QTEST_GUILESS_MAIN(PushRepositoryTest)
#include "PushRepositoryTest.moc"
