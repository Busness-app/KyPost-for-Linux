#include "contacts/ContactsController.h"

#include "contacts/ContactListModel.h"
#include "db/ContactDao.h"
#include "db/Database.h"
#include "db/GroupDao.h"
#include "db/PendingContactChangeDao.h"
#include "domain/ContactPhotoRepository.h"
#include "domain/ContactSyncRepository.h"
#include "domain/DevicePairing.h"
#include "domain/GroupsRepository.h"
#include "domain/PairingStore.h"
#include "models/Group.h"
#include "net/ContactPhotoClient.h"
#include "net/ContactSyncClient.h"
#include "net/GroupsClient.h"
#include "net/HttpClient.h"
#include "stores/ContactPhotoCache.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"

#include "../../core/net/FakeRelayServer.h"
#include "../ExecutorShutdownGuard.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QSet>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>
#include <utility>

namespace {

// Review follow-up (Task 2): the shared single-response FakeRelayServer
// (tests/core/net/FakeRelayServer.h) can't be reused as-is for
// syncSuccessRefreshesGroupsCache below, because that test drives
// ContactsController::sync() end to end -- which fires *two* sequential
// outbound requests against the same paired serverBaseUrl (first
// ContactSyncRepository::sync()'s GET /api/contacts/sync pull, then, only on
// Success, GroupsRepository::refresh()'s GET /api/groups) and each needs its
// own canned response shape. This is a small purpose-built variant, local to
// this test file, that dispatches on the request path instead of always
// replaying one fixed response. Both requests here are plain GETs (no
// pending changes queued -> ContactSyncRepository::sync() takes the pull(),
// not push(), branch), so -- unlike FakeRelayServer -- this doesn't need to
// wait for a Content-Length body; the header terminator is enough to know
// the request is complete.
class DualPathFakeRelayServer : public QObject
{
public:
    DualPathFakeRelayServer(QByteArray contactsSyncResponse, QByteArray groupsResponse)
        : m_contactsSyncResponse(std::move(contactsSyncResponse))
        , m_groupsResponse(std::move(groupsResponse))
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, &DualPathFakeRelayServer::onNewConnection);
    }

    quint16 port() const { return m_server.serverPort(); }
    bool groupsRequestReceived() const { return m_groupsRequestReceived; }
    bool contactsSyncRequestReceived() const { return m_contactsSyncRequestReceived; }

private:
    void onNewConnection()
    {
        QTcpSocket* socket = m_server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
            *buffer += socket->readAll();
            if (buffer->indexOf("\r\n\r\n") < 0)
                return; // wait for the full header block
            const bool isGroupsRequest = buffer->contains("/api/groups");
            if (isGroupsRequest)
                m_groupsRequestReceived = true;
            else if (buffer->contains("/api/contacts/sync"))
                m_contactsSyncRequestReceived = true;
            socket->write(isGroupsRequest ? m_groupsResponse : m_contactsSyncResponse);
            socket->flush();
            socket->disconnectFromHost();
        });
    }

    QTcpServer m_server;
    QByteArray m_contactsSyncResponse;
    QByteArray m_groupsResponse;
    bool m_groupsRequestReceived = false;
    bool m_contactsSyncRequestReceived = false;
};

// Same path-dispatch idea as DualPathFakeRelayServer, extended to a third
// endpoint -- ContactsController::dedupe() on a successful merge chains into
// sync() (contacts/sync pull) which on Success itself chains into
// GroupsRepository::refresh() (api/groups), so a merge-then-sync test needs
// three distinct canned responses behind one fake server.
class TriplePathFakeRelayServer : public QObject
{
public:
    TriplePathFakeRelayServer(QByteArray dedupeResponse, QByteArray contactsSyncResponse, QByteArray groupsResponse)
        : m_dedupeResponse(std::move(dedupeResponse))
        , m_contactsSyncResponse(std::move(contactsSyncResponse))
        , m_groupsResponse(std::move(groupsResponse))
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, &TriplePathFakeRelayServer::onNewConnection);
    }

    quint16 port() const { return m_server.serverPort(); }
    bool dedupeRequestReceived() const { return m_dedupeRequestReceived; }
    bool contactsSyncRequestReceived() const { return m_contactsSyncRequestReceived; }
    bool groupsRequestReceived() const { return m_groupsRequestReceived; }

private:
    void onNewConnection()
    {
        QTcpSocket* socket = m_server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
            *buffer += socket->readAll();
            if (buffer->indexOf("\r\n\r\n") < 0)
                return; // wait for the full header block

            if (buffer->contains("/api/contacts/dedupe")) {
                m_dedupeRequestReceived = true;
                socket->write(m_dedupeResponse);
            } else if (buffer->contains("/api/groups")) {
                m_groupsRequestReceived = true;
                socket->write(m_groupsResponse);
            } else if (buffer->contains("/api/contacts/sync")) {
                m_contactsSyncRequestReceived = true;
                socket->write(m_contactsSyncResponse);
            }
            socket->flush();
            socket->disconnectFromHost();
        });
    }

    QTcpServer m_server;
    QByteArray m_dedupeResponse;
    QByteArray m_contactsSyncResponse;
    QByteArray m_groupsResponse;
    bool m_dedupeRequestReceived = false;
    bool m_contactsSyncRequestReceived = false;
    bool m_groupsRequestReceived = false;
};

} // namespace

class ContactsControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void updateContactReplacesEmailsPhonesAddressesWholesale();
    void isSyncedReflectsPendingState();
    void createContactRejectsBlankName();
    void aContactThatCouldNotBeSavedIsReportedAsNotSaved();
    void updateContactRejectsBlankName();
    void syncWithoutPairingSetsNotPairedMessage();
    void syncSuccessRefreshesGroupsCache();
    void loadSortsSelfContactFirst();
    void createAndUpdateContactRoundTripExtendedFields();
    void allGroupsReturnsCachedGroupsAsIdNameMaps();
    void dedupeSuccessWithMergesChainsIntoSyncAndReloadsModel();
    void dedupeSuccessWithZeroMergedSkipsSyncButReloadsModel();
    void dedupeUnauthorizedSetsLastErrorNotStatusMessage();
    void searchContactsMatchesAcrossMultipleEmailsPerContact();
    void searchContactsIsCaseInsensitiveSubstring();
    void searchContactsRanksPrefixMatchesFirst();
    void searchContactsRespectsLimit();
    void searchContactsEmptyQueryReturnsEverythingUpToLimit();
    void searchContactsZeroOrNegativeLimitIsUnbounded();

private:
    static void savePairing(PairingStore& pairingStore, quint16 port);
};

void ContactsControllerTest::savePairing(PairingStore& pairingStore, quint16 port)
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

void ContactsControllerTest::updateContactReplacesEmailsPhonesAddressesWholesale()
{
    // emails/phones/addresses are whole-list-replace fields, same as every
    // other list-typed field (ims/websites/etc.) -- an update replaces the
    // entire list, it does not preserve any prior entries, and omitting the
    // key clears the field entirely.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- not paired, fine: this test never syncs

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    // extended-contact-fields Task 3: ContactsController's third constructor
    // dependency -- these tests don't exercise photoPathFor() itself
    // (that's ContactPhotoRepositoryTest's job), so a plain never-populated
    // cache dir is enough to satisfy the constructor.
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());

    Contact seed;
    seed.uid = QStringLiteral("srv-1");
    seed.rev = 1; // seeded directly into contactDao, no pendingDao row -- reads as synced under isSynced()
    seed.fn = QStringLiteral("Old Name");
    seed.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("old@example.com") },
                     ContactEmailEntry{ QStringLiteral("work"), QStringLiteral("extra@example.com") } };
    seed.phones = { ContactPhoneEntry{ std::nullopt, QStringLiteral("555-0100") } };
    seed.addresses = { ContactAddressEntry{ QStringLiteral("home"), QStringLiteral("1 Old St"), QStringLiteral("Old Town"),
                                             std::nullopt, std::nullopt, std::nullopt } };
    QVERIFY(contactDao.insertOrReplace(seed));

    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QVariantMap newEmail;
    newEmail[QStringLiteral("value")] = QStringLiteral("new@example.com");
    QVariantMap newPhone;
    newPhone[QStringLiteral("label")] = QStringLiteral("mobile");
    newPhone[QStringLiteral("value")] = QStringLiteral("555-0200");

    QVariantMap fields;
    fields[QStringLiteral("fn")] = QStringLiteral("New Name");
    fields[QStringLiteral("org")] = QString();
    fields[QStringLiteral("notes")] = QString();
    fields[QStringLiteral("emails")] = QVariantList{ newEmail };
    fields[QStringLiteral("phones")] = QVariantList{ newPhone };
    // addresses deliberately omitted -- proves omission clears the field.

    QVERIFY(controller.updateContact(QStringLiteral("srv-1"), fields));

    const QVariantMap updated = controller.contactAt(QStringLiteral("srv-1"));
    QCOMPARE(updated.value(QStringLiteral("fn")).toString(), QStringLiteral("New Name"));

    // The new list replaces the old one wholesale -- the seed's second email
    // does NOT survive.
    const QVariantList emails = updated.value(QStringLiteral("emails")).toList();
    QCOMPARE(emails.size(), 1);
    QCOMPARE(emails.at(0).toMap().value(QStringLiteral("value")).toString(), QStringLiteral("new@example.com"));

    const QVariantList phones = updated.value(QStringLiteral("phones")).toList();
    QCOMPARE(phones.size(), 1);
    QCOMPARE(phones.at(0).toMap().value(QStringLiteral("value")).toString(), QStringLiteral("555-0200"));
    QCOMPARE(phones.at(0).toMap().value(QStringLiteral("label")).toString(), QStringLiteral("mobile"));

    // Omitted key clears the field, same rule as ims/websites/etc.
    QVERIFY(updated.value(QStringLiteral("addresses")).toList().isEmpty());

    // The model was reloaded, too.
    auto* model = qobject_cast<ContactListModel*>(controller.contactModel());
    QVERIFY(model != nullptr);
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->data(model->index(0, 0), ContactListModel::FnRole).toString(), QStringLiteral("New Name"));
}

