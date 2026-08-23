#pragma once

// A whole MailController, wired the way main.cpp wires it: a real database, a
// real pairing, a real executor, and a fake relay on a real socket.
//
// Lifted out of MailDecryptionTest.cpp when the client-encrypted SEND tests
// needed the same thing, on the same reasoning as tests/core/net/
// FakeRelayServer.h -- one copy rather than a second that drifts.

#include "mail/MailController.h"

#include "db/Database.h"
#include "db/EmailDao.h"
#include "db/FolderDao.h"
#include "domain/DevicePairing.h"
#include "domain/FolderRepository.h"
#include "domain/KeywordRepository.h"
#include "domain/MailRepository.h"
#include "domain/PairingStore.h"
#include "net/FolderClient.h"
#include "net/HttpClient.h"
#include "net/NetworkExecutor.h"
#include "net/PgpBootstrapClient.h"
#include "net/PgpRecipientChecker.h"
#include "net/RelayMailSource.h"
#include "stores/CursorStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include "../../core/net/FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <memory>

// Harness shared by every test below: a real database, a real pairing, a real
// executor and a fake relay that answers the inbox first and the pgp-payload
// route second.
struct DecryptHarness
{
    Database db;
    QTemporaryDir secureDir;
    QTemporaryDir cursorDir;
    QTemporaryDir settingsDir;
    QNetworkAccessManager manager;

    std::unique_ptr<EmailDao> emailDao;
    std::unique_ptr<SecureStoreFile> secureStore;
    std::unique_ptr<PairingStore> pairingStore;
    std::unique_ptr<CursorStore> cursorStore;
    std::unique_ptr<SettingsStore> settingsStore;
    std::unique_ptr<KeywordRepository> keywordRepository;
    std::unique_ptr<HttpClient> http;
    std::unique_ptr<RelayMailSource> source;
    std::unique_ptr<PgpBootstrapClient> bootstrapClient;
    std::unique_ptr<PgpRecipientChecker> recipientChecker;
    std::unique_ptr<MailRepository> mailRepository;
    std::unique_ptr<FolderDao> folderDao;
    std::unique_ptr<FolderClient> folderClient;
    std::unique_ptr<FolderRepository> folderRepository;
    std::unique_ptr<NetworkExecutor> executor;
    std::unique_ptr<MailController> controller;

    bool build(FakeRelayServer& fake)
    {
        if (!db.open(QStringLiteral(":memory:")) || !secureDir.isValid() || !cursorDir.isValid()
            || !settingsDir.isValid()) {
            return false;
        }
        emailDao = std::make_unique<EmailDao>(db.handle());
        secureStore = std::make_unique<SecureStoreFile>(secureDir.path());
        pairingStore = std::make_unique<PairingStore>(*secureStore);

        DevicePairing pairing;
        pairing.subscriberId = QStringLiteral("sub-1");
        pairing.deviceSecret = QStringLiteral("secret-1");
        pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
        pairing.deviceId = QStringLiteral("dev-1");
        if (!pairingStore->save(pairing))
            return false;

        cursorStore = std::make_unique<CursorStore>(cursorDir.filePath(QStringLiteral("cursor.ini")));
        settingsStore = std::make_unique<SettingsStore>(settingsDir.filePath(QStringLiteral("settings.ini")));
        keywordRepository = std::make_unique<KeywordRepository>(*settingsStore);
        http = std::make_unique<HttpClient>(manager);
        source = std::make_unique<RelayMailSource>(*http);
        bootstrapClient = std::make_unique<PgpBootstrapClient>(*http);
        recipientChecker = std::make_unique<PgpRecipientChecker>(*http);
        mailRepository =
            std::make_unique<MailRepository>(*source, *emailDao, *pairingStore, *cursorStore);
        folderDao = std::make_unique<FolderDao>(db.handle());
        folderClient = std::make_unique<FolderClient>(*http);
        folderRepository = std::make_unique<FolderRepository>(*folderClient, *folderDao, *pairingStore);
        executor = std::make_unique<NetworkExecutor>(3000);
        controller = std::make_unique<MailController>(*mailRepository, *source, *keywordRepository,
                                                       *pairingStore, *folderRepository, *settingsStore,
                                                       *bootstrapClient, *recipientChecker, *executor);
        return true;
    }

    ~DecryptHarness()
    {
        if (executor)
            executor->shutdown();
    }
};
