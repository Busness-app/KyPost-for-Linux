#include "domain/FolderRepository.h"

#include "db/Database.h"
#include "db/FolderDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/FolderClient.h"
#include "net/HttpClient.h"
#include "stores/SecureStoreFile.h"

#include "../net/FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>

class FolderRepositoryTest : public QObject
{
    Q_OBJECT

private slots:
    void refreshCachesFoldersUnderParent();
    void refreshReplacesRatherThanMergesSoDeletionsPropagate();
    void refreshNotPairedReturnsNotPairedAndMakesNoRequest();
    void refreshUnauthorizedLeavesCacheIntact();
    void cachedFoldersSortsByPath();

private:
    static void savePairing(PairingStore& pairingStore, quint16 port);
};

void FolderRepositoryTest::savePairing(PairingStore& pairingStore, quint16 port)
{
    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("secret-1");
    pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    pairing.deviceId = QStringLiteral("dev-1");
    QVERIFY(pairingStore.save(pairing));
}

void FolderRepositoryTest::refreshCachesFoldersUnderParent()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    FolderDao folderDao(db.handle());

    FakeRelayServer fake(httpResponse(200, "OK", R"({
      "parent": "Archive",
      "folders": [{"path": "Archive/2026", "deletable": true},
                  {"path": "Archive/2025", "deletable": false}]
    })"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);
    FolderRepository repository(client, folderDao, pairingStore);

    const MailFetchOutcome outcome = repository.refresh(QStringLiteral("Archive"));
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::Success);

    const QVector<MailFolder> cached = repository.cachedFolders(QStringLiteral("Archive"));
    QCOMPARE(cached.size(), 2);
    QCOMPARE(cached.at(0).path, QStringLiteral("Archive/2025"));
    QCOMPARE(cached.at(0).deletable, false);
    QCOMPARE(cached.at(1).path, QStringLiteral("Archive/2026"));
    QCOMPARE(cached.at(1).deletable, true);
}

void FolderRepositoryTest::refreshReplacesRatherThanMergesSoDeletionsPropagate()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    FolderDao folderDao(db.handle());

    // A folder deleted on the server is only observable as an absence from
    // the list, so an upsert loop would leave it in the sidebar forever.
    QVERIFY(folderDao.insertOrReplace(QStringLiteral("Archive/gone"), QStringLiteral("Archive"), true,
                                       QStringLiteral("relay")));

    FakeRelayServer fake(httpResponse(
        200, "OK", R"({"parent":"Archive","folders":[{"path":"Archive/kept","deletable":true}]})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);
    FolderRepository repository(client, folderDao, pairingStore);

    QCOMPARE(repository.refresh(QStringLiteral("Archive")).outcome, MailRepositoryOutcome::Success);

    const QVector<MailFolder> cached = repository.cachedFolders(QStringLiteral("Archive"));
    QCOMPARE(cached.size(), 1);
    QCOMPARE(cached.at(0).path, QStringLiteral("Archive/kept"));
}

void FolderRepositoryTest::refreshNotPairedReturnsNotPairedAndMakesNoRequest()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    FolderDao folderDao(db.handle());

    FakeRelayServer fake(httpResponse(200, "OK", R"({"parent":"Archive","folders":[]})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // deliberately not saved

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);
    FolderRepository repository(client, folderDao, pairingStore);

    QCOMPARE(repository.refresh(QStringLiteral("Archive")).outcome, MailRepositoryOutcome::NotPaired);
    QVERIFY(fake.receivedRequest().isEmpty());
}

void FolderRepositoryTest::refreshUnauthorizedLeavesCacheIntact()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    FolderDao folderDao(db.handle());

    QVERIFY(folderDao.insertOrReplace(QStringLiteral("Archive/2026"), QStringLiteral("Archive"), true,
                                       QStringLiteral("relay")));

    FakeRelayServer fake(httpResponse(401, "Unauthorized", "unauthorized", "text/plain"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);
    FolderRepository repository(client, folderDao, pairingStore);

    QCOMPARE(repository.refresh(QStringLiteral("Archive")).outcome, MailRepositoryOutcome::Unauthorized);

    // A failed refresh must not wipe what the sidebar is currently showing.
    QCOMPARE(repository.cachedFolders(QStringLiteral("Archive")).size(), 1);
}

void FolderRepositoryTest::cachedFoldersSortsByPath()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    FolderDao folderDao(db.handle());

    for (const QString& path : { QStringLiteral("Archive/zeta"), QStringLiteral("Archive/alpha"),
                                  QStringLiteral("Archive/mid") }) {
        QVERIFY(folderDao.insertOrReplace(path, QStringLiteral("Archive"), true, QStringLiteral("relay")));
    }

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);
    FolderRepository repository(client, folderDao, pairingStore);

    const QVector<MailFolder> cached = repository.cachedFolders(QStringLiteral("Archive"));
    QCOMPARE(cached.size(), 3);
    QCOMPARE(cached.at(0).path, QStringLiteral("Archive/alpha"));
    QCOMPARE(cached.at(1).path, QStringLiteral("Archive/mid"));
    QCOMPARE(cached.at(2).path, QStringLiteral("Archive/zeta"));
}

QTEST_GUILESS_MAIN(FolderRepositoryTest)
#include "FolderRepositoryTest.moc"
