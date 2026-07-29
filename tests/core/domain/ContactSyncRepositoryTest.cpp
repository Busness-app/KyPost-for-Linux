#include "domain/ContactSyncRepository.h"

#include "db/ContactDao.h"
#include "db/Database.h"
#include "db/PendingContactChangeDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/ContactSyncClient.h"
#include "net/HttpClient.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"

#include "../net/FakeRelayServer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>

class ContactSyncRepositoryTest : public QObject
{
    Q_OBJECT

private slots:
    void syncWithoutPairingReturnsNotPaired();
    void fullSyncAssignsUidWithoutDuplicating();
    void serverDeleteRemovesLocalContactViaPull();
    void localDeleteOfSyncedContactSendsTombstone();
    void unsyncedLocalDeleteLeavesNoTombstone();
    void serverEditUpdatesExistingContact();
    void serverIsSelfFlagSurvivesSync();
    void serverFullSyncAppliesExtendedFields();
    void serverEditPreservesExtendedFieldsWhenOmitted();
    void tooOldResetsCursorAndCache();
    void findByUidReturnsContactWhenPresent();
    void findByUidReturnsNulloptWhenAbsent();
    void pendingUidsReflectsQueuedChanges();
    void dedupeWithoutPairingReturnsNotPaired();
    void dedupeSuccessReturnsMergedCountAndGroupsWithoutTouchingCache();
    void dedupeUnauthorizedFrom401MapsStatus();
    void dedupeServiceUnavailableFrom503MapsStatus();
    void deletingAnUnsyncedContactDropsItsLaterEditsToo();

private:
    static void savePairing(PairingStore& pairingStore, quint16 port);
};

void ContactSyncRepositoryTest::savePairing(PairingStore& pairingStore, quint16 port)
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

void ContactSyncRepositoryTest::syncWithoutPairingReturnsNotPaired()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::NotPaired);
}

void ContactSyncRepositoryTest::fullSyncAssignsUidWithoutDuplicating()
{
    const QByteArray body = R"({"cursor":456,"tooOld":false,"changed":[)"
                             R"({"uid":"srv-ada","rev":1,"fn":"Ada","emails":[{"value":"ada@example.com"}]})"
                             R"(],"deleted":[]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    Contact created;
    created.fn = QStringLiteral("Ada");
    created.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("ada@example.com") } };
    const QString tempUid = repository.queueCreate(created);
    QVERIFY(!tempUid.isEmpty());

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::Success);
    QCOMPARE(outcome.summary.pushed, 1);
    QCOMPARE(outcome.summary.applied, 1);
    QCOMPARE(outcome.summary.newCursor, qint64(456));

    QVERIFY(fake.receivedRequest().contains("POST /api/contacts/sync HTTP/1.1"));
    const QJsonObject sent = fake.receivedJsonBody();
    QCOMPARE(sent.value(QStringLiteral("baseCursor")).toInt(), 0);
    const QJsonObject sentChange = sent.value(QStringLiteral("changes")).toArray().at(0).toObject();
    QVERIFY(sentChange.contains(QStringLiteral("uid")));
    QCOMPARE(sentChange.value(QStringLiteral("uid")).toString(), QString());

    const QVector<Contact> all = contactDao.findAll();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).uid, QStringLiteral("srv-ada"));
    QVERIFY(pendingDao.findAll().isEmpty());
    QCOMPARE(cursorStore.contactBaseCursor(), QStringLiteral("456"));

    // The temp uid assigned by queueCreate() is dead once reconciliation
    // deletes its cache row -- callers (e.g. a native-contact link table)
    // need this pair to repoint themselves at the real server uid.
    QCOMPARE(outcome.uidReassignments.size(), 1);
    QCOMPARE(outcome.uidReassignments.at(0).localUid, tempUid);
    QCOMPARE(outcome.uidReassignments.at(0).serverUid, QStringLiteral("srv-ada"));
}

void ContactSyncRepositoryTest::serverDeleteRemovesLocalContactViaPull()
{
    const QByteArray body = R"({"cursor":2,"tooOld":false,"changed":[],)"
                             R"("deleted":[{"uid":"srv-1","rev":1}]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact existing;
    existing.uid = QStringLiteral("srv-1");
    existing.fn = QStringLiteral("Old");
    QVERIFY(contactDao.insertOrReplace(existing));

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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::Success);

    QVERIFY(fake.receivedRequest().contains("GET /api/contacts/sync?"));
    QVERIFY(fake.receivedRequest().contains("since=0"));

    QVERIFY(contactDao.findAll().isEmpty());
}

