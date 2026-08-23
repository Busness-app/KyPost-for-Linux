#include "domain/GroupsRepository.h"

#include "db/Database.h"
#include "db/GroupDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "models/Group.h"
#include "net/GroupsClient.h"
#include "net/HttpClient.h"
#include "stores/SecureStoreFile.h"

#include "../net/FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

class GroupsRepositoryTest : public QObject
{
    Q_OBJECT

private slots:
    void refreshWithoutPairingIsNoOp();
    void refreshUpsertsGroupsFromServer();
    void refreshOnFetchErrorLeavesExistingCacheUntouched();
    void applyRefreshDiscardsAReplyTheCurrentPairingDidNotAuthorise();
    void aGroupDeletedOnTheServerLeavesTheCache();
    void applyRefreshReportsAFailedCacheWrite();

private:
    static void savePairing(PairingStore& pairingStore, quint16 port);
};

void GroupsRepositoryTest::savePairing(PairingStore& pairingStore, quint16 port)
{
    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("secret-1");
    pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    pairing.registrationUrl = QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(port);
    pairing.pairingToken = QStringLiteral("pair-tok");
    pairing.deviceId = QStringLiteral("device-1");
    pairing.deviceName = QStringLiteral("My Linux Desktop");
    QVERIFY(pairingStore.save(pairing));
}

void GroupsRepositoryTest::refreshWithoutPairingIsNoOp()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    GroupDao groupDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- not paired

    QNetworkAccessManager manager;
    HttpClient http(manager);
    GroupsClient client(http);

    GroupsRepository repository(client, groupDao, pairingStore);

    // No FakeRelayServer at all -- refresh() must return without attempting
    // any network call when there's no pairing to derive a server URL from.
    QVERIFY(!repository.refresh()); // nothing was refreshed, and it says so

    QVERIFY(groupDao.findAll().isEmpty());
}

void GroupsRepositoryTest::refreshUpsertsGroupsFromServer()
{
    const QByteArray body = R"([{"id":"group-1","name":"Family","rev":1},)"
                             R"({"id":"group-2","name":"Work","rev":2}])";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    GroupDao groupDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    GroupsClient client(http);

    GroupsRepository repository(client, groupDao, pairingStore);
    QVERIFY(repository.refresh());

    QVERIFY(fake.receivedRequest().contains("GET /api/groups HTTP/1.1"));

    const QVector<Group> all = repository.groups();
    QCOMPARE(all.size(), 2);
}

void GroupsRepositoryTest::refreshOnFetchErrorLeavesExistingCacheUntouched()
{
    // 401 -- GroupsClient degrades gracefully to an error result;
    // GroupsRepository::refresh() must then silently give up rather than
    // wiping or corrupting whatever was cached from a previous successful
    // refresh (no crash, no propagated failure -- see this feature's
    // Global Constraints).
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "Unauthorized\n"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    GroupDao groupDao(db.handle());

    Group existing;
    existing.id = QStringLiteral("group-1");
    existing.name = QStringLiteral("Family");
    existing.rev = 1;
    QVERIFY(groupDao.insertOrReplace(existing));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    GroupsClient client(http);

    GroupsRepository repository(client, groupDao, pairingStore);
    QVERIFY(!repository.refresh()); // a 401 refreshed nothing, and it says so

    const QVector<Group> all = repository.groups();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0), existing);
}