void ContactsControllerTest::isSyncedReflectsPendingState()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    // Unknown uid: never existed, not synced.
    QVERIFY(!controller.isSynced(QStringLiteral("does-not-exist")));

    // createContact() queues a create -- pending, not yet synced.
    QVariantMap fields;
    fields[QStringLiteral("fn")] = QStringLiteral("Fresh Contact");
    const QString newUid = controller.createContact(fields);
    QVERIFY(!newUid.isEmpty());
    QVERIFY(!controller.isSynced(newUid));

    // Simulate what a successful sync() does to the pending table (no live
    // server round trip needed to prove isSynced() reads it correctly).
    pendingDao.deleteAll();
    QVERIFY(controller.isSynced(newUid));
}

void ContactsControllerTest::createContactRejectsBlankName()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    // extended-contact-fields Task 3: ContactsController's third constructor
    // dependency -- these tests don't exercise photoPathFor() itself
    // (that's ContactPhotoRepositoryTest's job), so a plain never-populated
    // cache dir is enough to satisfy the constructor.
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QSignalSpy errorSpy(&controller, &ContactsController::lastErrorChanged);

    QVariantMap fields;
    fields[QStringLiteral("fn")] = QStringLiteral("   "); // whitespace-only

    const QString newUid = controller.createContact(fields);

    QVERIFY(newUid.isEmpty());
    QCOMPARE(controller.lastError(), QStringLiteral("Name is required"));
    QVERIFY(errorSpy.count() >= 1);
    QVERIFY(repository.contacts().isEmpty());
}

void ContactsControllerTest::updateContactRejectsBlankName()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    // extended-contact-fields Task 3: ContactsController's third constructor
    // dependency -- these tests don't exercise photoPathFor() itself
    // (that's ContactPhotoRepositoryTest's job), so a plain never-populated
    // cache dir is enough to satisfy the constructor.
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());

    Contact seed;
    seed.uid = QStringLiteral("srv-1");
    seed.rev = 1;
    seed.fn = QStringLiteral("Old Name");
    QVERIFY(contactDao.insertOrReplace(seed));

    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QVariantMap fields;
    fields[QStringLiteral("fn")] = QString();

    QCOMPARE(controller.updateContact(QStringLiteral("srv-1"), fields), false);
    QCOMPARE(controller.lastError(), QStringLiteral("Name is required"));

    // The seed row is untouched.
    const QVariantMap unchanged = controller.contactAt(QStringLiteral("srv-1"));
    QCOMPARE(unchanged.value(QStringLiteral("fn")).toString(), QStringLiteral("Old Name"));
}

void ContactsControllerTest::syncWithoutPairingSetsNotPairedMessage()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- not paired

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    // extended-contact-fields Task 3: ContactsController's third constructor
    // dependency -- these tests don't exercise photoPathFor() itself
    // (that's ContactPhotoRepositoryTest's job), so a plain never-populated
    // cache dir is enough to satisfy the constructor.
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QSignalSpy errorSpy(&controller, &ContactsController::lastErrorChanged);
    QSignalSpy statusSpy(&controller, &ContactsController::statusMessageChanged);
    QSignalSpy busySpy(&controller, &ContactsController::isBusyChanged);

    controller.sync();

    // Refused synchronously: the pairing read is phase 1, on this thread, so
    // there is nothing to wait for and nothing was dispatched.
    QCOMPARE(controller.lastError(), QStringLiteral("Not paired"));
    QCOMPARE(controller.statusMessage(), QString());
    QVERIFY(errorSpy.count() >= 1);
    QCOMPARE(statusSpy.count(), 0); // was already "", stayed ""
    // isBusy never went true: it is only set once a request is actually on
    // its way, and this never got that far.
    QCOMPARE(busySpy.count(), 0);
    QCOMPARE(controller.isBusy(), false);
}

