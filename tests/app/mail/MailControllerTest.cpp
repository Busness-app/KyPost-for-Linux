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
#include <QJsonArray>
#include <QJsonObject>
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
    void downloadAttachmentStripsEmbeddedNulFromTheFilename();
    void hostileLocationRefusesToOpenNonAllowlistedAttachmentTypes();
    void hostileLocationForcesTheExtensionToMatchTheDeclaredType();
    void findByMessageIdReturnsMapForCachedEmailAndEmptyMapWhenMissing();
    void allKeywordSettingsReflectsInboxCacheAndSetKeywordVisibleRoundTrips();
    void sendMailEmitsPickupFallbackRequiredWithTheServersAddressList();
    void confirmPickupFallbackSendResendsTheIdenticalBodyWithTheOptIn();
    void confirmPickupFallbackSendWithoutAPendingSendDoesNothing();
    void sendMailSurfacesAWarningOnAnOtherwiseSuccessfulSend();
    void sendMailClientSideNeededHandsOffAndOffersNoToggles();
    void openWebmailDraftsRefusesAnInsecurePairingBeforeSavingAnything();
    void refreshPgpComposeStateFailureHidesEveryControl();
    void preflightRecipientsFailureClearsRatherThanReassures();

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
    QCOMPARE(spy.at(0).at(1).toStringList(), QStringList{ QStringLiteral("bob@example.com") });
    // The token names the one pending send this refusal belongs to. Never 0:
    // a default-constructed PendingSend must not be able to match it.
    QVERIFY(spy.at(0).at(0).value<quint64>() != 0);
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

    // A REAL attachment, deleted between the refusal and the confirm. This is
    // the assertion the whole PendingSend cache exists to satisfy: with an
    // empty attachment list the test could not tell "the cache held the bytes"
    // apart from "there were never any bytes", and rebuilding the payload from
    // the QML fields on confirm would re-read this path and fail here.
    QTemporaryDir attachmentDir;
    QVERIFY(attachmentDir.isValid());
    const QString attachmentPath = attachmentDir.filePath(QStringLiteral("note.txt"));
    const QByteArray originalBytes = "the-bytes-the-user-reviewed";
    {
        QFile attachment(attachmentPath);
        QVERIFY(attachment.open(QIODevice::WriteOnly));
        QCOMPARE(attachment.write(originalBytes), static_cast<qint64>(originalBytes.size()));
    }

    QSignalSpy fallbackSpy(&controller, &MailController::pickupFallbackRequired);
    QVERIFY(!controller.sendMail(QStringLiteral("bob@example.com"), QString(), QString(),
                                  QStringLiteral("Hi"), QStringLiteral("Body"), { attachmentPath },
                                  /*sign=*/false, /*encrypt=*/true));
    QCOMPARE(fallbackSpy.count(), 1);
    const quint64 token = fallbackSpy.at(0).at(0).value<quint64>();

    // The file is gone AND its contents replaced by the time the user
    // confirms -- an editor saved over it, or a temp export was cleaned up.
    QVERIFY(QFile::remove(attachmentPath));
    {
        QFile replacement(attachmentPath);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write("SOMETHING-ELSE-ENTIRELY");
    }

    // Second server: FakeRelayServer serves exactly one connection.
    FakeRelayServer accepted(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true,"warning":""})"));
    savePairing(pairingStore, accepted.port());

    // A confirmation naming a DIFFERENT pending send must not send: this is
    // what stops a dialog opened in another composer from resolving this one.
    // Checked before the real confirm on purpose -- `accepted` serves a single
    // connection, so if this reached the network the confirm below would fail.
    QCOMPARE(controller.confirmPickupFallbackSend(token + 1), false);

    QVERIFY(controller.confirmPickupFallbackSend(token));

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

    // ...and those identical attachment bytes are the ORIGINAL file's, not
    // whatever is on disk now.
    const QJsonArray resentAttachments = second.value(QStringLiteral("attachments")).toArray();
    QCOMPARE(resentAttachments.size(), 1);
    const QJsonObject resent = resentAttachments.at(0).toObject();
    QCOMPARE(resent.value(QStringLiteral("name")).toString(), QStringLiteral("note.txt"));
    QCOMPARE(QByteArray::fromBase64(resent.value(QStringLiteral("dataBase64")).toString().toLatin1()),
             originalBytes);

    // The opt-in is per-message: a second confirm must not re-send anything.
    QCOMPARE(controller.confirmPickupFallbackSend(token), false);
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

    // Any token at all, with nothing cached: a stray confirm must not mail.
    QCOMPARE(controller.confirmPickupFallbackSend(1), false);
    QCOMPARE(controller.confirmPickupFallbackSend(0), false);
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

