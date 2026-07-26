#include "domain/MailRepository.h"

#include "domain/PgpMessageState.h"

#include "db/Database.h"
#include "db/EmailDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "models/Email.h"
#include "net/HttpClient.h"
#include "net/RelayMailSource.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"

#include "../net/FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>

class MailRepositoryTest : public QObject
{
    Q_OBJECT

private slots:
    void refreshFolderFullSnapshotReplacesFolderCache();
    void refreshFolderDeltaMergesNewUpdatedRemovedAndPersistsCursor();
    void refreshFolderMessageInTwoTabsProducesOneRowWithBothKeywords();
    void refreshFolderNotPairedReturnsNotPairedWithNoRequest();
    void refreshFolderForceFullResyncOmitsSinceAndReplacesFolderCache();
    void findCachedEmailReturnsValueWhenPresentAndNulloptWhenMissing();
    void refreshFolderUpdatedDeltaKeepsCachedBody();
    void refreshFolderUpdatedDeltaDoesNotTurnDecryptedMailIntoClientProtected();
    void refreshFolderNewDeltaWithNoBodyStaysClientProtected();

private:
    static void savePairing(PairingStore& pairingStore, quint16 port);
};

void MailRepositoryTest::savePairing(PairingStore& pairingStore, quint16 port)
{
    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("secret-1");
    pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    pairing.deviceId = QStringLiteral("dev-1");
    QVERIFY(pairingStore.save(pairing));
}

void MailRepositoryTest::refreshFolderFullSnapshotReplacesFolderCache()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    Email staleEmail;
    staleEmail.messageId = QStringLiteral("stale-1");
    staleEmail.folder = QStringLiteral("INBOX");
    staleEmail.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    QVERIFY(emailDao.insertOrReplace(staleEmail));

    // Sole byTab key is the "Uncategorized" display-only fallback tab (not a
    // real keyword) -- so this message should land with empty keywords.
    const QByteArray body = R"(
    {
      "tabs": ["Uncategorized"],
      "byTab": {
        "Uncategorized": [
          {
            "messageId": "m1",
            "sender": "alice@example.com",
            "sentTo": "bob@example.com",
            "cc": "",
            "bcc": "",
            "subject": "Hello",
            "status": "unread",
            "atUtc": "2026-07-01T12:00:00Z",
            "hasAttachments": false,
            "label": ""
          }
        ]
      }
    }
    )";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    const MailFetchOutcome outcome = repository.refreshFolder(QStringLiteral("INBOX"));
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::Success);

    // No stored cursor and no forced resync -- since must be omitted entirely.
    const QByteArray request = fake.receivedRequest();
    QVERIFY(!request.contains("since="));
    QVERIFY(request.contains("mailbox=INBOX"));

    QVERIFY(!emailDao.findById(QStringLiteral("stale-1")).has_value());
    const QVector<Email> cached = emailDao.findByFolder(QStringLiteral("INBOX"));
    QCOMPARE(cached.size(), 1);
    QCOMPARE(cached.at(0).messageId, QStringLiteral("m1"));
    QCOMPARE(cached.at(0).folder, QStringLiteral("INBOX"));
    QVERIFY(cached.at(0).keywords.isEmpty());
}