void ContactsControllerTest::syncSuccessRefreshesGroupsCache()
{
    // Review follow-up (Task 2 finding): the line this whole GroupsRepository
    // design decision exists to wire up -- ContactsController::sync() calling
    // m_groupsRepository.refresh() in the ContactSyncStatus::Success branch
    // -- previously had zero automated regression protection, since every
    // existing sync() test here hit the NotPaired branch. This drives sync()
    // through a real Success outcome (valid pairing, no pending changes so
    // ContactSyncRepository::sync() takes the GET pull() branch) and asserts
    // the *observable effect* of refresh() having actually run: the groups
    // fetched from the fake server land in GroupsRepository::groups()/
    // GroupDao's cache. A future refactor that drops or misplaces the
    // refresh() call would leave the cache empty and fail this test.
    const QByteArray contactsSyncResponse =
        httpResponse(200, "OK", R"({"cursor":1,"tooOld":false,"changed":[],"deleted":[]})");
    const QByteArray groupsResponse =
        httpResponse(200, "OK", R"([{"id":"group-1","name":"Family","rev":1}])");
    DualPathFakeRelayServer fake(contactsSyncResponse, groupsResponse);

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    // extended-contact-fields Task 3: ContactsController's third constructor
    // dependency -- these tests don't exercise photoPathFor() itself
    // (that's ContactPhotoRepositoryTest's job), so a plain never-populated
    // cache dir is enough to satisfy the constructor.
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QVERIFY(groupsRepository.groups().isEmpty()); // nothing cached before sync()

    controller.sync();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 10000);

    QCOMPARE(controller.lastError(), QString());
    QCOMPARE(controller.statusMessage(), QStringLiteral("Synced -- 0 pushed, 0 applied"));

    QVERIFY(fake.contactsSyncRequestReceived());
    QVERIFY(fake.groupsRequestReceived()); // proves refresh() was actually invoked

    const QVector<Group> cachedGroups = groupsRepository.groups();
    QCOMPARE(cachedGroups.size(), 1);
    QCOMPARE(cachedGroups.at(0).id, QStringLiteral("group-1"));
    QCOMPARE(cachedGroups.at(0).name, QStringLiteral("Family"));
}

void ContactsControllerTest::loadSortsSelfContactFirst()
{
    // contact-self-and-pgp-qr-card Task 4: load() sorts the self-flagged
    // contact to the front via std::stable_partition, leaving every other
    // contact's relative order unchanged -- this is a pure in-memory sort
    // over m_repository.contacts(), no network/pairing involved, so this
    // fixture mirrors the minimal no-sync tests above (e.g.
    // createContactRejectsBlankName) rather than the fake-server ones.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact notSelf;
    notSelf.uid = QStringLiteral("c-1");
    notSelf.fn = QStringLiteral("Alice");
    QVERIFY(contactDao.insertOrReplace(notSelf));

    Contact self;
    self.uid = QStringLiteral("c-2");
    self.fn = QStringLiteral("Me");
    self.isSelf = true;
    QVERIFY(contactDao.insertOrReplace(self));

    Contact alsoNotSelf;
    alsoNotSelf.uid = QStringLiteral("c-3");
    alsoNotSelf.fn = QStringLiteral("Bob");
    QVERIFY(contactDao.insertOrReplace(alsoNotSelf));

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- not paired, fine: this test never syncs

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    controller.load();

    auto* listModel = qobject_cast<ContactListModel*>(controller.contactModel());
    QVERIFY(listModel != nullptr);
    QCOMPARE(listModel->rowCount(), 3);
    QCOMPARE(listModel->contactAt(0).uid, QStringLiteral("c-2")); // self-contact sorted first
    // The two non-self contacts keep their original relative order.
    QCOMPARE(listModel->contactAt(1).uid, QStringLiteral("c-1"));
    QCOMPARE(listModel->contactAt(2).uid, QStringLiteral("c-3"));
}