void MailControllerTest::sendMailClientSideNeededHandsOffAndOffersNoToggles()
{
    // The other 409. Categorical, not confirmable: the account's private key
    // exists only in the user's browser, so no request from this app can
    // encrypt on its behalf. Nothing may stay cached for a confirm, and the
    // composer must show the handoff INSTEAD OF the toggles, never both.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(
        409, "Conflict",
        R"({"error":"this account's PGP key is end-to-end protected","clientSideNeeded":true})"));

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

    QSignalSpy fallbackSpy(&controller, &MailController::pickupFallbackRequired);
    QSignalSpy composeStateSpy(&controller, &MailController::pgpComposeStateChanged);

    const bool sent = controller.sendMail(QStringLiteral("bob@example.com"), QString(), QString(),
                                           QStringLiteral("Hi"), QStringLiteral("Body"), {},
                                           /*sign=*/false, /*encrypt=*/true);

    QCOMPARE(sent, false);
    // Never the plaintext-link dialog: this refusal is not confirmable.
    QCOMPARE(fallbackSpy.count(), 0);
    QVERIFY(composeStateSpy.count() >= 1);
    QCOMPARE(controller.pgpHandoffToWebmail(), true);
    // One source of truth: the handoff block replaces the toggles.
    QCOMPARE(controller.pgpCanEncrypt(), false);
    QCOMPARE(controller.pgpCanSign(), false);
    QVERIFY(!controller.lastError().isEmpty());
    // The pending send was dropped, so a stray confirm cannot resurrect it.
    QCOMPARE(controller.confirmPickupFallbackSend(1), false);
}

void MailControllerTest::openWebmailDraftsRefusesAnInsecurePairingBeforeSavingAnything()
{
    // Order matters more than the return value: the https check runs BEFORE
    // the draft POST, so a handoff that cannot open the browser does not leave
    // a silently duplicated draft behind. savePairing() writes an
    // http://127.0.0.1 base and webmailMailboxUrl() is https-only, so this
    // pairing can never produce a link.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));

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

    const bool opened = controller.openWebmailDrafts(QStringLiteral("bob@example.com"), QString(), QString(),
                                                       QStringLiteral("Hi"), QStringLiteral("Body"), {});

    QCOMPARE(opened, false);
    QVERIFY(controller.lastError().contains(QStringLiteral("insecure connection")));
    // The actual invariant: no draft POST was made at all.
    QVERIFY(fake.receivedRequest().isEmpty());
}

void MailControllerTest::refreshPgpComposeStateFailureHidesEveryControl()
{
    // "Couldn't check" is never "no PGP" AND never "client custody" -- a
    // failed bootstrap must leave every control hidden rather than guess a
    // custody mode and offer the wrong send path.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(503, "Service Unavailable", "unavailable", "text/plain"));

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

    controller.refreshPgpComposeState();

    QCOMPARE(controller.pgpCanEncrypt(), false);
    QCOMPARE(controller.pgpCanSign(), false);
    QCOMPARE(controller.pgpHandoffToWebmail(), false);
}

void MailControllerTest::preflightRecipientsFailureClearsRatherThanReassures()
{
    // The preflight is an advisory lower bound. A failed check must leave the
    // list EMPTY -- showing nothing -- rather than a stale answer standing in
    // for one this call never got.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(500, "Internal Server Error", "boom", "text/plain"));

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

    controller.preflightRecipients(QStringLiteral("bob@example.com"), QString(), QString());

    QVERIFY(controller.pgpKeylessRecipients().isEmpty());
}


