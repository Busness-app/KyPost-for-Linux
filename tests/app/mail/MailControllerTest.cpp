#include "mail/MailController.h"

#include "db/Database.h"
#include "db/EmailDao.h"
#include "domain/DevicePairing.h"
#include "domain/KeywordRepository.h"
#include "db/FolderDao.h"
#include "domain/FolderRepository.h"
#include "domain/MailRepository.h"
#include "net/FolderClient.h"
#include "domain/PairingStore.h"
#include "mail/EmailListModel.h"
#include "net/HttpClient.h"
#include "net/PgpBootstrapClient.h"
#include "net/PgpRecipientChecker.h"
#include "net/RelayMailSource.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include "../../core/net/FakeRelayServer.h"

#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

class MailControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void selectKeywordFiltersCachedEmailsWithoutAnyNetworkCall();
    void archiveEmailsNotPairedShortCircuitsWithNoNetworkCall();
    void sendMailOverAttachmentCapRejectsBeforeAnyNetworkCall();
    void sendMailUsesHtmlSendMode();
    void downloadAttachmentSanitizesPathTraversalInSuggestedName();
    void downloadAttachmentSanitizesPathTraversalInServerFilename();
    void findByMessageIdReturnsMapForCachedEmailAndEmptyMapWhenMissing();
    void allKeywordSettingsReflectsInboxCacheAndSetKeywordVisibleRoundTrips();
    void sendMailEmitsPickupFallbackRequiredWithTheServersAddressList();
    void confirmPickupFallbackSendResendsTheIdenticalBodyWithTheOptIn();
    void confirmPickupFallbackSendWithoutAPendingSendDoesNothing();
    void sendMailSurfacesAWarningOnAnOtherwiseSuccessfulSend();

private:
    static void savePairing(PairingStore& pairingStore, quint16 port);
};

void MailControllerTest::savePairing(PairingStore& pairingStore, quint16 port)
{
    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("secret-1");
    pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    pairing.deviceId = QStringLiteral("dev-1");
    QVERIFY(pairingStore.save(pairing));
}

void MailControllerTest::selectKeywordFiltersCachedEmailsWithoutAnyNetworkCall()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    // "Work" tab has m1, the display-only "Uncategorized" fallback tab has
    // m2 -- so after refresh() m1 carries keywords=["Work"], m2 carries no
    // keywords at all (matches MailRepository's existing keyword-population
    // rule, see MailRepositoryTest.cpp).
    const QByteArray body = R"(
    {
      "tabs": ["Work", "Uncategorized"],
      "byTab": {
        "Work": [
          {
            "messageId": "m1",
            "sender": "a@example.com",
            "sentTo": "b@example.com",
            "cc": "",
            "bcc": "",
            "subject": "Work item",
            "status": "unread",
            "atUtc": "2026-07-01T00:00:00Z",
            "hasAttachments": false,
            "label": ""
          }
        ],
        "Uncategorized": [
          {
            "messageId": "m2",
            "sender": "c@example.com",
            "sentTo": "d@example.com",
            "cc": "",
            "bcc": "",
            "subject": "Solo item",
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

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    // refresh() is the only call in this test allowed to reach the network
    // -- it populates the cache selectKeyword() below must filter locally.
    controller.refresh();
    auto* model = qobject_cast<EmailListModel*>(controller.emailModel());
    QVERIFY(model != nullptr);
    QCOMPARE(model->rowCount(), 2);

    const QByteArray requestAfterRefresh = fake.receivedRequest();
    QVERIFY(!requestAfterRefresh.isEmpty());

    controller.selectKeyword(QStringLiteral("Work"));

    QCOMPARE(controller.selectedKeyword(), QStringLiteral("Work"));
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->emailAt(0).messageId, QStringLiteral("m1"));

    // No second connection was made -- FakeRelayServer's captured buffer is
    // unchanged (it can only append via a new connection's readyRead).
    QCOMPARE(fake.receivedRequest(), requestAfterRefresh);

    // Clearing the keyword restores both cached rows, still with no network.
    controller.selectKeyword(QString());
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(fake.receivedRequest(), requestAfterRefresh);
}

void MailControllerTest::archiveEmailsNotPairedShortCircuitsWithNoNetworkCall()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"action":"archive","processed":0,"failed":[],"targetMailbox":""})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- not paired

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    QSignalSpy errorSpy(&controller, &MailController::lastErrorChanged);
    const bool ok = controller.archiveEmails({ QStringLiteral("m1") });

    QCOMPARE(ok, false);
    QCOMPARE(controller.lastError(), QStringLiteral("Not paired"));
    QVERIFY(errorSpy.count() >= 1);
    QVERIFY(fake.receivedRequest().isEmpty());
}