void ContactsControllerTest::createAndUpdateContactRoundTripExtendedFields()
{
    // Every one of these QVariantMap keys, including addresses, is a
    // whole-value/whole-list replace on createContact/updateContact/
    // contactAt -- exercised together here since ContactDetail.qml's edit
    // form relies on this contract uniformly.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    // extended-contact-fields Task 3: ContactsController's third constructor
    // dependency -- these tests don't exercise photoPathFor() itself
    // (that's ContactPhotoRepositoryTest's job), so a plain never-populated
    // cache dir is enough to satisfy the constructor.
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QVariantMap imEntry;
    imEntry[QStringLiteral("service")] = QStringLiteral("Matrix");
    imEntry[QStringLiteral("label")] = QStringLiteral("work");
    imEntry[QStringLiteral("value")] = QStringLiteral("@ada:example.org");

    QVariantMap websiteEntry;
    websiteEntry[QStringLiteral("label")] = QStringLiteral("blog");
    websiteEntry[QStringLiteral("value")] = QStringLiteral("https://ada.example.com");

    QVariantMap relationEntry;
    relationEntry[QStringLiteral("label")] = QStringLiteral("spouse");
    relationEntry[QStringLiteral("name")] = QStringLiteral("William King");

    QVariantMap eventEntry;
    eventEntry[QStringLiteral("label")] = QStringLiteral("anniversary");
    eventEntry[QStringLiteral("date")] = QStringLiteral("2026-06-01");

    QVariantMap customFieldEntry;
    customFieldEntry[QStringLiteral("label")] = QStringLiteral("Employee ID");
    customFieldEntry[QStringLiteral("value")] = QStringLiteral("42");

    QVariantMap addressEntry;
    addressEntry[QStringLiteral("label")] = QStringLiteral("home");
    addressEntry[QStringLiteral("street")] = QStringLiteral("1 Analytical Engine Way");
    addressEntry[QStringLiteral("city")] = QStringLiteral("London");
    addressEntry[QStringLiteral("region")] = QStringLiteral("England");
    addressEntry[QStringLiteral("postalCode")] = QStringLiteral("SW1A 1AA");
    addressEntry[QStringLiteral("country")] = QStringLiteral("UK");

    QVariantMap fields;
    fields[QStringLiteral("fn")] = QStringLiteral("Ada Lovelace");
    fields[QStringLiteral("groupIds")] = QVariantList{ QStringLiteral("group-1"), QStringLiteral("group-2") };
    fields[QStringLiteral("photoRef")] = QStringLiteral("photo-ref-1");
    fields[QStringLiteral("pgpKey")] = QStringLiteral("-----BEGIN PGP PUBLIC KEY BLOCK-----");
    fields[QStringLiteral("ims")] = QVariantList{ imEntry };
    fields[QStringLiteral("websites")] = QVariantList{ websiteEntry };
    fields[QStringLiteral("relations")] = QVariantList{ relationEntry };
    fields[QStringLiteral("events")] = QVariantList{ eventEntry };
    fields[QStringLiteral("phoneticGivenName")] = QStringLiteral("Ay-da");
    fields[QStringLiteral("phoneticFamilyName")] = QStringLiteral("Love-lace");
    fields[QStringLiteral("department")] = QStringLiteral("Engineering");
    fields[QStringLiteral("customFields")] = QVariantList{ customFieldEntry };
    fields[QStringLiteral("pronouns")] = QStringLiteral("she/her");
    fields[QStringLiteral("addresses")] = QVariantList{ addressEntry };

    const QString newUid = controller.createContact(fields);
    QVERIFY(!newUid.isEmpty());

    const QVariantMap created = controller.contactAt(newUid);
    QCOMPARE(created.value(QStringLiteral("groupIds")).toStringList(),
              QStringList({ QStringLiteral("group-1"), QStringLiteral("group-2") }));
    QCOMPARE(created.value(QStringLiteral("photoRef")).toString(), QStringLiteral("photo-ref-1"));
    QCOMPARE(created.value(QStringLiteral("pgpKey")).toString(), QStringLiteral("-----BEGIN PGP PUBLIC KEY BLOCK-----"));
    QCOMPARE(created.value(QStringLiteral("phoneticGivenName")).toString(), QStringLiteral("Ay-da"));
    QCOMPARE(created.value(QStringLiteral("phoneticFamilyName")).toString(), QStringLiteral("Love-lace"));
    QCOMPARE(created.value(QStringLiteral("department")).toString(), QStringLiteral("Engineering"));
    QCOMPARE(created.value(QStringLiteral("pronouns")).toString(), QStringLiteral("she/her"));

    const QVariantList createdIms = created.value(QStringLiteral("ims")).toList();
    QCOMPARE(createdIms.size(), 1);
    QCOMPARE(createdIms.at(0).toMap().value(QStringLiteral("service")).toString(), QStringLiteral("Matrix"));
    QCOMPARE(createdIms.at(0).toMap().value(QStringLiteral("value")).toString(), QStringLiteral("@ada:example.org"));

    const QVariantList createdWebsites = created.value(QStringLiteral("websites")).toList();
    QCOMPARE(createdWebsites.size(), 1);
    QCOMPARE(createdWebsites.at(0).toMap().value(QStringLiteral("value")).toString(),
              QStringLiteral("https://ada.example.com"));

    const QVariantList createdRelations = created.value(QStringLiteral("relations")).toList();
    QCOMPARE(createdRelations.size(), 1);
    QCOMPARE(createdRelations.at(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("William King"));

    const QVariantList createdEvents = created.value(QStringLiteral("events")).toList();
    QCOMPARE(createdEvents.size(), 1);
    QCOMPARE(createdEvents.at(0).toMap().value(QStringLiteral("date")).toString(), QStringLiteral("2026-06-01"));

    const QVariantList createdCustomFields = created.value(QStringLiteral("customFields")).toList();
    QCOMPARE(createdCustomFields.size(), 1);
    QCOMPARE(createdCustomFields.at(0).toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Employee ID"));
    QCOMPARE(createdCustomFields.at(0).toMap().value(QStringLiteral("value")).toString(), QStringLiteral("42"));

    const QVariantList createdAddresses = created.value(QStringLiteral("addresses")).toList();
    QCOMPARE(createdAddresses.size(), 1);
    QCOMPARE(createdAddresses.at(0).toMap().value(QStringLiteral("street")).toString(),
              QStringLiteral("1 Analytical Engine Way"));
    QCOMPARE(createdAddresses.at(0).toMap().value(QStringLiteral("country")).toString(), QStringLiteral("UK"));

    // updateContact is a whole-value/whole-list replace for every one of
    // these fields, same as it already is for org/notes -- omitting a key
    // from `fields` clears it, it does not preserve the prior value.
    QVariantMap updateFields;
    updateFields[QStringLiteral("fn")] = QStringLiteral("Ada Lovelace");
    updateFields[QStringLiteral("groupIds")] = QVariantList{ QStringLiteral("group-3") };
    updateFields[QStringLiteral("pronouns")] = QStringLiteral("they/them");
    // photoRef/pgpKey/ims/websites/relations/events/phoneticGivenName/
    // phoneticFamilyName/department/customFields deliberately omitted.

    QVERIFY(controller.updateContact(newUid, updateFields));

    const QVariantMap updated = controller.contactAt(newUid);
    QCOMPARE(updated.value(QStringLiteral("groupIds")).toStringList(), QStringList({ QStringLiteral("group-3") }));
    QCOMPARE(updated.value(QStringLiteral("pronouns")).toString(), QStringLiteral("they/them"));
    QCOMPARE(updated.value(QStringLiteral("photoRef")).toString(), QString());
    QCOMPARE(updated.value(QStringLiteral("pgpKey")).toString(), QString());
    QVERIFY(updated.value(QStringLiteral("ims")).toList().isEmpty());
    QVERIFY(updated.value(QStringLiteral("websites")).toList().isEmpty());
    QVERIFY(updated.value(QStringLiteral("relations")).toList().isEmpty());
    QVERIFY(updated.value(QStringLiteral("events")).toList().isEmpty());
    QVERIFY(updated.value(QStringLiteral("customFields")).toList().isEmpty());
    QCOMPARE(updated.value(QStringLiteral("phoneticGivenName")).toString(), QString());
    QCOMPARE(updated.value(QStringLiteral("phoneticFamilyName")).toString(), QString());
    QCOMPARE(updated.value(QStringLiteral("department")).toString(), QString());
    QVERIFY(updated.value(QStringLiteral("addresses")).toList().isEmpty());
}

void ContactsControllerTest::allGroupsReturnsCachedGroupsAsIdNameMaps()
{
    // extended-contact-fields Task 5: allGroups() backs the edit form's
    // group-assignment checkbox list -- exercises real GroupDao-backed
    // GroupsRepository::groups() (no fake server / sync() involved, unlike
    // syncSuccessRefreshesGroupsCache above which tests the *refresh* path;
    // this tests the *read* path allGroups() adds on top of it).
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    // Empty cache (nothing synced yet) -> empty list, not a crash.
    QVERIFY(controller.allGroups().isEmpty());

    Group family;
    family.id = QStringLiteral("group-1");
    family.name = QStringLiteral("Family");
    family.rev = 1;
    QVERIFY(groupDao.insertOrReplace(family));

    Group work;
    work.id = QStringLiteral("group-2");
    work.name = QStringLiteral("Work");
    work.rev = 1;
    QVERIFY(groupDao.insertOrReplace(work));

    const QVariantList groups = controller.allGroups();
    QCOMPARE(groups.size(), 2);

    const QVariantMap first = groups.at(0).toMap();
    QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("group-1"));
    QCOMPARE(first.value(QStringLiteral("name")).toString(), QStringLiteral("Family"));

    const QVariantMap second = groups.at(1).toMap();
    QCOMPARE(second.value(QStringLiteral("id")).toString(), QStringLiteral("group-2"));
    QCOMPARE(second.value(QStringLiteral("name")).toString(), QStringLiteral("Work"));
}

void ContactsControllerTest::dedupeSuccessWithMergesChainsIntoSyncAndReloadsModel()
{
    const QByteArray dedupeResponse =
        httpResponse(200, "OK", R"({"mergedCount":1,"groups":[{"survivor":"srv-1","absorbed":["srv-2"]}]})");
    const QByteArray contactsSyncResponse = httpResponse(200, "OK",
        R"({"cursor":7,"tooOld":false,"changed":[{"uid":"srv-1","rev":2,"fn":"Ada"}],)"
        R"("deleted":[{"uid":"srv-2","rev":2}]})");
    const QByteArray groupsResponse = httpResponse(200, "OK", R"([])");
    TriplePathFakeRelayServer fake(dedupeResponse, contactsSyncResponse, groupsResponse);

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact seedOne;
    seedOne.uid = QStringLiteral("srv-1");
    seedOne.rev = 1;
    seedOne.fn = QStringLiteral("Ada");
    QVERIFY(contactDao.insertOrReplace(seedOne));

    Contact seedTwo;
    seedTwo.uid = QStringLiteral("srv-2");
    seedTwo.rev = 1;
    seedTwo.fn = QStringLiteral("Ada Duplicate");
    QVERIFY(contactDao.insertOrReplace(seedTwo));

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    controller.dedupe();
    // dedupe dispatches, then chains into a sync from its own completion
    // handler -- so the whole chain is done only once isBusy has settled.
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 10000);

    QVERIFY(fake.dedupeRequestReceived());
    QVERIFY(fake.contactsSyncRequestReceived()); // proves dedupe() chained into sync()
    QVERIFY(fake.groupsRequestReceived());       // proves sync()'s own Success->refresh() still ran

    QCOMPARE(controller.lastError(), QString());
    QVERIFY(controller.statusMessage().startsWith(QStringLiteral("Merged 1 duplicate(s)")));

    // The tombstoned loser is gone from the local cache -- reloaded via the
    // chained sync() call, not by dedupe() itself.
    QVERIFY(!contactDao.findById(QStringLiteral("srv-2")).has_value());
    QVERIFY(contactDao.findById(QStringLiteral("srv-1")).has_value());

    auto* model = qobject_cast<ContactListModel*>(controller.contactModel());
    QVERIFY(model != nullptr);
    QCOMPARE(model->rowCount(), 1);
}

