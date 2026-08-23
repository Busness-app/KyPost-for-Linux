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

#include <QDir>
#include <QNetworkAccessManager>
#include <QSqlQuery>
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
    void refreshFolderForceFullResyncSendsSinceZeroAndReplacesFolderCache();
    void refreshFolderDeltaRemovedOnlyEvictsTheRequestedFolder();
    void refreshFolderUsesTheCursorForThisFolderNotAnotherFolders();
    void findCachedEmailRefusesAMessageIdCachedInMoreThanOneFolder();
    void findCachedEmailReturnsValueWhenPresentAndNulloptWhenMissing();
    void refreshFolderUpdatedDeltaKeepsCachedBody();
    void refreshFolderUpdatedDeltaDoesNotTurnDecryptedMailIntoClientProtected();
    void refreshFolderNewDeltaWithNoBodyStaysClientProtected();
    void planRefreshReturnsNothingWhenUnpairedAndCarriesTheStoredCursorOtherwise();
    void applyRefreshPersistsTheSameRowsWithNoNetwork();
    void applyRefreshHoldsTheCursorBackWhenTheCacheWriteFails();
    void applyRefreshReportsFailureWhenTheCursorCannotBePersisted();
    void applyRefreshDiscardsAReplyTheCurrentPairingDidNotAuthorise();

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

    // No stored cursor and no forced resync: `since=0`, NOT omitted. Sending
    // `since` at all is what selects the relay's cursor protocol, and the
    // cursor it returns is the only way this client ever acquires one. This
    // assertion used to read `QVERIFY(!request.contains("since="))`, which
    // pinned the bug in place: omitting it took the classic path, which
    // returns no cursor, so the next refresh had none either -- forever.
    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("since=0"));
    QVERIFY(request.contains("mailbox=INBOX"));

    QVERIFY(!emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("stale-1")).has_value());
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

    QVERIFY(emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-new")).has_value());

    const std::optional<Email> updated = emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-updated"));
    QVERIFY(updated.has_value());
    QCOMPARE(updated->subject, QStringLiteral("New subject"));
    QCOMPARE(updated->status, QStringLiteral("read"));
    QCOMPARE(updated->hasAttachments, true);

    QVERIFY(!emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-removed")).has_value());

    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")), QStringLiteral("4242"));
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

    const std::optional<Email> m1 = emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m1"));
    QVERIFY(m1.has_value());
    QCOMPARE(m1->folder, QStringLiteral("INBOX"));
    QStringList expectedKeywords{ QStringLiteral("Urgent"), QStringLiteral("Work") };
    std::sort(expectedKeywords.begin(), expectedKeywords.end());
    QCOMPARE(m1->keywords, expectedKeywords);

    const std::optional<Email> m2 = emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m2"));
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

void MailRepositoryTest::refreshFolderForceFullResyncSendsSinceZeroAndReplacesFolderCache()
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

    // Full-window payload as the cursor protocol reports it: delta=false
    // (because since==0), plus the cursor this fetch establishes.
    const QByteArray body = R"(
    {
      "delta": false,
      "cursor": 5150,
      "removed": [],
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
    // A stored cursor exists, but forceFullResync must ignore it and ask for
    // the whole window -- proving since=0 isn't just an accident of an empty
    // cursor.
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("999")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    const MailFetchOutcome outcome = repository.refreshFolder(QStringLiteral("INBOX"), /*forceFullResync=*/true);
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::Success);

    // Root-cause assertion, inverted from what it used to say. A full window
    // is `since=0`, not an omitted `since`: kypost-server's serveInbox reads
    // `cursorSync := TrimSpace(query "since") != ""` and answers `"delta":
    // since > 0`, so 0 asks for the cursor protocol AND a complete window.
    // Omitting it took the classic path, which returns no cursor at all.
    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("since=0"));
    QVERIFY(!request.contains("since=999"));
    QVERIFY(request.contains("mailbox=INBOX"));

    // Consequence assertion: the stale/orphaned row must not survive a
    // forced full resync -- it only would if this had gone through the
    // delta-merge branch instead of replaceFolderSnapshot.
    QVERIFY(!emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("stale-orphan")).has_value());
    const QVector<Email> cached = emailDao.findByFolder(QStringLiteral("INBOX"));
    QCOMPARE(cached.size(), 1);
    QCOMPARE(cached.at(0).messageId, QStringLiteral("fresh-1"));

    // The cursor the full window returned replaces the stale one. Persisting
    // it only on the delta branch is what stranded this client on the classic
    // path: a full window advanced nothing, so the next refresh was full too.
    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")),
             QStringLiteral("5150"));
}