void MailRepositoryTest::refreshFolderDeltaMergesNewUpdatedRemovedAndPersistsCursor()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    Email updatedSeed;
    updatedSeed.messageId = QStringLiteral("m-updated");
    updatedSeed.folder = QStringLiteral("INBOX");
    updatedSeed.subject = QStringLiteral("Old subject");
    updatedSeed.status = QStringLiteral("unread");
    updatedSeed.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    QVERIFY(emailDao.insertOrReplace(updatedSeed));

    Email removedSeed;
    removedSeed.messageId = QStringLiteral("m-removed");
    removedSeed.folder = QStringLiteral("INBOX");
    removedSeed.atUtc = QStringLiteral("2026-01-02T00:00:00Z");
    QVERIFY(emailDao.insertOrReplace(removedSeed));

    const QByteArray body = R"(
    {
      "tabs": ["Inbox"],
      "byTab": {
        "Inbox": [
          {
            "messageId": "m-new",
            "sender": "carol@example.com",
            "sentTo": "dave@example.com",
            "cc": "",
            "bcc": "",
            "subject": "Brand new",
            "status": "unread",
            "atUtc": "2026-07-05T00:00:00Z",
            "hasAttachments": false,
            "label": "",
            "changeType": "new"
          },
          {
            "messageId": "m-updated",
            "sender": "erin@example.com",
            "sentTo": "frank@example.com",
            "cc": "",
            "bcc": "",
            "subject": "New subject",
            "status": "read",
            "atUtc": "2026-07-06T00:00:00Z",
            "hasAttachments": true,
            "label": "",
            "changeType": "updated"
          }
        ]
      },
      "delta": true,
      "cursor": 4242,
      "removed": ["m-removed"]
    }
    )";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    const MailFetchOutcome outcome = repository.refreshFolder(QStringLiteral("INBOX"));
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::Success);

    QVERIFY(emailDao.findById(QStringLiteral("m-new")).has_value());

    const std::optional<Email> updated = emailDao.findById(QStringLiteral("m-updated"));
    QVERIFY(updated.has_value());
    QCOMPARE(updated->subject, QStringLiteral("New subject"));
    QCOMPARE(updated->status, QStringLiteral("read"));
    QCOMPARE(updated->hasAttachments, true);

    QVERIFY(!emailDao.findById(QStringLiteral("m-removed")).has_value());

    QCOMPARE(cursorStore.mailCursor(), QStringLiteral("4242"));
}

void MailRepositoryTest::refreshFolderMessageInTwoTabsProducesOneRowWithBothKeywords()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    const QByteArray body = R"(
    {
      "tabs": ["Work", "Urgent", "Uncategorized"],
      "byTab": {
        "Work": [
          {
            "messageId": "m1",
            "sender": "a@example.com",
            "sentTo": "b@example.com",
            "cc": "",
            "bcc": "",
            "subject": "Both tabs",
            "status": "unread",
            "atUtc": "2026-07-01T00:00:00Z",
            "hasAttachments": false,
            "label": "Work"
          }
        ],
        "Urgent": [
          {
            "messageId": "m1",
            "sender": "a@example.com",
            "sentTo": "b@example.com",
            "cc": "",
            "bcc": "",
            "subject": "Both tabs",
            "status": "unread",
            "atUtc": "2026-07-01T00:00:00Z",
            "hasAttachments": false,
            "label": "Work"
          }
        ],
        "Uncategorized": [
          {
            "messageId": "m2",
            "sender": "c@example.com",
            "sentTo": "d@example.com",
            "cc": "",
            "bcc": "",
            "subject": "Solo",
            "status": "unread",
            "atUtc": "2026-07-02T00:00:00Z",
            "hasAttachments": false,
            "label": ""
          }
        ]
      }
    }
    )";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    const MailFetchOutcome outcome = repository.refreshFolder(QStringLiteral("INBOX"));
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::Success);

    const QVector<Email> cached = emailDao.findByFolder(QStringLiteral("INBOX"));
    QCOMPARE(cached.size(), 2);

    const std::optional<Email> m1 = emailDao.findById(QStringLiteral("m1"));
    QVERIFY(m1.has_value());
    QCOMPARE(m1->folder, QStringLiteral("INBOX"));
    QStringList expectedKeywords{ QStringLiteral("Urgent"), QStringLiteral("Work") };
    std::sort(expectedKeywords.begin(), expectedKeywords.end());
    QCOMPARE(m1->keywords, expectedKeywords);

    const std::optional<Email> m2 = emailDao.findById(QStringLiteral("m2"));
    QVERIFY(m2.has_value());
    // Only appeared under the "Uncategorized" fallback tab -- excluded from
    // keywords, matching the display-only-bucket rule.
    QVERIFY(m2->keywords.isEmpty());
}

void MailRepositoryTest::refreshFolderNotPairedReturnsNotPairedWithNoRequest()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(200, "OK", R"({"tabs":[],"byTab":{}})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    const MailFetchOutcome outcome = repository.refreshFolder(QStringLiteral("INBOX"));
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::NotPaired);
    QVERIFY(fake.receivedRequest().isEmpty());
}