void ContactSyncRepositoryTest::localDeleteOfSyncedContactSendsTombstone()
{
    const QByteArray body = R"({"cursor":3,"tooOld":false,"changed":[],"deleted":[]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact existing;
    existing.uid = QStringLiteral("srv-9");
    existing.rev = 1;
    existing.fn = QStringLiteral("Grace");
    QVERIFY(contactDao.insertOrReplace(existing));

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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    repository.queueDelete(QStringLiteral("srv-9"), 1);
    QVERIFY(!contactDao.findById(QStringLiteral("srv-9")).has_value());
    QCOMPARE(pendingDao.findAll().size(), 1);

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::Success);

    QVERIFY(fake.receivedRequest().contains("POST /api/contacts/sync HTTP/1.1"));
    const QJsonObject sent = fake.receivedJsonBody();
    const QJsonObject sentChange = sent.value(QStringLiteral("changes")).toArray().at(0).toObject();
    QCOMPARE(sentChange.value(QStringLiteral("uid")).toString(), QStringLiteral("srv-9"));
    QCOMPARE(sentChange.value(QStringLiteral("deleted")).toBool(), true);

    QVERIFY(pendingDao.findAll().isEmpty());
}

void ContactSyncRepositoryTest::unsyncedLocalDeleteLeavesNoTombstone()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, 1); // no request expected -- no sync() call in this test

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    Contact created;
    created.fn = QStringLiteral("Draft Person");
    const QString tempUid = repository.queueCreate(created);
    QCOMPARE(pendingDao.findAll().size(), 1);

    repository.queueDelete(tempUid, 0);

    QVERIFY(!contactDao.findById(tempUid).has_value());
    QVERIFY(pendingDao.findAll().isEmpty());
}

void ContactSyncRepositoryTest::serverEditUpdatesExistingContact()
{
    const QByteArray body = R"({"cursor":9,"tooOld":false,"changed":[)"
                             R"({"uid":"srv-1","rev":4,"fn":"Ada L.","phones":[{"value":"555"}]})"
                             R"(],"deleted":[]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact existing;
    existing.uid = QStringLiteral("srv-1");
    existing.rev = 1;
    existing.fn = QStringLiteral("Ada");
    existing.emails = { ContactEmailEntry{ std::nullopt, QStringLiteral("ada@example.com") } };
    QVERIFY(contactDao.insertOrReplace(existing));

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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::Success);
    QCOMPARE(outcome.summary.applied, 1);

    const std::optional<Contact> updated = contactDao.findById(QStringLiteral("srv-1"));
    QVERIFY(updated.has_value());
    QCOMPARE(*updated->fn, QStringLiteral("Ada L."));
    QCOMPARE(updated->rev, qint64(4));
    QCOMPARE(updated->phones.size(), 1);
    QCOMPARE(updated->phones.at(0).value, QStringLiteral("555"));
    // The delta response carried no "emails" key at all -- the seeded email
    // must survive the merge, proving field-by-field merge rather than a
    // blind insertOrReplace(c) overwrite.
    QVERIFY(updated->emails.size() == 1);
    QCOMPARE(updated->emails.at(0).value, QStringLiteral("ada@example.com"));
}

void ContactSyncRepositoryTest::serverIsSelfFlagSurvivesSync()
{
    // Reproduces the reported bug: the server sends a full Contact with
    // isSelf/mergedUIDs/mergedInto set (ContactWire::contactFromJson parses
    // them correctly off the wire), but ContactSyncRepository::sync()'s
    // mergeContact() only copies a fixed allowlist of fields into the
    // Contact actually persisted to ContactDao -- isSelf/mergedUIDs/
    // mergedInto aren't on that list, so they're silently dropped before
    // ever reaching SQLite, even though the wire parse itself is correct.
    const QByteArray body = R"({"cursor":5,"tooOld":false,"changed":[)"
                             R"({"uid":"srv-1","rev":5,"fn":"Me","isSelf":true,)"
                             R"("mergedUIDs":["loser-1"],"mergedInto":"survivor-1"})"
                             R"(],"deleted":[]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::Success);
    QCOMPARE(outcome.summary.applied, 1);

    const std::optional<Contact> synced = contactDao.findById(QStringLiteral("srv-1"));
    QVERIFY(synced.has_value());
    QCOMPARE(synced->isSelf, true);
    QCOMPARE(synced->mergedUIDs, QVector<QString>({ QStringLiteral("loser-1") }));
    QCOMPARE(synced->mergedInto, std::optional<QString>(QStringLiteral("survivor-1")));
}