void MailRepositoryTest::findCachedEmailReturnsValueWhenPresentAndNulloptWhenMissing()
{
    // findCachedEmail() is a pure local-cache lookup for callers that have
    // only a messageId (a desktop notification's payload carries no mailbox).
    // No network call is involved, so no FakeRelayServer/pairing setup is
    // needed. The ambiguous case has its own test below.
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

    const std::optional<Email> stored = emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-updated"));
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
    QCOMPARE(pgpMessageStateOf(*emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-pgp"))),
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

    const std::optional<Email> stored = emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-pgp"));
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

    const std::optional<Email> stored = emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-sealed"));
    QVERIFY(stored.has_value());
    QCOMPARE(pgpMessageStateOf(*stored), PgpMessageState::ClientProtected);
}

// Phase 1 is the only phase that reads the thread-confined stores, so it is
// the only phase that can answer "not paired" -- and it must carry the stored
// cursor across the thread hop, because the executor thread cannot read
// CursorStore itself.
void MailRepositoryTest::planRefreshReturnsNothingWhenUnpairedAndCarriesTheStoredCursorOtherwise()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("4242")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    // Unpaired: no plan, so the caller never dispatches anything.
    QVERIFY(!repository.planRefresh(QStringLiteral("INBOX"), false).has_value());

    savePairing(pairingStore, 1); // port is irrelevant, nothing is sent here

    const std::optional<MailRefreshPlan> delta = repository.planRefresh(QStringLiteral("INBOX"), false);
    QVERIFY(delta.has_value());
    QCOMPARE(delta->folder, QStringLiteral("INBOX"));
    QCOMPARE(delta->endpoint.auth.deviceId, QStringLiteral("dev-1"));
    QCOMPARE(delta->since, qint64(4242));
    // The plan carries the cursor's OWNER too: applyRefresh files the returned
    // cursor back under it rather than re-reading the pairing, which can be
    // cleared while the request is in flight.
    QCOMPARE(delta->subscriberId, QStringLiteral("sub-1"));

    // forceFullResync sends since=0. This assertion used to require `since`
    // be absent; see refreshFolder()'s header for why that was wrong and what
    // it cost.
    const std::optional<MailRefreshPlan> full = repository.planRefresh(QStringLiteral("INBOX"), true);
    QVERIFY(full.has_value());
    QCOMPARE(full->since, qint64(0));

    // A folder this subscriber has no cursor for is since=0 as well -- the
    // same full-window request, reached without the caller forcing it.
    const std::optional<MailRefreshPlan> other = repository.planRefresh(QStringLiteral("Archive"), false);
    QVERIFY(other.has_value());
    QCOMPARE(other->since, qint64(0));
}

// Phase 3 does the delta merge and every database write. Proving it needs no
// NETWORK is what makes it safe to run from a completion handler: the reply is
// already in hand.
//
// It does read PairingStore, once, and this test used to assert that it did
// not. That assertion conflated two different reads. Re-reading the store for
// a VALUE to use -- the cursor's owner, say -- is the TOCTOU it was written
// against, and the plan still carries every such value. Reading it to COMPARE
// against the identity the plan captured is the opposite: it is the only way
// to notice that the account these rows belong to is no longer the account
// this device is paired to. See applyRefreshDiscardsAReplyTheCurrentPairing-
// DidNotAuthorise below.
void MailRepositoryTest::applyRefreshPersistsTheSameRowsWithNoNetwork()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, 1); // port is irrelevant -- nothing is sent in this test

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    // Hand-built rather than fetched: there is no server in this test, which
    // is the point.
    Email work;
    work.messageId = QStringLiteral("m1");
    work.folder = QStringLiteral("Work"); // the tab name, as RelayMailSource stamps it
    work.atUtc = QStringLiteral("2026-07-01T00:00:00Z");

    InboxFetchResult result;
    result.tabs = QStringList{ QStringLiteral("Work"), QStringLiteral("Uncategorized") };
    result.byTab[QStringLiteral("Work")] = QVector<InboxEmailItem>{ InboxEmailItem{ work, QString(), std::nullopt } };
    result.isDelta = true;
    result.cursor = 99;

    MailRefreshPlan plan;
    plan.folder = QStringLiteral("INBOX");
    // The plan names the cursor's owner. applyRefresh files the returned
    // cursor under it rather than re-reading PairingStore, which can be
    // cleared while the request is in flight -- a cursor written under an
    // empty subscriber id is one that is never found again.
    plan.subscriberId = QStringLiteral("sub-1");
    // Together with subscriberId this is the identity that authorised the
    // request, and it matches what savePairing() stored, so the reply is
    // still ours and gets written.
    plan.endpoint.auth.deviceId = QStringLiteral("dev-1");

    QCOMPARE(repository.applyRefresh(plan, result).outcome, MailRepositoryOutcome::Success);

    // Both wire-mapping fixes this layer owns still happen: folder corrected
    // to the requested mailbox, keywords populated from the tab bucket.
    const std::optional<Email> stored = emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m1"));
    QVERIFY(stored.has_value());
    QCOMPARE(stored->folder, QStringLiteral("INBOX"));
    QCOMPARE(stored->keywords, QStringList{ QStringLiteral("Work") });
    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")), QStringLiteral("99"));

    // A transport error is reported without touching the cache at all.
    InboxFetchResult failed;
    failed.error = NetworkError::ServiceUnavailable;
    failed.detail = QStringLiteral("relay down");
    const MailFetchOutcome outcome = repository.applyRefresh(plan, failed);
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::ServiceUnavailable);
    QCOMPARE(outcome.detail, QStringLiteral("relay down"));
    QVERIFY(emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m1")).has_value());
}