void ContactsControllerTest::dedupeSuccessWithZeroMergedSkipsSyncButReloadsModel()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"mergedCount":0,"groups":[]})"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact seed;
    seed.uid = QStringLiteral("srv-1");
    seed.rev = 1;
    seed.fn = QStringLiteral("Ada");
    QVERIFY(contactDao.insertOrReplace(seed));

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    controller.dedupe();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 10000);

    QCOMPARE(controller.lastError(), QString());
    QCOMPARE(controller.statusMessage(), QStringLiteral("No duplicates found"));

    // Only the dedupe endpoint should have been hit -- no chained sync().
    QVERIFY(fake.receivedRequest().contains("POST /api/contacts/dedupe HTTP/1.1"));
    QVERIFY(!fake.receivedRequest().contains("/api/contacts/sync"));

    auto* model = qobject_cast<ContactListModel*>(controller.contactModel());
    QVERIFY(model != nullptr);
    QCOMPARE(model->rowCount(), 1); // reloaded from the untouched cache
}

void ContactsControllerTest::dedupeUnauthorizedSetsLastErrorNotStatusMessage()
{
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "Unauthorized\n"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    controller.dedupe();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 10000);

    QCOMPARE(controller.lastError(), QStringLiteral("Unauthorized -- please re-pair this device"));
    QCOMPARE(controller.statusMessage(), QString());
}