void ContactSyncRepositoryTest::serverFullSyncAppliesExtendedFields()
{
    // Reproduces the sibling bug found alongside isSelf: mergeContact()
    // predates the extended-contact-fields feature and never learned about
    // any of groupIDs/photoRef/pgpKey/ims/websites/relations/events/
    // phoneticGivenName/phoneticFamilyName/department/customFields/
    // pronouns either -- every one of them was silently dropped on every
    // sync pull, for every contact, regardless of isSelf.
    const QByteArray body = R"({"cursor":7,"tooOld":false,"changed":[)"
                             R"({"uid":"srv-1","rev":7,"fn":"Ada","groupIDs":["group-1"],)"
                             R"("photoRef":"photo-ref-1","pgpKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----",)"
                             R"("ims":[{"service":"Matrix","value":"@ada:example.org"}],)"
                             R"("websites":[{"label":"blog","value":"https://ada.example.com"}],)"
                             R"("relations":[{"label":"spouse","name":"William King"}],)"
                             R"("events":[{"label":"anniversary","date":"2026-06-01"}],)"
                             R"("phoneticGivenName":"Ay-da","phoneticFamilyName":"Love-lace",)"
                             R"("department":"Engineering",)"
                             R"("customFields":[{"label":"Employee ID","value":"42"}],)"
                             R"("pronouns":"she/her"})"
                             R"(],"deleted":[]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::Success);
    QCOMPARE(outcome.summary.applied, 1);

    const std::optional<Contact> synced = contactDao.findById(QStringLiteral("srv-1"));
    QVERIFY(synced.has_value());
    QCOMPARE(synced->groupIds, QVector<QString>({ QStringLiteral("group-1") }));
    QCOMPARE(synced->photoRef, std::optional<QString>(QStringLiteral("photo-ref-1")));
    QCOMPARE(synced->pgpKey, std::optional<QString>(QStringLiteral("-----BEGIN PGP PUBLIC KEY BLOCK-----")));
    QCOMPARE(synced->ims.size(), 1);
    QCOMPARE(synced->ims.at(0).value, QStringLiteral("@ada:example.org"));
    QCOMPARE(synced->websites.size(), 1);
    QCOMPARE(synced->websites.at(0).value, QStringLiteral("https://ada.example.com"));
    QCOMPARE(synced->relations.size(), 1);
    QCOMPARE(synced->relations.at(0).name, QStringLiteral("William King"));
    QCOMPARE(synced->events.size(), 1);
    QCOMPARE(synced->events.at(0).date, QStringLiteral("2026-06-01"));
    QCOMPARE(synced->phoneticGivenName, std::optional<QString>(QStringLiteral("Ay-da")));
    QCOMPARE(synced->phoneticFamilyName, std::optional<QString>(QStringLiteral("Love-lace")));
    QCOMPARE(synced->department, std::optional<QString>(QStringLiteral("Engineering")));
    QCOMPARE(synced->customFields.size(), 1);
    QCOMPARE(synced->customFields.at(0).value, QStringLiteral("42"));
    QCOMPARE(synced->pronouns, std::optional<QString>(QStringLiteral("she/her")));
}