// The relay's `removed` list is the diff for the mailbox the request named.
// An unscoped DELETE evicted the same messageId from every other folder too,
// so archiving a message (which removes it from INBOX's window) deleted the
// Archive copy the very same sync had just cached.
void MailRepositoryTest::refreshFolderDeltaRemovedOnlyEvictsTheRequestedFolder()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    // The same message, cached under two mailboxes -- which migration 006's
    // composite PRIMARY KEY is what makes representable at all.
    Email inboxCopy;
    inboxCopy.messageId = QStringLiteral("m-shared");
    inboxCopy.folder = QStringLiteral("INBOX");
    inboxCopy.subject = QStringLiteral("Shared");
    QVERIFY(emailDao.insertOrReplace(inboxCopy));

    Email archiveCopy = inboxCopy;
    archiveCopy.folder = QStringLiteral("Archive");
    QVERIFY(emailDao.insertOrReplace(archiveCopy));

    QCOMPARE(emailDao.findAll().size(), 2);

    const QByteArray body = R"(
    {
      "delta": true,
      "cursor": 77,
      "removed": ["m-shared"],
      "tabs": ["Uncategorized"],
      "byTab": { "Uncategorized": [] }
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
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("70")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    QCOMPARE(repository.refreshFolder(QStringLiteral("INBOX")).outcome, MailRepositoryOutcome::Success);

    QVERIFY(!emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-shared")).has_value());
    QVERIFY(emailDao.findById(QStringLiteral("Archive"), QStringLiteral("m-shared")).has_value());
}

// One cursor for the whole account asked Archive for a diff against a window
// only INBOX ever had. The relay keys its cursor per mailbox, so the client
// must too.
void MailRepositoryTest::refreshFolderUsesTheCursorForThisFolderNotAnotherFolders()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    const QByteArray body = R"(
    {
      "delta": false,
      "cursor": 31337,
      "removed": [],
      "tabs": ["Uncategorized"],
      "byTab": { "Uncategorized": [] }
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
    // INBOX is well ahead; Archive has never synced.
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("900")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    QCOMPARE(repository.refreshFolder(QStringLiteral("Archive")).outcome, MailRepositoryOutcome::Success);

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("mailbox=Archive"));
    QVERIFY(request.contains("since=0"));
    QVERIFY(!request.contains("since=900"));

    // Each folder's cursor advances on its own; Archive's arrival must not
    // stamp over INBOX's.
    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive")),
             QStringLiteral("31337"));
    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")),
             QStringLiteral("900"));
}

// A notification payload names a messageId and no mailbox. Once the same id
// can be cached under several folders, "open that message" has no single
// answer, and picking one silently opens the wrong copy.
void MailRepositoryTest::findCachedEmailRefusesAMessageIdCachedInMoreThanOneFolder()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    Email inboxCopy;
    inboxCopy.messageId = QStringLiteral("m-dup");
    inboxCopy.folder = QStringLiteral("INBOX");
    inboxCopy.subject = QStringLiteral("Inbox copy");
    QVERIFY(emailDao.insertOrReplace(inboxCopy));

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

    // One folder: unambiguous, so it resolves.
    const std::optional<Email> single = repository.findCachedEmail(QStringLiteral("m-dup"));
    QVERIFY(single.has_value());
    QCOMPARE(single->subject, QStringLiteral("Inbox copy"));

    Email archiveCopy = inboxCopy;
    archiveCopy.folder = QStringLiteral("Archive");
    archiveCopy.subject = QStringLiteral("Archive copy");
    QVERIFY(emailDao.insertOrReplace(archiveCopy));

    // Two folders: refuse, rather than return whichever row SQLite yields.
    QVERIFY(!repository.findCachedEmail(QStringLiteral("m-dup")).has_value());

    // A caller that knows the mailbox is unaffected.
    QCOMPARE(repository.cachedEmail(QStringLiteral("Archive"), QStringLiteral("m-dup"))->subject,
             QStringLiteral("Archive copy"));
    QCOMPARE(repository.cachedEmail(QStringLiteral("INBOX"), QStringLiteral("m-dup"))->subject,
             QStringLiteral("Inbox copy"));
}