// Hostile Location Protection routes attachments through
// openAttachmentEphemerally(), which ends in QDesktopServices::openUrl --
// i.e. it hands attacker-supplied bytes to whatever handler the desktop has
// registered for the file's extension. There was no type check at all, so
// "Invoice.pdf.desktop" or "notes.pdf.html" got an arbitrary handler
// launched with the user's full session privileges. The mode built for
// people who expect to be attacked was the only mode that auto-opened
// hostile input.
void MailControllerTest::hostileLocationRefusesToOpenNonAllowlistedAttachmentTypes()
{
    QStandardPaths::setTestModeEnabled(true);

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    // Declared type is not on the allowlist, whatever the name claims.
    FakeRelayServer fake(httpResponse(200, "OK", "#!/bin/sh\necho pwned", "application/x-desktop",
                                       { { "Content-Disposition",
                                           "attachment; filename=\"Invoice.pdf.desktop\"" } }));

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
    settingsStore.setHostileLocationProtectionEnabled(true);
    KeywordRepository keywordRepository(settingsStore);

    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toUtf8());

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

    const bool ok = controller.downloadAttachment(QStringLiteral("Inbox"), QStringLiteral("42"), 0,
                                                    QStringLiteral("Invoice.pdf.desktop"));

    QCOMPARE(ok, false);
    QVERIFY2(!controller.lastError().isEmpty(), "the refusal must be explained, not silent");

    // Nothing was written anywhere for a handler to pick up.
    const QDir attachmentDir(runtimeDir.filePath(QStringLiteral("kypost-attachments")));
    QVERIFY(attachmentDir.entryList(QDir::Files).isEmpty());
}

// Even for an allowlisted type, the filename must not decide the handler:
// a .desktop name declared as application/pdf would otherwise pass the gate
// and still be launched as a .desktop file.
void MailControllerTest::hostileLocationForcesTheExtensionToMatchTheDeclaredType()
{
    QStandardPaths::setTestModeEnabled(true);

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao emailDao(db.handle());

    FakeRelayServer fake(httpResponse(200, "OK", "%PDF-1.4 fake", "application/pdf",
                                       { { "Content-Disposition",
                                           "attachment; filename=\"Invoice.pdf.desktop\"" } }));

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
    settingsStore.setHostileLocationProtectionEnabled(true);
    KeywordRepository keywordRepository(settingsStore);

    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toUtf8());

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

    QVERIFY(controller.downloadAttachment(QStringLiteral("Inbox"), QStringLiteral("42"), 0,
                                           QStringLiteral("Invoice.pdf.desktop")));

    const QDir attachmentDir(runtimeDir.filePath(QStringLiteral("kypost-attachments")));
    const QStringList written = attachmentDir.entryList(QDir::Files);
    QCOMPARE(written.size(), 1);
    QVERIFY2(written.first().endsWith(QStringLiteral(".pdf")),
             "the extension must come from the declared MIME type, not the message");
    QVERIFY(!written.first().contains(QStringLiteral(".desktop")));

    controller.clearEphemeralAttachments();
}


// A NUL in the mail-supplied filename split Qt's own view of the path in two:
// QFile::open() hands the encoded path to open(2), which truncates at the
// NUL, while QFile::exists()/QFileInfo::exists()/QFile::remove() reject the
// same string outright. Under Hostile Location Protection that defeated the
// MIME-driven extension forcing -- "Invoice.desktop\0.pdf" looked like a
// .pdf to the gate and landed as "Invoice.desktop" on disk, handing the
// desktop's handler choice back to the sender -- and it silently no-op'd the
// delete timer, the exit cleanup and the write rollback, so the file outlived
// the session in the one mode that promises it cannot.
void MailControllerTest::downloadAttachmentStripsEmbeddedNulFromTheFilename()
{
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
    const QString truncatedTarget = QDir(downloadDir).filePath(QStringLiteral("Invoice.desktop"));
    QFile::remove(truncatedTarget);

    QString hostile = QStringLiteral("Invoice.desktop");
    hostile.append(QChar(u'\0'));
    hostile.append(QStringLiteral(".pdf"));
    QCOMPARE(hostile.size(), 20);

    QVERIFY(controller.downloadAttachment(QStringLiteral("Inbox"), QStringLiteral("42"), 0, hostile));

    // The name open(2) would have truncated to must NOT be what landed.
    QVERIFY(!QFile::exists(truncatedTarget));

    // What did land keeps its real extension and is visible to the same Qt
    // calls that clean it up -- exists() agreeing with open() is the point.
    const QString expected = QDir(downloadDir).filePath(QStringLiteral("Invoice.desktop.pdf"));
    QVERIFY(QFile::exists(expected));
    QVERIFY(QFile::remove(expected));
}

QTEST_GUILESS_MAIN(MailControllerTest)
#include "MailControllerTest.moc"