void ContactSyncRepositoryTest::serverEditPreservesExtendedFieldsWhenOmitted()
{
    // Mirrors serverEditUpdatesExistingContact's emails-preservation check,
    // extended to every extended-contact-fields field: a delta response
    // that omits them entirely must not wipe out the locally-cached values.
    const QByteArray body = R"({"cursor":9,"tooOld":false,"changed":[)"
                             R"({"uid":"srv-1","rev":4,"fn":"Ada L."})"
                             R"(],"deleted":[]})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact existing;
    existing.uid = QStringLiteral("srv-1");
    existing.rev = 1;
    existing.fn = QStringLiteral("Ada");
    existing.groupIds = { QStringLiteral("group-1") };
    existing.photoRef = QStringLiteral("photo-ref-1");
    existing.pgpKey = QStringLiteral("-----BEGIN PGP PUBLIC KEY BLOCK-----");
    existing.ims = { ContactImEntry{ QStringLiteral("Matrix"), std::nullopt, QStringLiteral("@ada:example.org") } };
    existing.websites = { ContactUrlEntry{ QStringLiteral("blog"), QStringLiteral("https://ada.example.com") } };
    existing.relations = { ContactRelationEntry{ QStringLiteral("spouse"), QStringLiteral("William King") } };
    existing.events = { ContactEventEntry{ QStringLiteral("anniversary"), QStringLiteral("2026-06-01") } };
    existing.phoneticGivenName = QStringLiteral("Ay-da");
    existing.phoneticFamilyName = QStringLiteral("Love-lace");
    existing.department = QStringLiteral("Engineering");
    existing.customFields = { ContactCustomFieldEntry{ QStringLiteral("Employee ID"), QStringLiteral("42") } };
    existing.pronouns = QStringLiteral("she/her");
    QVERIFY(contactDao.insertOrReplace(existing));

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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::Success);
    QCOMPARE(outcome.summary.applied, 1);

    const std::optional<Contact> updated = contactDao.findById(QStringLiteral("srv-1"));
    QVERIFY(updated.has_value());
    QCOMPARE(*updated->fn, QStringLiteral("Ada L."));
    QCOMPARE(updated->groupIds, QVector<QString>({ QStringLiteral("group-1") }));
    QCOMPARE(updated->photoRef, std::optional<QString>(QStringLiteral("photo-ref-1")));
    QCOMPARE(updated->pgpKey, std::optional<QString>(QStringLiteral("-----BEGIN PGP PUBLIC KEY BLOCK-----")));
    QCOMPARE(updated->ims.size(), 1);
    QCOMPARE(updated->websites.size(), 1);
    QCOMPARE(updated->relations.size(), 1);
    QCOMPARE(updated->events.size(), 1);
    QCOMPARE(updated->phoneticGivenName, std::optional<QString>(QStringLiteral("Ay-da")));
    QCOMPARE(updated->phoneticFamilyName, std::optional<QString>(QStringLiteral("Love-lace")));
    QCOMPARE(updated->department, std::optional<QString>(QStringLiteral("Engineering")));
    QCOMPARE(updated->customFields.size(), 1);
    QCOMPARE(updated->pronouns, std::optional<QString>(QStringLiteral("she/her")));
}

void ContactSyncRepositoryTest::tooOldResetsCursorAndCache()
{
    const QByteArray body = R"({"cursor":0,"tooOld":true})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact existing;
    existing.uid = QStringLiteral("srv-1");
    existing.fn = QStringLiteral("Stale");
    QVERIFY(contactDao.insertOrReplace(existing));

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursors.ini")));
    cursorStore.setContactBaseCursor(QStringLiteral("99"));
    cursorStore.setMailCursor(QStringLiteral("12345")); // unrelated -- must survive

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactSyncOutcome outcome = repository.sync();
    QCOMPARE(outcome.status, ContactSyncStatus::Success);
    QCOMPARE(outcome.summary.applied, 0);
    QCOMPARE(outcome.summary.newCursor, qint64(0));

    QVERIFY(contactDao.findAll().isEmpty());
    QVERIFY(cursorStore.contactBaseCursor().isEmpty());
    // Proves CursorStore::reset() was correctly not used -- a contacts-only
    // tooOld response has nothing to do with mail sync.
    QCOMPARE(cursorStore.mailCursor(), QStringLiteral("12345"));
}

void ContactSyncRepositoryTest::findByUidReturnsContactWhenPresent()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact existing;
    existing.uid = QStringLiteral("srv-1");
    existing.fn = QStringLiteral("Grace");
    QVERIFY(contactDao.insertOrReplace(existing));

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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const std::optional<Contact> found = repository.findByUid(QStringLiteral("srv-1"));
    QVERIFY(found.has_value());
    QCOMPARE(*found->fn, QStringLiteral("Grace"));
}

void ContactSyncRepositoryTest::findByUidReturnsNulloptWhenAbsent()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    QVERIFY(!repository.findByUid(QStringLiteral("does-not-exist")).has_value());
}

void ContactSyncRepositoryTest::pendingUidsReflectsQueuedChanges()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    // Untouched uid: not pending.
    QVERIFY(!repository.isPending(QStringLiteral("never-touched")));
    QVERIFY(repository.pendingUids().isEmpty());

    // queueCreate() enqueues a pending row under its assigned temp uid.
    Contact fresh;
    fresh.fn = QStringLiteral("New Contact");
    const QString tempUid = repository.queueCreate(fresh);
    QVERIFY(repository.isPending(tempUid));
    QVERIFY(repository.pendingUids().contains(tempUid));

    // queueUpdate() on an already-synced (already-in-contactDao, no prior
    // pending row) contact enqueues a pending row for its real uid too.
    Contact existing;
    existing.uid = QStringLiteral("srv-1");
    existing.rev = 5;
    existing.fn = QStringLiteral("Existing");
    QVERIFY(contactDao.insertOrReplace(existing));
    QVERIFY(!repository.isPending(QStringLiteral("srv-1")));

    existing.fn = QStringLiteral("Existing, Edited");
    repository.queueUpdate(existing);
    QVERIFY(repository.isPending(QStringLiteral("srv-1")));
    QCOMPARE(repository.pendingUids().size(), 2);

    // pendingDao.deleteAll() is what sync() calls on success -- simulate
    // that directly here since this test doesn't need a live server round
    // trip to prove the pending-uid bookkeeping itself.
    pendingDao.deleteAll();
    QVERIFY(!repository.isPending(tempUid));
    QVERIFY(!repository.isPending(QStringLiteral("srv-1")));
    QVERIFY(repository.pendingUids().isEmpty());
}