// Group names are the previous account's data and the group table has no
// subscriber column, so a reply that lands after the device has been re-paired
// must not be cached. No server here: phase 3 is exercised directly, which is
// the only way to place the re-pair exactly between the request and the write.
void GroupsRepositoryTest::applyRefreshDiscardsAReplyTheCurrentPairingDidNotAuthorise()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    GroupDao groupDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, 1); // port is irrelevant -- nothing is sent here

    QNetworkAccessManager manager;
    HttpClient http(manager);
    GroupsClient client(http);
    GroupsRepository repository(client, groupDao, pairingStore);

    const std::optional<RelayRequestPlan> plan = repository.planRefresh();
    QVERIFY(plan.has_value());

    Group group;
    group.id = QStringLiteral("g-1");
    group.name = QStringLiteral("Legal counsel");

    GroupsFetchResult result;
    result.groups = QVector<Group>{ group };

    // Re-paired to a different account while the request was out.
    DevicePairing replacement;
    replacement.subscriberId = QStringLiteral("sub-2");
    replacement.deviceId = QStringLiteral("device-2");
    replacement.deviceSecret = QStringLiteral("secret-2");
    replacement.serverBaseUrl = QStringLiteral("http://127.0.0.1:1");
    QVERIFY(pairingStore.save(replacement));

    QVERIFY(!repository.applyRefresh(*plan, result));
    QVERIFY2(groupDao.findAll().isEmpty(),
             "the previous account's group names were cached for the new account");

    // Control: restore the pairing the plan named and the same reply applies,
    // so the assertion above cannot pass on a repository that stopped writing.
    DevicePairing original;
    original.subscriberId = QStringLiteral("sub-1");
    original.deviceId = QStringLiteral("device-1");
    original.deviceSecret = QStringLiteral("secret-1");
    original.serverBaseUrl = QStringLiteral("http://127.0.0.1:1");
    QVERIFY(pairingStore.save(original));
    QVERIFY(repository.applyRefresh(*plan, result));
    QCOMPARE(groupDao.findAll().size(), 1);
}


// /api/groups answers with the whole list, so a group deleted on another
// device is visible here only as an absence from it. The upsert loop this
// replaces could not see an absence: a deleted group stayed in the name-cache
// forever, still labelling contacts with a group they are no longer in.
void GroupsRepositoryTest::aGroupDeletedOnTheServerLeavesTheCache()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    GroupDao groupDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, 1); // nothing is sent in this test

    QNetworkAccessManager manager;
    HttpClient http(manager);
    GroupsClient client(http);
    GroupsRepository repository(client, groupDao, pairingStore);

    const std::optional<RelayRequestPlan> plan = repository.planRefresh();
    QVERIFY(plan.has_value());

    Group family;
    family.id = QStringLiteral("g-1");
    family.name = QStringLiteral("Family");
    Group work;
    work.id = QStringLiteral("g-2");
    work.name = QStringLiteral("Work");

    GroupsFetchResult both;
    both.groups = QVector<Group>{ family, work };
    QVERIFY(repository.applyRefresh(*plan, both));
    QCOMPARE(groupDao.findAll().size(), 2);

    // "Work" was deleted elsewhere: the next response simply does not mention
    // it.
    GroupsFetchResult onlyFamily;
    onlyFamily.groups = QVector<Group>{ family };
    QVERIFY(repository.applyRefresh(*plan, onlyFamily));

    const QVector<Group> after = groupDao.findAll();
    QCOMPARE(after.size(), 1);
    QCOMPARE(after.first().id, QStringLiteral("g-1"));
}

// The cache write used to be unreportable: applyRefresh() returned void, so a
// half-applied response and a correct one looked identical from the caller.
void GroupsRepositoryTest::applyRefreshReportsAFailedCacheWrite()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    GroupDao groupDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, 1);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    GroupsClient client(http);
    GroupsRepository repository(client, groupDao, pairingStore);

    const std::optional<RelayRequestPlan> plan = repository.planRefresh();
    QVERIFY(plan.has_value());

    {
        QSqlQuery drop(db.handle());
        QVERIFY(drop.exec(QStringLiteral("DROP TABLE groups")));
    }

    Group group;
    group.id = QStringLiteral("g-1");
    group.name = QStringLiteral("Family");

    GroupsFetchResult result;
    result.groups = QVector<Group>{ group };

    QVERIFY2(!repository.applyRefresh(*plan, result), "a cache write that failed was reported as a refresh");
}

QTEST_GUILESS_MAIN(GroupsRepositoryTest)
#include "GroupsRepositoryTest.moc"