// searchContacts() is a pure in-memory filter over m_repository.contacts()
// -- none of these tests exercise networking/pairing, so each fixture below
// only seeds ContactDao directly, same minimal-setup shape as
// createContactRejectsBlankName above.

void ContactsControllerTest::searchContactsMatchesAcrossMultipleEmailsPerContact()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact ada;
    ada.uid = QStringLiteral("c-1");
    ada.fn = QStringLiteral("Ada Lovelace");
    ada.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("ada@example.com") },
                   ContactEmailEntry{ QStringLiteral("work"), QStringLiteral("ada.lovelace@work.example.com") } };
    QVERIFY(contactDao.insertOrReplace(ada));

    Contact grace;
    grace.uid = QStringLiteral("c-2");
    grace.fn = QStringLiteral("Grace Hopper");
    grace.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("grace@example.com") } };
    QVERIFY(contactDao.insertOrReplace(grace));

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);
    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    // "ada" matches both of Ada's emails -- each is its own candidate.
    const QVariantList results = controller.searchContacts(QStringLiteral("ada"), 5);
    QCOMPARE(results.size(), 2);
    QCOMPARE(results.at(0).toMap().value(QStringLiteral("uid")).toString(), QStringLiteral("c-1"));
    QCOMPARE(results.at(1).toMap().value(QStringLiteral("uid")).toString(), QStringLiteral("c-1"));
    QSet<QString> matchedEmails{ results.at(0).toMap().value(QStringLiteral("email")).toString(),
                                  results.at(1).toMap().value(QStringLiteral("email")).toString() };
    QVERIFY(matchedEmails.contains(QStringLiteral("ada@example.com")));
    QVERIFY(matchedEmails.contains(QStringLiteral("ada.lovelace@work.example.com")));
}

void ContactsControllerTest::searchContactsIsCaseInsensitiveSubstring()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact grace;
    grace.uid = QStringLiteral("c-1");
    grace.fn = QStringLiteral("Grace Hopper");
    grace.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("grace@EXAMPLE.com") } };
    QVERIFY(contactDao.insertOrReplace(grace));

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);
    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QCOMPARE(controller.searchContacts(QStringLiteral("HOPPER"), 5).size(), 1);
    QCOMPARE(controller.searchContacts(QStringLiteral("example"), 5).size(), 1);
    QVERIFY(controller.searchContacts(QStringLiteral("nomatch"), 5).isEmpty());
}