void MailControllerTest::sendMailOverAttachmentCapRejectsBeforeAnyNetworkCall()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true,"warning":""})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    QTemporaryDir attachmentDir;
    QVERIFY(attachmentDir.isValid());
    const QString bigFilePath = attachmentDir.filePath(QStringLiteral("big.bin"));
    QFile bigFile(bigFilePath);
    QVERIFY(bigFile.open(QIODevice::WriteOnly));
    // One byte over the 25 MB cap -- sparse-zero-filled, so this stays fast
    // and small on disk regardless of the logical size reported back.
    QVERIFY(bigFile.resize(25LL * 1024 * 1024 + 1));
    bigFile.close();

    const bool ok = controller.sendMail(QStringLiteral("to@example.com"), QString(), QString(),
                                         QStringLiteral("Subject"), QStringLiteral("Body"), { bigFilePath },
                                         /*sign=*/false, /*encrypt=*/false);

    QCOMPARE(ok, false);
    QVERIFY(controller.lastError().contains(QStringLiteral("25 MB")));
    QVERIFY(fake.receivedRequest().isEmpty());
}

void MailControllerTest::sendMailUsesHtmlSendMode()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true,"warning":""})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    const bool ok = controller.sendMail(QStringLiteral("to@example.com"), QString(), QString(),
                                         QStringLiteral("Subject"), QStringLiteral("<b>Body</b>"), {},
                                         /*sign=*/false, /*encrypt=*/false);

    QCOMPARE(ok, true);
    const QJsonObject sent = fake.receivedJsonBody();
    QCOMPARE(sent.value(QStringLiteral("mode")).toString(), QStringLiteral("html"));
    QCOMPARE(sent.value(QStringLiteral("body")).toString(), QStringLiteral("<b>Body</b>"));
}

void MailControllerTest::downloadAttachmentSanitizesPathTraversalInSuggestedName()
{
    // Regression test for a path-traversal fix: both the caller-supplied
    // suggestedName (this test) and the server's Content-Disposition
    // filename (next test) are attacker-influenced -- they originate from
    // the mail message's own attachment metadata -- so a name containing
    // "../" segments must not be able to write outside the Downloads
    // directory. QStandardPaths::setTestModeOn() redirects
    // QStandardPaths::DownloadLocation to a sandboxed test location so this
    // test can safely assert on real filesystem writes.
    QStandardPaths::setTestModeEnabled(true);

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    const QByteArray attachmentBytes = "attachment-bytes";
    FakeRelayServer fake(httpResponse(200, "OK", attachmentBytes, "application/octet-stream"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    const QString downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(downloadDir);
    const QString escapeTarget = QDir(downloadDir).filePath(QStringLiteral("../evil.txt"));
    QFile::remove(QDir::cleanPath(escapeTarget));

    const bool ok = controller.downloadAttachment(QStringLiteral("Inbox"), QStringLiteral("42"), 0,
                                                    QStringLiteral("../evil.txt"));

    QCOMPARE(ok, true);
    QVERIFY(!QFile::exists(QDir::cleanPath(escapeTarget)));

    const QString sanitizedPath = QDir(downloadDir).filePath(QStringLiteral("evil.txt"));
    QVERIFY(QFile::exists(sanitizedPath));
    QFile written(sanitizedPath);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), attachmentBytes);
}

void MailControllerTest::downloadAttachmentSanitizesPathTraversalInServerFilename()
{
    QStandardPaths::setTestModeEnabled(true);

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    const QByteArray attachmentBytes = "server-named-bytes";
    // No suggestedName supplied by the caller -- downloadAttachment() falls
    // back to the response's Content-Disposition filename, which is just as
    // attacker-influenced as the caller-supplied name in the sibling test.
    FakeRelayServer fake(httpResponse(200, "OK", attachmentBytes, "application/octet-stream",
                                       { { "Content-Disposition", R"(attachment; filename="../../evil2.txt")" } }));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    const QString downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(downloadDir);
    const QString escapeTarget = QDir::cleanPath(QDir(downloadDir).filePath(QStringLiteral("../../evil2.txt")));
    QFile::remove(escapeTarget);

    const bool ok = controller.downloadAttachment(QStringLiteral("Inbox"), QStringLiteral("42"), 0, QString());

    QCOMPARE(ok, true);
    QVERIFY(!QFile::exists(escapeTarget));

    const QString sanitizedPath = QDir(downloadDir).filePath(QStringLiteral("evil2.txt"));
    QVERIFY(QFile::exists(sanitizedPath));
}