void ContactSyncRepositoryTest::dedupeWithoutPairingReturnsNotPaired()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactDedupeOutcome outcome = repository.dedupe();
    QCOMPARE(outcome.status, ContactDedupeStatus::NotPaired);
}

void ContactSyncRepositoryTest::dedupeSuccessReturnsMergedCountAndGroupsWithoutTouchingCache()
{
    FakeRelayServer fake(httpResponse(
        200, "OK", R"({"mergedCount":1,"groups":[{"survivor":"srv-1","absorbed":["srv-2"]}]})"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    Contact existing;
    existing.uid = QStringLiteral("srv-1");
    existing.fn = QStringLiteral("Ada");
    QVERIFY(contactDao.insertOrReplace(existing));

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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactDedupeOutcome outcome = repository.dedupe();
    QCOMPARE(outcome.status, ContactDedupeStatus::Success);
    QCOMPARE(outcome.mergedCount, 1);
    QCOMPARE(outcome.groups.size(), 1);
    QCOMPARE(outcome.groups.at(0).survivor, QStringLiteral("srv-1"));
    QCOMPARE(outcome.groups.at(0).absorbed, (QVector<QString>{QStringLiteral("srv-2")}));

    QVERIFY(fake.receivedRequest().contains("POST /api/contacts/dedupe HTTP/1.1"));

    // dedupe() must not touch the local cache -- that's sync()'s job on a
    // subsequent call.
    const QVector<Contact> all = contactDao.findAll();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).uid, QStringLiteral("srv-1"));
    QCOMPARE(*all.at(0).fn, QStringLiteral("Ada"));
}

void ContactSyncRepositoryTest::dedupeUnauthorizedFrom401MapsStatus()
{
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "Unauthorized\n"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactDedupeOutcome outcome = repository.dedupe();
    QCOMPARE(outcome.status, ContactDedupeStatus::Unauthorized);
}

void ContactSyncRepositoryTest::dedupeServiceUnavailableFrom503MapsStatus()
{
    FakeRelayServer fake(httpResponse(503, "Service Unavailable", "down\n"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
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

    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    const ContactDedupeOutcome outcome = repository.dedupe();
    QCOMPARE(outcome.status, ContactDedupeStatus::ServiceUnavailable);
}


// queueDelete() cancelled only the pending record whose WIRE uid was empty --
// the create. An edit saved after the create carries the temporary uid, so it
// survived and was pushed on the next sync as a change naming a uid the
// server had never seen. The relay treats that as a create under that uid, so
// the contact the user deleted was recreated account-wide and echoed straight
// back into the local database.
void ContactSyncRepositoryTest::deletingAnUnsyncedContactDropsItsLaterEditsToo()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    ContactDao contactDao(db.handle());
    PendingContactChangeDao pendingDao(db.handle());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactSyncClient client(http);
    ContactSyncRepository repository(client, contactDao, pendingDao, cursorStore, pairingStore);

    Contact created;
    created.fn = QStringLiteral("Temp Person");
    const QString tempUid = repository.queueCreate(created);
    QVERIFY(!tempUid.isEmpty());

    // The user reopens it and fixes a typo before ever syncing.
    Contact edited = created;
    edited.uid = tempUid;
    edited.org = QStringLiteral("Acme");
    repository.queueUpdate(edited);
    QCOMPARE(pendingDao.findAll().size(), 2);

    // ...then decides to remove it.
    repository.queueDelete(tempUid, 0);

    // Nothing queued survives: no create, no edit, and no tombstone for a
    // contact the server never heard of.
    QCOMPARE(pendingDao.findAll().size(), 0);
    QVERIFY(contactDao.findAll().isEmpty());
}

QTEST_GUILESS_MAIN(ContactSyncRepositoryTest)
#include "ContactSyncRepositoryTest.moc"