void MailRepositoryTest::refreshFolderForceFullResyncOmitsSinceAndReplacesFolderCache()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    // Orphaned/stale cached email that the server's full-snapshot response
    // below does not mention at all -- a genuine full-snapshot response
    // carries no per-item "removed" list (that's a delta-only field), so the
    // only way this row can be purged is via replaceFolderSnapshot's
    // wholesale replace, not the delta-merge branch's targeted deletes.
    Email staleEmail;
    staleEmail.messageId = QStringLiteral("stale-orphan");
    staleEmail.folder = QStringLiteral("INBOX");
    staleEmail.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    QVERIFY(emailDao.insertOrReplace(staleEmail));

    // Genuine full-snapshot payload: no "delta"/"cursor"/"removed" keys,
    // matching what a real omitted-`since` request gets back.
    const QByteArray body = R"(
    {
      "tabs": ["Inbox"],
      "byTab": {
        "Inbox": [
          {
            "messageId": "fresh-1",
            "sender": "alice@example.com",
            "sentTo": "bob@example.com",
            "cc": "",
            "bcc": "",
            "subject": "Still here",
            "status": "unread",
            "atUtc": "2026-07-10T00:00:00Z",
            "hasAttachments": false,
            "label": ""
          }
        ]
      }
    }
    )";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));
    // A stored cursor exists, but forceFullResync must bypass it entirely --
    // proving the omission isn't just an accident of an empty cursor.
    cursorStore.setMailCursor(QStringLiteral("999"));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    const MailFetchOutcome outcome = repository.refreshFolder(QStringLiteral("INBOX"), /*forceFullResync=*/true);
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::Success);

    // Root-cause assertion: forceFullResync must omit `since` entirely, not
    // send since=0 -- since present at all (even 0) puts the mail endpoint
    // into delta mode per RelayMailSource's wire contract.
    const QByteArray request = fake.receivedRequest();
    QVERIFY(!request.contains("since="));
    QVERIFY(request.contains("mailbox=INBOX"));

    // Consequence assertion: the stale/orphaned row must not survive a
    // forced full resync -- it only would if this had gone through the
    // delta-merge branch instead of replaceFolderSnapshot.
    QVERIFY(!emailDao.findById(QStringLiteral("stale-orphan")).has_value());
    const QVector<Email> cached = emailDao.findByFolder(QStringLiteral("INBOX"));
    QCOMPARE(cached.size(), 1);
    QCOMPARE(cached.at(0).messageId, QStringLiteral("fresh-1"));
}

void MailRepositoryTest::findCachedEmailReturnsValueWhenPresentAndNulloptWhenMissing()
{
    // Task 35: findCachedEmail() is a pure local-cache lookup by messageId
    // (EmailDao's SQLite PRIMARY KEY), independent of folder -- unlike
    // cachedEmails(folder) above, it doesn't need the caller to already
    // know which folder the message lives in. No network call is involved,
    // so no FakeRelayServer/pairing setup is needed for this test.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    Email seed;
    seed.messageId = QStringLiteral("m-cached");
    seed.folder = QStringLiteral("INBOX");
    seed.subject = QStringLiteral("Cached subject");
    seed.sender = QStringLiteral("Alice <alice@example.com>");
    QVERIFY(emailDao.insertOrReplace(seed));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    const std::optional<Email> found = repository.findCachedEmail(QStringLiteral("m-cached"));
    QVERIFY(found.has_value());
    QCOMPARE(found->subject, QStringLiteral("Cached subject"));
    QCOMPARE(found->sender, QStringLiteral("Alice <alice@example.com>"));

    QVERIFY(!repository.findCachedEmail(QStringLiteral("no-such-id")).has_value());
}