void MailControllerTest::findByMessageIdReturnsMapForCachedEmailAndEmptyMapWhenMissing()
{
    // Task 35: findByMessageId() is the QML-facing wrapper EmailDetail.qml
    // calls to look up a full Email by messageId -- a pure local-cache read
    // (MailController::findByMessageId -> MailRepository::findCachedEmail
    // -> EmailDao::findById), no network/pairing involved.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    Email seed;
    seed.messageId = QStringLiteral("m-1");
    seed.folder = QStringLiteral("INBOX");
    seed.sender = QStringLiteral("Alice <alice@example.com>");
    seed.subject = QStringLiteral("Hello");
    seed.preview = QStringLiteral("Preview text");
    seed.keywords = QStringList{ QStringLiteral("Work") };
    seed.hasAttachments = true;
    QVERIFY(emailDao.insertOrReplace(seed));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    const QVariantMap found = controller.findByMessageId(QStringLiteral("m-1"));
    QCOMPARE(found.value(QStringLiteral("messageId")).toString(), QStringLiteral("m-1"));
    QCOMPARE(found.value(QStringLiteral("sender")).toString(), QStringLiteral("Alice <alice@example.com>"));
    QCOMPARE(found.value(QStringLiteral("subject")).toString(), QStringLiteral("Hello"));
    QCOMPARE(found.value(QStringLiteral("hasAttachments")).toBool(), true);
    QCOMPARE(found.value(QStringLiteral("keywords")).toStringList(), QStringList{ QStringLiteral("Work") });

    QVERIFY(controller.findByMessageId(QStringLiteral("no-such-id")).isEmpty());
}

void MailControllerTest::allKeywordSettingsReflectsInboxCacheAndSetKeywordVisibleRoundTrips()
{
    // Task 39: allKeywordSettings()/setKeywordVisible() (Settings > Keywords
    // pane) are pure local reads/writes over EmailDao + SettingsStore --
    // no network/pairing involved, same shape as
    // findByMessageId's test above.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    Email seed;
    seed.messageId = QStringLiteral("m-1");
    seed.folder = QStringLiteral("INBOX");
    seed.sender = QStringLiteral("Alice <alice@example.com>");
    seed.subject = QStringLiteral("Hello");
    seed.preview = QStringLiteral("Preview text");
    seed.keywords = QStringList{ QStringLiteral("Work") };
    QVERIFY(emailDao.insertOrReplace(seed));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    // "Work" was never toggled -- SettingsStore::keywordVisible() defaults
    // to true (see its own doc comment), so it should show up as visible.
    const QVariantList before = controller.allKeywordSettings();
    QCOMPARE(before.size(), 1);
    const QVariantMap workEntry = before.at(0).toMap();
    QCOMPARE(workEntry.value(QStringLiteral("keyword")).toString(), QStringLiteral("Work"));
    QCOMPARE(workEntry.value(QStringLiteral("visible")).toBool(), true);

    QSignalSpy keywordTabsChangedSpy(&controller, &MailController::keywordTabsChanged);
    controller.setKeywordVisible(QStringLiteral("Work"), false);
    QVERIFY(keywordTabsChangedSpy.count() >= 1);

    const QVariantList after = controller.allKeywordSettings();
    QCOMPARE(after.size(), 1);
    QCOMPARE(after.at(0).toMap().value(QStringLiteral("visible")).toBool(), false);

    // Also persisted through to SettingsStore directly, independent of this
    // controller.
    QCOMPARE(settingsStore.keywordVisible(QStringLiteral("Work")), false);
}