// The cursor is a promise that everything up to it has been applied. Advance
// it past rows that failed to write and the relay never mentions them again:
// the messages are gone from this device permanently, with no error the user
// ever saw. So a failed write must hold the cursor AND report itself.
void MailRepositoryTest::applyRefreshHoldsTheCursorBackWhenTheCacheWriteFails()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, 1); // still paired -- the cache write is what fails here

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("10")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    // Make every write fail, the bluntest honest way: take the table away.
    {
        QSqlQuery drop(db.handle());
        QVERIFY(drop.exec(QStringLiteral("DROP TABLE emails")));
    }

    Email incoming;
    incoming.messageId = QStringLiteral("m-lost");
    incoming.folder = QStringLiteral("INBOX");
    incoming.atUtc = QStringLiteral("2026-07-01T00:00:00Z");

    InboxFetchResult result;
    result.tabs = QStringList{ QStringLiteral("Uncategorized") };
    result.byTab[QStringLiteral("Uncategorized")] =
        QVector<InboxEmailItem>{ InboxEmailItem{ incoming, QString(), std::nullopt } };
    result.isDelta = true;
    result.cursor = 500;

    MailRefreshPlan plan;
    plan.folder = QStringLiteral("INBOX");
    plan.subscriberId = QStringLiteral("sub-1");
    plan.endpoint.auth.deviceId = QStringLiteral("dev-1");

    const MailFetchOutcome outcome = repository.applyRefresh(plan, result);
    QCOMPARE(outcome.outcome, MailRepositoryOutcome::CacheWriteFailed);
    // No English sentence from core/: the wording belongs to app/.
    QVERIFY(outcome.detail.isEmpty());

    // The cursor did NOT move, so the next refresh asks for this window again.
    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")),
             QStringLiteral("10"));

    // Same rule on the full-window branch, where the failure mode is worse:
    // replaceFolderSnapshot deletes before it inserts.
    InboxFetchResult fullWindow = result;
    fullWindow.isDelta = false;
    fullWindow.cursor = 600;
    QCOMPARE(repository.applyRefresh(plan, fullWindow).outcome, MailRepositoryOutcome::CacheWriteFailed);
    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")),
             QStringLiteral("10"));
}