void ContactsControllerTest::searchContactsRanksPrefixMatchesFirst()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    // "Barbara Ann" only contains "ann" mid-name; "Anna Smith" starts with it.
    Contact barbara;
    barbara.uid = QStringLiteral("c-1");
    barbara.fn = QStringLiteral("Barbara Ann");
    barbara.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("barbara@example.com") } };
    QVERIFY(contactDao.insertOrReplace(barbara));

    Contact anna;
    anna.uid = QStringLiteral("c-2");
    anna.fn = QStringLiteral("Anna Smith");
    anna.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("anna@example.com") } };
    QVERIFY(contactDao.insertOrReplace(anna));

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);
    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    const QVariantList results = controller.searchContacts(QStringLiteral("ann"), 5);
    QCOMPARE(results.size(), 2);
    // Anna (prefix match) ranks before Barbara Ann (substring-elsewhere match),
    // even though Barbara was inserted first.
    QCOMPARE(results.at(0).toMap().value(QStringLiteral("uid")).toString(), QStringLiteral("c-2"));
    QCOMPARE(results.at(1).toMap().value(QStringLiteral("uid")).toString(), QStringLiteral("c-1"));
}

void ContactsControllerTest::searchContactsRespectsLimit()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    for (int i = 0; i < 10; ++i) {
        Contact c;
        c.uid = QStringLiteral("c-%1").arg(i);
        c.fn = QStringLiteral("Match Person %1").arg(i);
        c.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("match%1@example.com").arg(i) } };
        QVERIFY(contactDao.insertOrReplace(c));
    }

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);
    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QCOMPARE(controller.searchContacts(QStringLiteral("match"), 5).size(), 5);
}

void ContactsControllerTest::searchContactsEmptyQueryReturnsEverythingUpToLimit()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact ada;
    ada.uid = QStringLiteral("c-1");
    ada.fn = QStringLiteral("Ada Lovelace");
    ada.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("ada@example.com") } };
    QVERIFY(contactDao.insertOrReplace(ada));

    Contact grace;
    grace.uid = QStringLiteral("c-2");
    grace.fn = QStringLiteral("Grace Hopper");
    grace.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("grace@example.com") } };
    QVERIFY(contactDao.insertOrReplace(grace));

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);
    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QCOMPARE(controller.searchContacts(QString(), 5).size(), 2);
    QCOMPARE(controller.searchContacts(QStringLiteral("   "), 5).size(), 2); // whitespace-only trims to empty
}

void ContactsControllerTest::searchContactsZeroOrNegativeLimitIsUnbounded()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    for (int i = 0; i < 10; ++i) {
        Contact c;
        c.uid = QStringLiteral("c-%1").arg(i);
        c.fn = QStringLiteral("Match Person %1").arg(i);
        c.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("match%1@example.com").arg(i) } };
        QVERIFY(contactDao.insertOrReplace(c));
    }

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);
    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());
    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    QCOMPARE(controller.searchContacts(QStringLiteral("match"), 0).size(), 10);
    QCOMPARE(controller.searchContacts(QStringLiteral("match"), -1).size(), 10);
}


// The UI must not say "saved" over a database that refused the write. The
// contact pane closes on a successful save, so a false success here is a
// contact the user believes they have and does not.
void ContactsControllerTest::aContactThatCouldNotBeSavedIsReportedAsNotSaved()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    GroupDao groupDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- nothing here syncs

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    GroupsClient groupsClient(http);
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    ContactPhotoClient photoClient(http);
    QTemporaryDir photoCacheDir;
    QVERIFY(photoCacheDir.isValid());
    ContactPhotoCache photoCache(photoCacheDir.path());
    ContactPhotoRepository photoRepository(photoClient, photoCache, pairingStore);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore, db.handle());

    NetworkExecutor executor(3000);
    ContactsController controller(repository, groupsRepository, photoRepository, executor);
    ExecutorShutdownGuard shutdownGuard{ executor };

    // Every write fails from here on, the bluntest honest way.
    {
        QSqlQuery drop(db.handle());
        QVERIFY(drop.exec(QStringLiteral("DROP TABLE pending_contact_changes")));
    }

    QVariantMap fields;
    fields[QStringLiteral("fn")] = QStringLiteral("Ada Lovelace");

    QVERIFY2(controller.createContact(fields).isEmpty(), "a contact that was not saved came back with a uid");
    QVERIFY2(!controller.lastError().isEmpty(), "the save failed silently");
}

QTEST_GUILESS_MAIN(ContactsControllerTest)
#include "ContactsControllerTest.moc"