void MailControllerTest::sendMailEmitsPickupFallbackRequiredWithTheServersAddressList()
{
    // The dialog names the SERVER's list, not the preflight's: the server ran
    // the discovery ladder as well as contacts, and may name an address typed
    // after the last preflight.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(
        409, "Conflict",
        R"({"error":"some recipients have no usable PGP key",)"
        R"("keylessRecipients":["bob@example.com"],"pickupFallbackAvailable":true})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    QSignalSpy spy(&controller, &MailController::pickupFallbackRequired);
    const bool sent = controller.sendMail(QStringLiteral("bob@example.com"), QString(), QString(),
                                           QStringLiteral("Hi"), QStringLiteral("Body"), {},
                                           /*sign=*/false, /*encrypt=*/true);

    QCOMPARE(sent, false);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{ QStringLiteral("bob@example.com") });
}

void MailControllerTest::confirmPickupFallbackSendResendsTheIdenticalBodyWithTheOptIn()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer refusal(httpResponse(
        409, "Conflict",
        R"({"error":"no usable key","keylessRecipients":["bob@example.com"],"pickupFallbackAvailable":true})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, refusal.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    QVERIFY(!controller.sendMail(QStringLiteral("bob@example.com"), QString(), QString(),
                                  QStringLiteral("Hi"), QStringLiteral("Body"), {},
                                  /*sign=*/false, /*encrypt=*/true));

    // Second server: FakeRelayServer serves exactly one connection.
    FakeRelayServer accepted(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true,"warning":""})"));
    savePairing(pairingStore, accepted.port());
    QVERIFY(controller.confirmPickupFallbackSend());

    const QJsonObject first = refusal.receivedJsonBody();
    const QJsonObject second = accepted.receivedJsonBody();
    QCOMPARE(second.value(QStringLiteral("allowPickupFallback")).toBool(), true);
    QCOMPARE(first.value(QStringLiteral("allowPickupFallback")).toBool(), false);
    // Everything else is byte-identical: same subject, body, recipients, flags.
    for (const QString& key : { QStringLiteral("to"), QStringLiteral("cc"), QStringLiteral("bcc"),
                                 QStringLiteral("subject"), QStringLiteral("body"),
                                 QStringLiteral("mode"), QStringLiteral("sign"),
                                 QStringLiteral("encrypt") }) {
        QCOMPARE(second.value(key), first.value(key));
    }
    QCOMPARE(second.value(QStringLiteral("attachments")), first.value(QStringLiteral("attachments")));

    // The opt-in is per-message: a second confirm must not re-send anything.
    QCOMPARE(controller.confirmPickupFallbackSend(), false);
}

void MailControllerTest::confirmPickupFallbackSendWithoutAPendingSendDoesNothing()
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

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    QCOMPARE(controller.confirmPickupFallbackSend(), false);
}

void MailControllerTest::sendMailSurfacesAWarningOnAnOtherwiseSuccessfulSend()
{
    // A non-empty warning means partial trouble (the Sent copy failed, or some
    // pickup links did not deliver) -- the message WAS sent, so this must not
    // look like a failure and must not offer a retry that would duplicate it.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    // Custom raw-string delimiter (not the default R"(...)"): the warning
    // text's "recipient(s)" contains the literal sequence )" , which would
    // otherwise terminate a default-delimited raw string early.
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"json({"ok":true,"sentSaved":false,"warning":"failed to deliver a pickup link to 1 of 3 recipient(s)"})json"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QTemporaryDir cursorDir;
    QVERIFY(cursorDir.isValid());
    CursorStore cursorStore(cursorDir.filePath(QStringLiteral("cursor.ini")));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository keywordRepository(settingsStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);
    PgpBootstrapClient bootstrapClient(http);
    PgpRecipientChecker recipientChecker(http);
    MailRepository mailRepository(source, emailDao, pairingStore, cursorStore);
    FolderDao folderDao(db.handle());
    FolderClient folderClient(http);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);

    MailController controller(mailRepository, source, keywordRepository, pairingStore, folderRepository,
                               settingsStore, bootstrapClient, recipientChecker);

    QSignalSpy spy(&controller, &MailController::sendWarning);
    const bool sent = controller.sendMail(QStringLiteral("bob@example.com"), QString(), QString(),
                                           QStringLiteral("Hi"), QStringLiteral("Body"), {},
                                           /*sign=*/false, /*encrypt=*/true);

    QCOMPARE(sent, true);
    QCOMPARE(spy.count(), 1);
    QVERIFY(!spy.at(0).at(0).toString().isEmpty());
}

QTEST_GUILESS_MAIN(MailControllerTest)
#include "MailControllerTest.moc"