// The reply outlives the pairing that asked for it.
//
// Nothing in this schema records WHICH account a cached row belongs to -- the
// emails table is per-device, keyed (folder, message_id). So a refresh that
// was authorised by account A and lands after the device has been re-paired
// to account B writes A's mail into B's inbox, where B reads it. Pairing a
// replacement account purges the caches, but the purge happens on the
// registration reply and this write happens on the refresh reply; whichever
// order they arrive in, the purge cannot clean up a write that comes after it.
//
// Three ways for the identity to move, all of which must discard:
// a different subscriber, the same subscriber re-registered, and no pairing
// at all.
void MailRepositoryTest::applyRefreshDiscardsAReplyTheCurrentPairingDidNotAuthorise()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

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

    // The reply account A's refresh got back, with a body: exactly what must
    // not become readable to whoever pairs next.
    Email confidential;
    confidential.messageId = QStringLiteral("m-private");
    confidential.folder = QStringLiteral("INBOX");
    confidential.subject = QStringLiteral("Salary review");
    confidential.body = QStringLiteral("Do not show this to the next user.");
    confidential.atUtc = QStringLiteral("2026-07-01T00:00:00Z");

    InboxFetchResult result;
    result.tabs = QStringList{ QStringLiteral("Uncategorized") };
    result.byTab[QStringLiteral("Uncategorized")] =
        QVector<InboxEmailItem>{ InboxEmailItem{ confidential, QString(), std::nullopt } };
    result.isDelta = true;
    result.cursor = 777;

    // The plan account A built before its request went out.
    MailRefreshPlan plan;
    plan.folder = QStringLiteral("INBOX");
    plan.subscriberId = QStringLiteral("sub-1");
    plan.endpoint.auth.deviceId = QStringLiteral("dev-1");

    const auto assertDiscarded = [&](const char* what) {
        const MailFetchOutcome outcome = repository.applyRefresh(plan, result);
        QCOMPARE(outcome.outcome, MailRepositoryOutcome::PairingChanged);
        // core/ owns the value, app/ owns the wording (AGENTS.md 6c).
        QVERIFY(outcome.detail.isEmpty());
        QVERIFY2(!emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-private")).has_value(), what);
        // The cursor is account A's too. Filing it would make the NEW
        // account's first sync ask for a delta against a window it never had.
        QVERIFY(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    };

    // 1. A different account was paired while the request was out.
    DevicePairing replacement;
    replacement.subscriberId = QStringLiteral("sub-2");
    replacement.deviceId = QStringLiteral("dev-2");
    replacement.deviceSecret = QStringLiteral("secret-2");
    replacement.serverBaseUrl = QStringLiteral("http://127.0.0.1:1");
    QVERIFY(pairingStore.save(replacement));
    assertDiscarded("a different account's mail was written into the new account's inbox");

    // 2. The SAME account, re-registered. A fresh registration mints a new
    // deviceId, and the previous registration's reply has no claim on it.
    DevicePairing reregistered;
    reregistered.subscriberId = QStringLiteral("sub-1");
    reregistered.deviceId = QStringLiteral("dev-1-new");
    reregistered.deviceSecret = QStringLiteral("secret-3");
    reregistered.serverBaseUrl = QStringLiteral("http://127.0.0.1:1");
    QVERIFY(pairingStore.save(reregistered));
    assertDiscarded("a superseded registration's reply was still written");

    // 3. Unpaired outright. There is no account to file anything under.
    QVERIFY(pairingStore.clear());
    assertDiscarded("a reply was written into an unpaired profile");

    // And the control: restore the identity the plan named, and the very same
    // reply is applied. Without this the three assertions above would pass on
    // a repository that had simply stopped writing anything.
    DevicePairing original;
    original.subscriberId = QStringLiteral("sub-1");
    original.deviceId = QStringLiteral("dev-1");
    original.deviceSecret = QStringLiteral("secret-1");
    original.serverBaseUrl = QStringLiteral("http://127.0.0.1:1");
    QVERIFY(pairingStore.save(original));
    QCOMPARE(repository.applyRefresh(plan, result).outcome, MailRepositoryOutcome::Success);
    QVERIFY(emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-private")).has_value());
    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")), QStringLiteral("777"));
}


// The other half of the same rule. The rows land, the cursor file refuses the
// write: QSettings keeps the new value in memory and says nothing, so this
// session behaves as though the cursor moved while the next launch reads the
// old one. Reporting the failure costs one redundant refresh whose rows
// upsert over themselves.
void MailRepositoryTest::applyRefreshReportsFailureWhenTheCursorCannotBePersisted()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, 1); // still paired -- the cursor write is what fails

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    const QString blocked = cursorDir.filePath(QStringLiteral("cursor.ini"));
    QVERIFY(QDir().mkpath(blocked)); // a directory here: no file can be written
    CursorStore cursorStore(blocked);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    MailRepository repository(source, emailDao, pairingStore, cursorStore);

    Email incoming;
    incoming.messageId = QStringLiteral("m-1");
    incoming.folder = QStringLiteral("INBOX");
    incoming.atUtc = QStringLiteral("2026-07-01T00:00:00Z");

    InboxFetchResult result;
    result.tabs = QStringList{ QStringLiteral("Uncategorized") };
    result.byTab[QStringLiteral("Uncategorized")] =
        QVector<InboxEmailItem>{ InboxEmailItem{ incoming, QString(), std::nullopt } };
    result.isDelta = true;
    result.cursor = 500;

    MailRefreshPlan plan;
    plan.folder = QStringLiteral("INBOX");
    plan.subscriberId = QStringLiteral("sub-1");
    plan.endpoint.auth.deviceId = QStringLiteral("dev-1");

    QCOMPARE(repository.applyRefresh(plan, result).outcome, MailRepositoryOutcome::CacheWriteFailed);
    // The message itself did land: the next refresh re-fetches this window
    // and writes the same row again, which is the harmless direction.
    QVERIFY(emailDao.findById(QStringLiteral("INBOX"), QStringLiteral("m-1")).has_value());
}

QTEST_GUILESS_MAIN(MailRepositoryTest)
#include "MailRepositoryTest.moc"