// Regression: the backend documents that changeType == "updated" carries an
// intentionally empty Body ("the client already has it cached"). Upserting
// that row verbatim wiped the cached body, so any flag/label change blanked
// an already-opened message.
void MailRepositoryTest::refreshFolderUpdatedDeltaKeepsCachedBody()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    Email seed;
    seed.messageId = QStringLiteral("m-updated");
    seed.folder = QStringLiteral("INBOX");
    seed.subject = QStringLiteral("Old subject");
    seed.preview = QStringLiteral("Old preview");
    seed.body = QStringLiteral("<p>The real body</p>");
    seed.status = QStringLiteral("unread");
    seed.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    QVERIFY(emailDao.insertOrReplace(seed));

    const QByteArray body = R"(
    {
      "tabs": ["Inbox"],
      "byTab": {
        "Inbox": [
          {
            "messageId": "m-updated",
            "sender": "erin@example.com",
            "subject": "New subject",
            "status": "read",
            "atUtc": "2026-07-06T00:00:00Z",
            "changeType": "updated"
          }
        ]
      },
      "delta": true,
      "cursor": 99,
      "removed": []
    }
    )";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    QCOMPARE(repository.refreshFolder(QStringLiteral("INBOX")).outcome, MailRepositoryOutcome::Success);

    const std::optional<Email> stored = emailDao.findById(QStringLiteral("m-updated"));
    QVERIFY(stored.has_value());
    // The metadata the delta *did* carry must still win...
    QCOMPARE(stored->subject, QStringLiteral("New subject"));
    QCOMPARE(stored->status, QStringLiteral("read"));
    // ...while the body it deliberately omitted survives.
    QVERIFY(stored->body.has_value());
    QCOMPARE(*stored->body, QStringLiteral("<p>The real body</p>"));
    QCOMPARE(stored->preview, QStringLiteral("Old preview"));
}

// The same wipe, but on an encrypted message, is worse than a blank body:
// pgpEncrypted + no body + no error is the exact wire signature of a
// client-protected message, so it would tell a server-mode user their own
// readable mail is unreadable.
void MailRepositoryTest::refreshFolderUpdatedDeltaDoesNotTurnDecryptedMailIntoClientProtected()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    Email seed;
    seed.messageId = QStringLiteral("m-pgp");
    seed.folder = QStringLiteral("INBOX");
    seed.body = QStringLiteral("decrypted by the server, readable");
    seed.pgpEncrypted = true;
    seed.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    QVERIFY(emailDao.insertOrReplace(seed));
    QCOMPARE(pgpMessageStateOf(*emailDao.findById(QStringLiteral("m-pgp"))),
             PgpMessageState::DecryptedByServer);

    const QByteArray body = R"(
    {
      "tabs": ["Inbox"],
      "byTab": {
        "Inbox": [
          {
            "messageId": "m-pgp",
            "status": "read",
            "atUtc": "2026-07-06T00:00:00Z",
            "pgpEncrypted": true,
            "changeType": "updated"
          }
        ]
      },
      "delta": true,
      "cursor": 100,
      "removed": []
    }
    )";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    QCOMPARE(repository.refreshFolder(QStringLiteral("INBOX")).outcome, MailRepositoryOutcome::Success);

    const std::optional<Email> stored = emailDao.findById(QStringLiteral("m-pgp"));
    QVERIFY(stored.has_value());
    QCOMPARE(pgpMessageStateOf(*stored), PgpMessageState::DecryptedByServer);
}

// The mirror image: a genuinely client-protected message has no cached body
// to restore, so preservation must not invent one and must leave the
// classification alone.
void MailRepositoryTest::refreshFolderNewDeltaWithNoBodyStaysClientProtected()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    const QByteArray body = R"(
    {
      "tabs": ["Inbox"],
      "byTab": {
        "Inbox": [
          {
            "messageId": "m-sealed",
            "sender": "alice@example.com",
            "subject": "Sealed",
            "status": "unread",
            "atUtc": "2026-07-06T00:00:00Z",
            "pgpEncrypted": true,
            "changeType": "new"
          }
        ]
      },
      "delta": true,
      "cursor": 101,
      "removed": []
    }
    )";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    QCOMPARE(repository.refreshFolder(QStringLiteral("INBOX")).outcome, MailRepositoryOutcome::Success);

    const std::optional<Email> stored = emailDao.findById(QStringLiteral("m-sealed"));
    QVERIFY(stored.has_value());
    QCOMPARE(pgpMessageStateOf(*stored), PgpMessageState::ClientProtected);
}

QTEST_GUILESS_MAIN(MailRepositoryTest)
#include "MailRepositoryTest.moc"
