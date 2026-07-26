#include "contacts/ContactsController.h"
#include "general/GeneralController.h"
#include "mail/MailController.h"
#include "mail/RemoteContentInterceptor.h"
#include "pairing/MfaController.h"
#include "pairing/PairingController.h"
#include "pgp/PgpQrController.h"
#include "pgp/PgpQrScanner.h"
#include "platform/SecureStoreKeychain.h"
#include "security/AppLockManager.h"
#include "security/AppRelauncher.h"
#include "push/NotificationDispatcher.h"
#include "push/NtfyTopicProvisioner.h"
#include "push/PushPayloadParser.h"
#include "push/UnifiedPushConnector.h"
#include "theme/ThemeController.h"
#include "tray/TrayController.h"

#include "db/ContactDao.h"
#include "db/Database.h"
#include "db/EmailDao.h"
#include "db/FolderDao.h"
#include "db/SecurityWipe.h"
#include "db/GroupDao.h"
#include "db/PendingContactChangeDao.h"
#include "db/PushDao.h"
#include "domain/ContactPhotoRepository.h"
#include "domain/ContactSyncRepository.h"
#include "domain/DeviceRegistrationService.h"
#include "domain/GroupsRepository.h"
#include "domain/KeywordRepository.h"
#include "domain/FolderRepository.h"
#include "security/AppLockStore.h"
#include "domain/MailRepository.h"
#include "domain/PairingStore.h"
#include "domain/PairingStoreCredentialSealer.h"
#include "domain/PgpQrRepository.h"
#include "domain/PushRepository.h"
#include "domain/TransportStateMachine.h"
#include "models/PushNotification.h"
#include "net/ContactPhotoClient.h"
#include "net/ContactSyncClient.h"
#include "net/FolderClient.h"
#include "net/GroupsClient.h"
#include "net/HttpClient.h"
#include "net/MfaResponseClient.h"
#include "net/DeregisterClient.h"
#include "net/NativeRegistrationClient.h"
#include "net/NtfySubscriber.h"
#include "net/PgpBootstrapClient.h"
#include "net/PgpQrClient.h"
#include "net/PgpRecipientChecker.h"
#include "net/PushNotificationClient.h"
#include "net/RelayMailSource.h"
#include "stores/ContactPhotoCache.h"
#include "stores/CursorStore.h"
#include "stores/SettingsStore.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QQmlApplicationEngine>
#include <QStandardPaths>
#include <QUrl>
#include <QWindow>

#include <KDBusService>
#include <KLocalizedQmlContext>
#include <KLocalizedString>

// Task 28: every bundled font file (see app/resources/fonts.qrc) loaded via
// QFontDatabase::addApplicationFont before the QQmlApplicationEngine is
// constructed, so QML content can reference these families from the very
// first frame. These 11 files are baked into the resource bundle at build
// time (app/resources/fonts.qrc), so a load failure here is a packaging
// defect, not an expected runtime condition -- unlike the mobile
// counterparts' system-font-fallback pattern, there is no silent fallback:
// qWarning() on every -1 so a broken bundle is loud, not silently degraded.
static void loadBundledFonts()
{
    static const char* const kFontResources[] = {
        ":/fonts/SpaceGrotesk-Light.ttf",
        ":/fonts/SpaceGrotesk-Regular.ttf",
        ":/fonts/SpaceGrotesk-Medium.ttf",
        ":/fonts/SpaceGrotesk-SemiBold.ttf",
        ":/fonts/SpaceGrotesk-Bold.ttf",
        ":/fonts/IBMPlexMono-Regular.ttf",
        ":/fonts/IBMPlexMono-Italic.ttf",
        ":/fonts/IBMPlexMono-Medium.ttf",
        ":/fonts/IBMPlexMono-SemiBold.ttf",
        ":/fonts/IBMPlexMono-Bold.ttf",
        ":/fonts/IBMPlexMono-BoldItalic.ttf",
    };
    for (const char* resource : kFontResources) {
        const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(resource));
        if (id == -1) {
            qWarning() << "loadBundledFonts: failed to load bundled font:" << resource;
            continue;
        }
        qDebug() << "loadBundledFonts: loaded" << resource << "as"
                  << QFontDatabase::applicationFontFamilies(id);
    }
}

// Task 34: replaces the Task 12 log-only stub -- scans a set of
// command-line-style arguments for a kypost:// deep link and, for a
// recognized kypost://native-pair link, actually routes it to
// PairingController::pairFromDeepLink() instead of just logging it (see
// PairingController.cpp's parseNativePairLink() for the full sub/hash/srv/
// pt/reg parsing contract). Reachable both via a fresh launch's argv and via
// KDBusService::activateRequested relaying a second launch's argv to the
// already-running instance -- same dual call-site shape Task 12 established.
//
// kypost://desktop-pair (or any other kypost:// host) is
// unrecognized here, per Phase 6 global constraint 6 (desktop pairing is
// out of scope for this client family) -- it falls through to the same
// redacted-query-string qDebug() Task 11's security review required,
// unchanged. Never logs sub/hash/pt (or the query string of any
// unrecognized kypost:// link) -- those are real authentication
// credentials.
//
// pairingController is a plain (non-owning) pointer rather than a
// reference: see main()'s comment on pairingControllerForDeepLinks for why
// it can transiently be nullptr between KDBusService's construction and
// PairingController's.
static void routeDeepLink(const QStringList& arguments, PairingController* pairingController)
{
    for (const QString& argument : arguments) {
        const QUrl url(argument);
        if (url.scheme() != QStringLiteral("kypost"))
            continue;

        if (url.host() == QStringLiteral("native-pair")) {
            if (pairingController == nullptr) {
                // Should not happen in practice -- see main()'s comment on
                // pairingControllerForDeepLinks -- but a dropped pairing
                // attempt is much better than a null dereference.
                qWarning() << "routeDeepLink: native-pair link arrived before PairingController was ready, dropping";
                return;
            }
            qDebug() << "routeDeepLink: routing kypost://native-pair link to PairingController";
            pairingController->pairFromDeepLink(url);
            return;
        }

        // Any other kypost:// host (including desktop-pair, which
        // Phase 6 global constraint 6 puts explicitly out of scope) is
        // unrecognized -- strip the query string before logging, same as
        // the Task 11 stub did, so a real credential never lands in the
        // journal.
        QUrl redacted = url;
        redacted.setQuery(QString());
        qDebug() << "routeDeepLink: received unrecognized deep link:" << redacted;
        return;
    }
}

int main(int argc, char* argv[])
{
    // QApplication (not QGuiApplication): TrayController's KStatusNotifierItem
    // uses a QMenu (Qt Widgets) for its standard-actions context menu, and a
    // QWidget cannot be constructed under a plain QGuiApplication. QApplication
    // is a strict superset of QGuiApplication, so every existing QGuiApplication-
    // typed usage of `app` below (e.g. applicationStateChanged) still compiles.
    QApplication app(argc, argv);

    // Task 11: applicationName + organizationDomain feed KDBusService's name
    // derivation (reversed domain + app name -- see KDBusService::KDBusService
    // docs), which must produce the same "com.urlxl.mail" well-known
    // name that UnifiedPushConnector registers below and that the .service
    // file at ~/.local/share/dbus-1/services/com.urlxl.mail.service
    // advertises to the D-Bus daemon for on-demand activation.
    app.setApplicationName(QStringLiteral("mail"));
    app.setOrganizationDomain(QStringLiteral("urlxl.com"));

    // Task 48: KI18n translation domain. Per KLocalizedString::
    // setApplicationDomain()'s own doc comment ("This function should be
    // called right after creating the instance of QCoreApplication or one of
    // its subclasses"), so it runs before anything below can call i18n()/
    // i18nc() (KLocalizedContext registration further down, deep-link
    // routing, etc.). "kypost" matches po/kypost.pot's catalog name
    // (po/CMakeLists.txt's ki18n_install()/po/extract-messages.sh) and the
    // .mo filename that will be installed at
    // share/locale/<lang>/LC_MESSAGES/kypost.mo once Task 49 lands real
    // .po files -- there are none yet (phase8-global-constraints.md item 5),
    // so every i18n() call falls back to its literal source-code English
    // text right now, which is the correct, expected behavior for this
    // phase.
    KLocalizedString::setApplicationDomain("kypost");

    // KDBusService (KF6DBusAddons, confirmed installed via `pacman -Qs
    // kdbusaddons`) claims the well-known bus name in Unique mode. Verified
    // (Task 11) that this build (KF6DBusAddons 6.27.0) does NOT export
    // org.freedesktop.Application/Activate at /MainApplication -- introspection
    // shows only org.qtproject.Qt.QCoreApplication/QGuiApplication reflected
    // properties there. activateRequested() below fires via the documented
    // Unique-mode collision path instead: launching the binary again while an
    // instance is already running (including one the D-Bus daemon just spawned
    // per the .service file's Exec=) relays the new invocation's argv/cwd to
    // the running instance over D-Bus and emits this signal there; the
    // duplicate process then quits on its own. Constructed before
    // QQmlApplicationEngine per KDBusService's own race-avoidance guidance:
    // export D-Bus objects before the event loop (app.exec()) runs.
    //
    // Must also stay constructed before UnifiedPushConnector below: this is
    // what actually claims the "com.urlxl.mail" well-known bus name on
    // the session bus. UnifiedPushConnector relies on that name already
    // being owned by this same connection by the time it constructs -- it
    // does not register the name itself. Reordering these two would make
    // UnifiedPushConnector grab the name first, and KDBusService::Unique
    // would then think another instance is already running and relay-and-quit
    // on every launch.
    //
    // Task 34: PairingController doesn't exist yet at this point in main()
    // (it's constructed further down, after the core/domain composition
    // root it wraps) but the lambda below needs to be able to reach it once
    // a second launch is relayed here -- which can only happen once this
    // process has entered app.exec(), by which point main() has long since
    // finished constructing pairingController and pointed
    // pairingControllerForDeepLinks at it. The lambda captures this pointer
    // variable by reference (not the not-yet-existing controller itself),
    // so it always sees the up-to-date value whenever activateRequested
    // actually fires.
    PairingController* pairingControllerForDeepLinks = nullptr;
    KDBusService dbusService(KDBusService::Unique);
    QObject::connect(&dbusService, &KDBusService::activateRequested, &dbusService,
                      [&pairingControllerForDeepLinks](const QStringList& arguments, const QString& workingDirectory) {
                          // Task 34 security fix: this used to log the raw `arguments` list
                          // (Task 11/12), which is fine while every kypost:// URL only
                          // ever carries a fake test token, but arguments can now carry a
                          // real kypost://native-pair link -- sub/hash/pt are real
                          // authentication credentials once pairFromDeepLink() below
                          // succeeds. Log only the count and workingDirectory here;
                          // routeDeepLink() below does its own properly-redacted logging of
                          // the link itself.
                          qDebug() << "KDBusService: activateRequested -- argument count:" << arguments.size()
                                    << "workingDirectory:" << workingDirectory;
                          // Task 12/34: a second `kypost kypost://...` invocation gets
                          // redirected here instead of spawning a duplicate process -- route
                          // its argv through the same deep-link router used at startup.
                          routeDeepLink(arguments, pairingControllerForDeepLinks);
                      });

    // Task 28: bundled fonts must be registered with QFontDatabase before
    // the QQmlApplicationEngine below parses any QML that might reference
    // them by family name (none does yet, but ThemeController.fontUi()/
    // fontMono() already advertise these family names to future QML).
    loadBundledFonts();

    // Task 28: first real on-disk location for app-level persistent state
    // (core/ deliberately never decides this -- see SettingsStore.h's doc
    // comment). AppConfigLocation is the XDG-correct place for a
    // human-editable-if-needed settings file, distinct from AppDataLocation
    // (databases/caches, not used by this task). Constructed before
    // ThemeController/QQmlApplicationEngine since both need it to already
    // exist.
    const QString settingsDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(settingsDir);
    // VibeSec fix: lock this app's own per-user config directory to
    // owner-only (0700) so settings.ini can't be read by other local users
    // on a multi-user system -- mirrors the discipline SecureStoreFile.cpp
    // already applies to individual secret files. Re-applied on every
    // startup (idempotent) so a directory left over from before this fix
    // gets tightened too, not just freshly created ones.
    QFile::setPermissions(settingsDir,
                           QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    SettingsStore settingsStore(settingsDir + QStringLiteral("/settings.ini"));

    // Convergent root selection (see the comment further below at
    // engine.load()): resolved here, right after settingsStore exists,
    // because GeneralController (constructed next) needs to know this
    // process's resolved mode at construction time (its isDesktopMode
    // property is CONSTANT -- fixed for the process lifetime, distinct from
    // the pending settingsStore.preferredMode() preference for next launch).
    // A persisted "desktop"/"mobile" preference overrides the env var;
    // "auto" (the default) preserves today's env-var-only behavior.
    const QString preferredMode = settingsStore.preferredMode();
    const bool isMobile = preferredMode == QStringLiteral("mobile")    ? true
                         : preferredMode == QStringLiteral("desktop")  ? false
                         : qEnvironmentVariableIntValue("QT_QUICK_CONTROLS_MOBILE") != 0;

    // Task 28: wraps core::ThemeManager (which itself wraps settingsStore
    // above) for QML consumption. Constructed before the engine and
    // registered as a QML singleton instance immediately after, so
    // MobileRoot.qml (loaded just below) can already bind to Theme.* from
    // its very first frame in a later task -- this task only proves the
    // singleton registers and loads cleanly.
    ThemeController themeController(settingsStore);
    qmlRegisterSingletonInstance<ThemeController>(
        "com.urlxl.mail", 1, 0, "Theme", &themeController);

    // GeneralController: Desktop/Mobile mode preference + desktop-only tray
    // settings. Shares no state with ThemeController -- registered the same
    // way, right next to it.
    GeneralController generalController(settingsStore, !isMobile);
    qmlRegisterSingletonInstance<GeneralController>(
        "com.urlxl.mail", 1, 0, "General", &generalController);

    // Task 31: composition root for the rest of core/db, core/net, and
    // core/domain -- every later Phase 6 task (32-34) builds its
    // QObject-derived controller against references into this graph rather
    // than constructing its own copies. Everything below is a main() local,
    // same lifetime tier as settingsStore/themeController above (declared
    // before QQmlApplicationEngine, destroyed in reverse declaration order
    // at the end of main()). Order matters: each object below only takes
    // references/handles to objects already constructed above it.
    //
    // 1. Database -- AppDataLocation (not AppConfigLocation, which
    // settingsDir above already claimed for settings.ini) is the XDG-correct
    // place for a real on-disk database file. A failed open() has no
    // reasonable degraded mode -- every DAO below hands out a live
    // QSqlDatabase& into this object, so treat it as fatal.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    // VibeSec fix: same rationale as settingsDir above -- kypost.db holds
    // full contact records (names, emails, phones, notes, PGP public keys)
    // and mail content in plaintext, and the contact-photos cache directory
    // constructed below also lives under dataDir, so this one chmod covers
    // both.
    QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    // One-time migration for the Llama Mail -> KyPost rename, which moved the
    // profile in TWO independent ways that have to be undone together:
    //
    //   1. the database filename:  llamamail.db -> kypost.db
    //   2. the whole data directory, because AppDataLocation is derived from
    //      QCoreApplication::applicationName() and that changed
    //      "LlamaMail" -> "mail" (commit e0dde4e, one day before the
    //      filename change). Measured: ~/.local/share/LlamaMail
    //      vs ~/.local/share/mail.
    //
    // The original migration only handled (1), so it looked for
    // llamamail.db inside the NEW directory and could never find a genuinely
    // pre-rename profile, which lives in the old one. Both candidates are
    // checked here, newest-directory first.
    //
    // Known limitation, stated rather than papered over: the guard below is
    // "only if no kypost.db exists yet". An install that already launched a
    // post-rename build has an empty kypost.db, and this will not overwrite
    // it -- choosing between two populated databases is a data-merge
    // decision, not a rename fix, and silently picking one could destroy
    // mail. Such profiles keep their old data orphaned on disk and re-sync
    // from the relay instead. See docs/RENAME_NOTES.md.
    const QString newDbPath = dataDir + QStringLiteral("/kypost.db");
    if (!QFile::exists(newDbPath)) {
        const QString legacyDataDir =
            QFileInfo(dataDir).absolutePath() + QStringLiteral("/LlamaMail");
        const QStringList oldDbCandidates = {
            dataDir + QStringLiteral("/llamamail.db"),
            legacyDataDir + QStringLiteral("/llamamail.db"),
        };
        for (const QString& oldDbPath : oldDbCandidates) {
            if (!QFile::exists(oldDbPath))
                continue;
            if (QFile::copy(oldDbPath, newDbPath)) {
                // Copy, not rename: leaving the original in place means a
                // failed or unwanted migration is always recoverable by
                // hand. It costs one database's worth of disk, once.
                qInfo("main: migrated pre-rename database from %s", qPrintable(oldDbPath));
            } else {
                qWarning("main: failed to migrate pre-rename database from %s", qPrintable(oldDbPath));
            }
            break;
        }
    }
    // Hostile Location Protection: read BEFORE the database is constructed,
    // because it decides what the database even is. ":memory:" leaves no
    // mail, contacts or folders on disk for the whole session. Every DAO is
    // unchanged either way -- they only ever see a QSqlDatabase&.
    //
    // This is also why toggling the setting relaunches the process rather
    // than taking effect live: main()'s composition root is one unbroken
    // chain of stack locals from Database down through every DAO,
    // repository, controller and QML singleton, and there is no supported
    // way to re-point it at a different database at runtime.
    const bool hostileLocation = settingsStore.hostileLocationProtectionEnabled();

    Database database;
    if (hostileLocation) {
        // Defensive: if a previous run crashed between "write the flag" and
        // "delete the file", there could still be a database on disk holding
        // exactly what this mode exists to prevent.
        SecurityWipe::removeDatabaseFiles(newDbPath);
        SecurityWipe::clearCacheDirectory(dataDir + QStringLiteral("/contact-photos"));
        if (!database.open(QStringLiteral(":memory:")))
            qFatal("main: Database::open failed for in-memory database");
    } else if (!database.open(newDbPath)) {
        qFatal("main: Database::open failed for %s", qPrintable(newDbPath));
    }

    // extended-contact-fields Task 3: on-disk cache for fetched contact
    // photo bytes, keyed by Contact::photoRef -- reuses the same
    // AppDataLocation dataDir already resolved for the SQLite database
    // above, in its own "contact-photos" subdirectory (see
    // core/stores/ContactPhotoCache.h's doc comment for why no invalidation
    // logic is needed here).
    ContactPhotoCache contactPhotoCache(dataDir + QStringLiteral("/contact-photos"));

    // 2. DAOs, each borrowing database.handle(). PushDao has no
    // Phase 6 caller yet -- constructed anyway per the task-31 brief, since
    // it's part of the schema Phase 2 already built and future phases
    // will need it wired here rather than re-deriving this block.
    EmailDao emailDao(database.handle());
    ContactDao contactDao(database.handle());
    PushDao pushDao(database.handle());
    PendingContactChangeDao pendingContactChangeDao(database.handle());
    // extended-contact-fields Task 2: local name-cache for backend contact
    // groups (see core/db/migrations/003_extended_contact_fields.sql's
    // appended `groups` table).
    GroupDao groupDao(database.handle());
    // Server-side mailbox list (subfolders under Archive). The table and DAO
    // existed since the initial schema but were never constructed here, so
    // the sidebar showed only the six hardcoded standard folders.
    FolderDao folderDao(database.handle());

    // 3. SecureStoreKeychain -- the app/platform/ Secret-Service-backed
    // SecureStore built in Phase 2 (SecureStoreFile is for tests/UT only).
    // Reuses the same "com.urlxl.mail" service name KDBusService and
    // UnifiedPushConnector already use above, for consistency.
    SecureStoreKeychain secureStore(QStringLiteral("com.urlxl.mail"));

    // Probe the secret store before anything depends on it.
    //
    // SecureStoreKeychain::set() returns false whenever no Secret Service
    // provider is reachable -- a bare WM session, a locked wallet, a Flatpak
    // on a host with neither gnome-keyring nor kwalletd. Every caller used
    // to discard that bool, so pairing "succeeded" while nothing reached
    // disk: the server minted and burned a one-shot deviceSecret, the next
    // launch was unpaired, and re-pairing then failed too because the
    // pairing token had already been consumed. DeviceRegistrationService now
    // reports that failure properly, but a one-line canary here means the
    // user learns before they try rather than after.
    const QString kSecureStoreCanaryKey = QStringLiteral("startup.canary");
    const bool secureStoreWritable = secureStore.set(kSecureStoreCanaryKey, QStringLiteral("1"))
        && secureStore.get(kSecureStoreCanaryKey).has_value();
    secureStore.remove(kSecureStoreCanaryKey);
    if (!secureStoreWritable) {
        qCritical("main: the system secret store is not writable -- pairing and the app lock "
                  "cannot persist. Start a keyring service (gnome-keyring or kwallet) and "
                  "relaunch KyPost.");
    }

    // 4. CursorStore -- reuses settingsDir (already computed for
    // SettingsStore above), not a second directory-resolution block.
    CursorStore cursorStore(settingsDir + QStringLiteral("/cursors.ini"));

    // 5. PairingStore -- the one shared "are we paired" contract every
    // repository below reads instead of re-deriving SecureStore key names.
    PairingStore pairingStore(secureStore);
    // App lock policy lives in SecureStore, NOT SettingsStore: settings.ini
    // is a plain text file, so storing `lockEnabled` there would let anyone
    // with file access simply switch the lock off -- the exact access level
    // the lock exists to survive. See AppLockStore.h.
    AppLockStore appLockStore(secureStore);

    // 6. HttpClient -- default transferTimeoutMs. networkManager must
    // outlive httpClient and every net/ client below that borrows it
    // transitively.
    QNetworkAccessManager networkManager;
    HttpClient httpClient(networkManager);

    // Enforce the certificate pin captured when this device paired (trust on
    // first use -- see DeviceRegistrationService::pair). Empty for a pairing
    // made before pinning existed, or over plain http in testing, in which
    // case enforcement stays off rather than bricking every request.
    if (const std::optional<DevicePairing> existingPairing = pairingStore.load();
        existingPairing.has_value() && !existingPairing->certificateSpkiSha256.isEmpty()) {
        httpClient.setCertificatePin(
            QByteArray::fromBase64(existingPairing->certificateSpkiSha256.toLatin1()));
    }

    // 7. core/net clients -- thin wire-format wrappers around httpClient.
    RelayMailSource relayMailSource(httpClient);
    ContactSyncClient contactSyncClient(httpClient);
    MfaResponseClient mfaResponseClient(httpClient);
    DeregisterClient deregisterClient(httpClient);
    NativeRegistrationClient nativeRegistrationClient(httpClient);
    // extended-contact-fields Task 2: GET /api/groups, this repo's first
    // per-resource GET client -- see core/net/GroupsClient.h.
    GroupsClient groupsClient(httpClient);
    FolderClient folderClient(httpClient);
    // extended-contact-fields Task 3: GET /api/contacts/{id}/photo, this
    // repo's second per-resource GET client -- see core/net/ContactPhotoClient.h.
    ContactPhotoClient contactPhotoClient(httpClient);
    // PGP QR key exchange: talks to /api/pgp/qr/token and /api/pgp/qr/key --
    // see core/net/PgpQrClient.h.
    PgpQrClient pgpQrClient(httpClient);
    // Encrypted send (client-encrypted-send Task 6): GET /api/pgp/bootstrap
    // (compose-control custody check) and POST /api/pgp/recipients/check
    // (inline keyless-recipient warning) -- see core/net/PgpBootstrapClient.h
    // and core/net/PgpRecipientChecker.h.
    PgpBootstrapClient pgpBootstrapClient(httpClient);
    PgpRecipientChecker pgpRecipientChecker(httpClient);

    // 8. core/domain repositories -- the layer Tasks 32-34's QML-facing
    // controllers actually call into.
    MailRepository mailRepository(relayMailSource, emailDao, pairingStore, cursorStore);
    FolderRepository folderRepository(folderClient, folderDao, pairingStore);
    KeywordRepository keywordRepository(settingsStore);
    // extended-contact-fields Task 2: refreshes groupDao from groupsClient
    // once per contact sync cycle -- wired into ContactsController::sync()
    // below, deliberately kept separate from ContactSyncRepository itself
    // (see GroupsRepository.h's doc comment for why).
    GroupsRepository groupsRepository(groupsClient, groupDao, pairingStore);
    // extended-contact-fields Task 3: lazy per-contact photo fetch+cache,
    // called on demand from ContactsController::photoPathFor() below (never
    // from sync()) -- see core/domain/ContactPhotoRepository.h's doc
    // comment.
    ContactPhotoRepository contactPhotoRepository(contactPhotoClient, contactPhotoCache, pairingStore);
    // PGP QR key exchange: wraps only the "My QR Code" token-fetch side,
    // which needs this device's own pairing resolved -- see
    // core/domain/PgpQrRepository.h's doc comment for why the key-fetch
    // side goes straight through pgpQrClient instead.
    PgpQrRepository pgpQrRepository(pgpQrClient, pairingStore);
    ContactSyncRepository contactSyncRepository(contactSyncClient, contactDao, pendingContactChangeDao,
                                                 cursorStore, pairingStore);
    DeviceRegistrationService deviceRegistrationService(nativeRegistrationClient, pairingStore, settingsStore,
                                                        httpClient);

    // 9. Task 41: the push domain graph deferred by the Phase 4 final-review
    // note above -- PushNotificationClient (over httpClient, same
    // construction pattern as RelayMailSource/ContactSyncClient just above),
    // NtfySubscriber (over networkManager), PushRepository (over pushDao/
    // cursorStore/the new PushNotificationClient/pairingStore/settingsStore,
    // all already constructed above), TransportStateMachine (over the new
    // NtfySubscriber+PushRepository), and NotificationDispatcher (Task 40,
    // app/push/ -- KNotifications wrapper, no core/ dependency).
    // PushPayloadParser (also Task 40) is a pure namespace, not a class --
    // no instance to construct.
    //
    // The actual signal wiring (UnifiedPushConnector's distributor-tier
    // arrivals/re-registration, TransportStateMachine's embedded-subscriber
    // and polling-tier arrivals, foreground/background reporting) lives
    // further down, after pushConnector (KUnifiedPush) is constructed --
    // see the comment block right before pushConnector's construction.
    //
    // Task 43 review-finding fix: NtfySubscriber's topic argument used to be
    // an empty QString() here (see task-41-report.md) -- nothing generated or
    // persisted a real one. NtfyTopicProvisioner::getOrCreateTopic()
    // (app/push/, new this task) reads the persisted topic from secureStore
    // (key "ntfy-topic", per SecureStore.h's own doc comment) if one already
    // exists, or generates a fresh >=128-bit random one and persists it
    // otherwise -- Linux_QT_Client_Plan.md's risk #8 design. Never log
    // ntfyTopic itself (it is a bearer secret on this path, same logging
    // discipline as endpoint URLs -- phase7-global-constraints.md item 6).
    //
    // Registering this topic with the backend as a deviceToken so the relay
    // actually knows where to publish (previously a documented gap here) is
    // now wired below via ntfyUrl + the tierChanged connection near
    // transportStateMachine's construction: deviceToken tracks whichever
    // tier is actually active (pushConnector.endpoint() for Distributor,
    // this URL for EmbeddedSubscriber), confirmed against the backend
    // (kypost-server's UnifiedPushSender.Send) treating deviceToken as an
    // arbitrary public URL to POST to under the existing
    // transport=unifiedpush value -- no backend change needed for ntfy.sh
    // itself to be that URL.
    PushNotificationClient pushNotificationClient(httpClient);
    // Empty when the topic could not be persisted (see
    // NtfyTopicProvisioner::rotateTopic). An unpersisted topic is worse than
    // none: the subscriber would listen on an address the backend was never
    // told about, so the EmbeddedSubscriber tier is left with no URL to
    // register rather than a plausible-looking dead one.
    const QString ntfyTopic = NtfyTopicProvisioner::getOrCreateTopic(secureStore);
    if (ntfyTopic.isEmpty())
        qWarning("main: no ntfy topic available -- the embedded push subscriber tier is disabled");
    // pushServerBaseUrl() defaults to "https://ntfy.sh" (no trailing slash)
    // but is user-configurable via SettingsStore::setPushServerBaseUrl(), so
    // this defensively strips one if present rather than assuming the
    // default's shape holds forever.
    QString ntfyBaseUrl = settingsStore.pushServerBaseUrl();
    while (ntfyBaseUrl.endsWith(QLatin1Char('/')))
        ntfyBaseUrl.chop(1);
    // Mutable and re-derived on rotation below -- it used to be const,
    // computed once at startup, so a topic rotated on re-pair left this
    // pointing at the old one until the next launch.
    QString ntfyUrl = ntfyTopic.isEmpty() ? QString() : ntfyBaseUrl + QLatin1Char('/') + ntfyTopic;
    NtfySubscriber ntfySubscriber(networkManager, settingsStore.pushServerBaseUrl(), ntfyTopic);
    PushRepository pushRepository(pushDao, cursorStore, pushNotificationClient, pairingStore, settingsStore);
    TransportStateMachine transportStateMachine(ntfySubscriber, pushRepository);
    NotificationDispatcher notificationDispatcher;

    // 10. Nothing above is registered with QML yet -- Tasks 32-34 each add
    // "construct controller X, register it" below, right above
    // QQmlApplicationEngine, without reordering anything in this block.

    // Task 32: QML-facing bridge over mailRepository/relayMailSource/
    // keywordRepository/pairingStore (all constructed above). Owns its
    // EmailListModel (parented to itself); every network-calling slot on
    // this controller blocks the GUI thread synchronously, same accepted
    // tradeoff as every other Phase 6 controller (see global constraint 2).
    MailController mailController(mailRepository, relayMailSource, keywordRepository, pairingStore,
                                   folderRepository, settingsStore, pgpBootstrapClient, pgpRecipientChecker);
    qmlRegisterSingletonInstance<MailController>(
        "com.urlxl.mail", 1, 0, "MailApp", &mailController);

    // App lock ("AppLock"). Registered here rather than beside
    // Theme/General above because AppLockStore is part of the composition
    // graph built in this section, not the app-shell preferences block.
    //
    // The sealer is injected rather than wired through a signal. See
    // core/security/CredentialSealer.h: the previous credentialGateChanged
    // signal could not report whether the seal/unseal it ordered actually
    // happened, which let the stored "credential gate on" flag coexist with
    // a plaintext secret, and let disableLock() erase the PIN while the
    // secret was still sealed under it -- an unrecoverable pairing.
    PairingStoreCredentialSealer credentialSealer(pairingStore);
    AppLockManager appLockManager(appLockStore, settingsStore, credentialSealer);
    qmlRegisterSingletonInstance<AppLockManager>(
        "com.urlxl.mail", 1, 0, "AppLock", &appLockManager);

    // Wipe on repeated PIN failure. AppLockManager knows only that the
    // threshold was crossed; deciding what "wipe" means is the host's job,
    // and it is deliberately everything an attacker could otherwise read:
    // cached mail/contacts, the pairing credential, and the lock itself
    // (leaving a PIN behind would be a hint about the wiped account).
    QObject::connect(&appLockManager, &AppLockManager::wipeRequested, &appLockManager,
                      [&database, &pairingStore, &appLockStore, &settingsStore, dataDir]() {
                          qWarning("App lock: wipe threshold reached, erasing local data");
                          database.wipeAllTables();
                          SecurityWipe::clearCacheDirectory(dataDir + QStringLiteral("/contact-photos"));
                          pairingStore.clear();
                          appLockStore.clear();
                          settingsStore.setDeliveryMode(QString());
                          // Relaunch into the now-empty, unpaired state
                          // rather than leaving a running window whose
                          // controllers still hold the pre-wipe view of the
                          // world. Same mechanism the HLP toggle uses.
                          AppRelauncher::requestRelaunch();
                      });

    // Sealing/unsealing the device secret is no longer wired here: it goes
    // through credentialSealer above, which AppLockManager calls directly so
    // it can act on the result. The three lambdas that used to live here
    // (credentialGateChanged / unlockedWithPin / relockRequested) all
    // discarded the bool their PairingStore call returned, which is exactly
    // how the gate flag and the real state of the secret could disagree.

    // Hostile Location Protection was toggled. The setting is already
    // persisted; this erases on-disk data when switching the mode ON, then
    // relaunches so the next process picks the right database.
    QObject::connect(&appLockManager, &AppLockManager::relaunchRequired, &appLockManager,
                      [&database, newDbPath, dataDir](bool wipeDisk) {
                          if (wipeDisk) {
                              // Empty the tables first: the connection is
                              // still open, so the file cannot be removed
                              // reliably yet, and this guarantees the content
                              // is gone even if the unlink below loses a race
                              // with a straggling writer.
                              database.wipeAllTables();
                              SecurityWipe::removeDatabaseFiles(newDbPath);
                              SecurityWipe::clearCacheDirectory(dataDir + QStringLiteral("/contact-photos"));
                          }
                          AppRelauncher::requestRelaunch();
                      });

    // Task 42: notification tap-through. NotificationDispatcher (Task 40,
    // already constructed above as part of the Task 41 push graph) emits
    // openRequested(messageId) when the user activates a KNotification's
    // "View" action; forwarded straight to MailController::openEmailRequested
    // -- a direct signal-to-signal connect, no lambda needed, since both
    // signals already carry the same (const QString&) shape. Neither
    // NotificationDispatcher nor MailController has window/pageStack access
    // (by design -- see both classes' doc comments), so this connect only
    // gets the bare messageId to QML; MobileRoot.qml/DesktopRoot.qml each
    // have their own `Connections { target: MailApp }` block that hydrates
    // the full email via MailApp.findByMessageId() and does the actual
    // navigation + window raise/focus. Deliberately NOT connected here for
    // NotificationDispatcher::notify() itself (Task 41's Step 3 wiring,
    // above) -- a background push arriving must only show the KNotification,
    // never steal focus; only this openRequested (a genuine user click) may
    // ever result in a window raise, and even that raise happens on the QML
    // side, not here.
    QObject::connect(&notificationDispatcher, &NotificationDispatcher::openRequested, &mailController,
                      &MailController::openEmailRequested);

    // Task 33: QML-facing bridge over contactSyncRepository (constructed
    // above). Owns its ContactListModel (parented to itself); sync() blocks
    // the GUI thread synchronously, same accepted tradeoff as MailController
    // above (see global constraint 2). Its model starts empty until QML
    // calls load()/sync() -- see ContactsController's constructor comment.
    ContactsController contactsController(contactSyncRepository, groupsRepository, contactPhotoRepository);
    qmlRegisterSingletonInstance<ContactsController>(
        "com.urlxl.mail", 1, 0, "ContactsApp", &contactsController);

    // PGP QR key exchange: QML-facing bridge over pgpQrRepository/
    // pgpQrClient (both constructed above). Persistence of a scanned key
    // onto a contact happens entirely in QML, gluing this singleton to
    // ContactsApp -- see PgpQrController.h's doc comment.
    PgpQrController pgpQrController(pgpQrRepository, pgpQrClient);
    qmlRegisterSingletonInstance<PgpQrController>(
        "com.urlxl.mail", 1, 0, "PgpQr", &pgpQrController);
    // This repo's first creatable (non-singleton) QML-registered C++ type --
    // PgpScanContactKey.qml instantiates one per scan screen, attaching it
    // to the live VideoOutput's videoSink (see PgpQrScanner.h's doc comment).
    qmlRegisterType<PgpQrScanner>("com.urlxl.mail", 1, 0, "PgpQrScanner");

    // VibeSec fix: EmailDetail.qml instantiates one of these per its
    // WebEngineProfile, binding its imagesLoaded property to the "Show
    // images" toggle -- see RemoteContentInterceptor.h's class doc comment.
    qmlRegisterType<RemoteContentInterceptor>("com.urlxl.mail", 1, 0, "RemoteContentInterceptor");

    // Task 34: QML-facing bridge over deviceRegistrationService/pairingStore
    // (both constructed above). Refreshes its isPaired/pairedServerHost/
    // deviceId from pairingStore eagerly on construction (see its
    // constructor comment) -- unlike Mail/ContactsController, there's no
    // reasonable "empty until QML asks" state for "are we paired".
    // Task 39: also takes settingsStore directly (constructed at the very
    // top of main()) so its read-only deliveryMode()/transport()/
    // pushServerBaseUrl() properties (Settings > Notifications) can read
    // straight from it -- see PairingController.h's doc comment on why
    // those three reuse pairingChanged() rather than a new signal.
    PairingController pairingController(deviceRegistrationService, pairingStore, settingsStore, deregisterClient);
    qmlRegisterSingletonInstance<PairingController>(
        "com.urlxl.mail", 1, 0, "Pairing", &pairingController);

    // Keep PairingController's view of the lock current, so a
    // kypost://native-pair link arriving while the app is locked cannot raise a
    // confirmable pairing prompt over the PIN screen. The confirm prompt is a
    // QQC2 Popup and therefore renders inside QQuickOverlay, above the app-lock
    // overlay's z: 1000 -- gating in the controller makes that stacking
    // irrelevant. Seeded once here because the app can start locked, then
    // pushed on every transition; both the argv deep-link path and the
    // KDBusService::activateRequested relay are covered by construction, since
    // both funnel through pairFromDeepLink().
    pairingController.setAppLocked(appLockManager.locked());
    QObject::connect(&appLockManager, &AppLockManager::lockedChanged, &pairingController,
                     [&pairingController, &appLockManager]() {
                         pairingController.setAppLocked(appLockManager.locked());
                     });

    // Task 43 review-finding fix: rotate the ntfy topic (SecureStore
    // "ntfy-topic" key, see NtfyTopicProvisioner above) on every successful
    // (re-)pair, per Linux_QT_Client_Plan.md's risk #8 ("rotated on
    // re-pair"). pairingStateChanged() is PairingController's own
    // already-existing signal for exactly this transition -- it fires from
    // pairFromParsedParams() on every outcome, so the state is checked here
    // rather than adding new re-pair-specific machinery to PairingController
    // itself (out of this task's scope per its brief: wire an existing hook,
    // don't build one). Note this only rotates the *persisted* secret --
    // ntfySubscriber above was already constructed with whatever topic
    // existed at startup and has no live topic-update seam (changing that
    // would touch core/net/NtfySubscriber's core logic, also out of scope),
    // so a rotation here takes effect starting from the next app launch, not
    // mid-session.
    //
    // The rotation now also reaches the LIVE subscriber and the URL that
    // gets registered as the deviceToken, via NtfySubscriber::setTopic().
    // Previously only the persisted value changed, so "rotated on re-pair"
    // was true of the stored secret and false of the running connection
    // until the next launch.
    QObject::connect(&pairingController, &PairingController::pairingStateChanged, &pairingController,
                      [&pairingController, &secureStore, &ntfySubscriber, &ntfyUrl, ntfyBaseUrl]() {
                          if (pairingController.state() != PairingController::State::Paired)
                              return;
                          const QString rotated = NtfyTopicProvisioner::rotateTopic(secureStore);
                          if (rotated.isEmpty()) {
                              // Could not persist the new topic. Keep the
                              // old one running rather than switching the
                              // live subscriber to an address the backend
                              // will never be told about.
                              qWarning("main: ntfy topic rotation failed, keeping the previous topic");
                              return;
                          }
                          ntfyUrl = ntfyBaseUrl + QLatin1Char('/') + rotated;
                          ntfySubscriber.setTopic(rotated);
                      });

    // pairingController now exists -- point the pointer the KDBusService
    // activateRequested lambda above captured (by reference) at it, so a
    // second launch relaying a kypost://native-pair link over D-Bus can
    // actually reach PairingController. See that lambda's comment for why
    // this ordering is safe.
    pairingControllerForDeepLinks = &pairingController;

    // Task 34: this process is the one that "won" KDBusService's Unique-mode
    // registration (construction above already guarantees that -- a losing
    // instance relays its argv and exits at that point, it doesn't reach
    // here), so also check its own argv for a kypost:// URL -- covers
    // the case where xdg-open launches kypost fresh (nothing was running
    // yet to redirect to). Moved here (versus immediately after KDBusService,
    // where the Task 12 stub ran this) so PairingController already exists
    // to route a native-pair link to.
    routeDeepLink(app.arguments(), pairingControllerForDeepLinks);

    // Task 34: QML-facing bridge over mfaResponseClient/pairingStore (both
    // constructed above).
    MfaController mfaController(mfaResponseClient, pairingStore);
    qmlRegisterSingletonInstance<MfaController>(
        "com.urlxl.mail", 1, 0, "Mfa", &mfaController);

    QQmlApplicationEngine engine;

    // Task 48: makes i18n()/i18nc()/i18np()/i18ncp() (and their xi18n* KUIT
    // counterparts) callable from every QML file loaded by this engine, with
    // no `import` statement needed -- KLocalization::setupLocalizedContext()
    // (KF6::I18nQml) constructs a KLocalizedQmlContext and installs it as
    // engine.rootContext()'s context object, then also sets it as that
    // context's own QQmlEngine (via QQmlEngine::setContextForObject(),
    // internally) so property bindings using these functions re-evaluate on
    // a runtime language change. Must run before engine.load() below --
    // QML resolves i18n() through the root context at parse/binding time,
    // so a context object set afterwards would be too late for
    // MobileRoot.qml's very first frame. Deliberately not the older
    // KLocalizedContext class (see app/CMakeLists.txt's KF6::I18nQml comment
    // for why: deprecated since 6.8, this is its replacement).
    KLocalization::setupLocalizedContext(&engine);

    // Convergent root selection: picks MobileRoot.qml vs DesktopRoot.qml
    // (Tasks 38/39) directly, rather than loading a QML-side Loader-based
    // dispatcher -- a Loader-wrapped root was tried first and reproducibly
    // broke window visibility under this Wayland/Kirigami stack (the QML
    // content loads and runs fine underneath it, but no window ever gets
    // mapped), confirmed by comparing two otherwise-identical fresh builds
    // that differed only in root-vs-Loader. Deciding here mirrors
    // Kirigami.Settings.isMobile's own primary override signal
    // (QT_QUICK_CONTROLS_MOBILE) without needing its QML-singleton-only
    // Settings class from C++: a real Plasma Mobile session also flips this
    // env var on for Qt Quick Controls apps, so this covers the same cases
    // Linux_QT_Client_Plan.md's "Root selection" section called for. `isMobile`
    // itself is resolved earlier, right after settingsStore is constructed
    // (see that comment for the persisted-preference-over-env-var precedence).
    engine.load(QUrl(isMobile ? QStringLiteral("qrc:/qml/MobileRoot.qml")
                               : QStringLiteral("qrc:/qml/DesktopRoot.qml")));
    if (engine.rootObjects().isEmpty())
        return -1;

    // Desktop-only system tray icon (see GeneralController.trayIconEnabled /
    // minimizeToTrayOnClose, and DesktopRoot.qml's onClosing handler). Never
    // constructed in Mobile mode -- there is no tray concept there.
    std::optional<TrayController> trayController;
    if (!isMobile) {
        auto* rootWindow = qobject_cast<QWindow*>(engine.rootObjects().constFirst());
        trayController.emplace(rootWindow, generalController);
    }

    // Task 41: UnifiedPushConnector is now a real emitting wrapper (see
    // app/push/UnifiedPushConnector.h/.cpp) -- the live entry point for the
    // Distributor tier of the three-tier push pipeline
    // (core/domain/TransportStateMachine.h: Distributor -> EmbeddedSubscriber
    // -> Polling). registerClient() below (moved to just before app.exec(),
    // after every wiring connection below is already in place) is safe to
    // call on every startup -- KUnifiedPush persists registration state
    // itself.
    UnifiedPushConnector pushConnector(QStringLiteral("com.urlxl.mail"));

    // Distributor-tier availability: only KUnifiedPush::Connector::Registered
    // means "available" (phase7-global-constraints.md item 5) -- Registering
    // is transient, Unregistered/NoDistributor/Error all mean unavailable.
    // This is the only TransportStateMachine input pushConnector drives --
    // the Distributor tier's own message arrivals are wired directly below,
    // never routed through TransportStateMachine (constraint item 4).
    QObject::connect(&pushConnector, &UnifiedPushConnector::stateChanged, &pushConnector,
                      [&transportStateMachine](KUnifiedPush::Connector::State state) {
                          // No secrets in a connector state value -- safe to log in full,
                          // same as Task 10's original stub did.
                          qDebug() << "main: UnifiedPushConnector state changed:" << state;
                          transportStateMachine.setDistributorAvailable(state == KUnifiedPush::Connector::Registered);
                      });

    // Re-registers the already-paired device with the relay whenever the
    // distributor hands out a new (or rotated) endpoint. reregisterIfPaired()
    // is a documented no-op when there is no stored pairing yet
    // (DeviceRegistrationService.cpp: returns std::nullopt without calling
    // the client, confirmed by reading it) -- first-time pairing stays the
    // existing Pairing.qml/kypost://native-pair flow from Phase 6 and
    // is never triggered from here, even though this signal can fire before
    // the user has ever paired.
    //
    // Also feeds PairingController::setDeviceToken() (Task 43 fix): the
    // backend's deviceToken field is required on the FIRST pairing request
    // too, not just re-registration -- discovered live (a real pairing
    // attempt failed with HTTP 400) since pairingController previously
    // always sent an empty QString() on first pair. pairingController is
    // constructed above (before pushConnector exists), so this is the same
    // late-bound wiring shape as main.cpp's existing
    // pairingControllerForDeepLinks pointer.
    //
    // The result is now inspected rather than discarded. AGENTS.md section 8
    // flags this exact case in bold ("Re-registration silently 401s once the
    // pairing token expires ... handle the 401 explicitly"), and both call
    // sites used to throw the std::optional away -- leaving the relay
    // publishing to a dead endpoint behind a UI that still said "Paired".
    const auto reregisterAndReport = [&deviceRegistrationService,
                                       &pairingController](const QString& endpoint) {
        const std::optional<NativeRegistrationResult> result =
            deviceRegistrationService.reregisterIfPaired(endpoint);
        if (!result.has_value())
            return; // not paired yet -- an ordinary state, not a failure
        if (result->outcome == RegistrationOutcome::Unauthorized) {
            qWarning("main: re-registration was rejected (401) -- the pairing token has expired, "
                     "push delivery is now broken until the user pairs again");
            pairingController.setReregistrationRejected(true);
        }
    };

    QObject::connect(&pushConnector, &UnifiedPushConnector::endpointChanged, &pushConnector,
                      [&pairingController, reregisterAndReport](const QString& endpoint) {
                          reregisterAndReport(endpoint);
                          pairingController.setDeviceToken(endpoint);
                      });
    // Apply whatever endpoint is already known (if any) immediately, same
    // reasoning as applicationStateChanged's initial-state call below --
    // endpointChanged only fires on a future *change*, so a pairing attempt
    // between now and the first real change would otherwise still see an
    // empty token even though pushConnector may already hold a valid one
    // from a prior run (KUnifiedPush persists registration across
    // restarts).
    pairingController.setDeviceToken(pushConnector.endpoint());

    // Distributor-tier arrival path -- independent of TransportStateMachine,
    // per constraint item 4. Parses the raw UnifiedPush message bytes with
    // Task 40's PushPayloadParser; on success, persists+dedupes via
    // PushRepository::recordPushArrival() and hands the parsed payload to
    // NotificationDispatcher; on failure, logs byte count only (never
    // content) and does nothing else.
    //
    // PushPayloadParser::parse() (Task 43 fix) also accepts the backend's
    // sparser /api/notifications/test envelope, which has no messageId --
    // PushDao/PushRepository::recordPushArrival() treat messageId as the
    // required identity key, so persistence is skipped for a keyless
    // result; the notification is still shown either way.
    QObject::connect(&pushConnector, &UnifiedPushConnector::messageReceived, &pushConnector,
                      [&pushRepository, &notificationDispatcher](const QByteArray& message) {
                          const std::optional<PushNotification> payload = PushPayloadParser::parse(message);
                          if (!payload.has_value()) {
                              qWarning() << "main: UnifiedPushConnector::messageReceived: failed to parse push"
                                            " payload,"
                                         << message.size() << "bytes";
                              return;
                          }
                          if (!payload->messageId.isEmpty())
                              pushRepository.recordPushArrival(*payload, QDateTime::currentMSecsSinceEpoch());
                          notificationDispatcher.notify(*payload);
                      });

    // EmbeddedSubscriber-tier arrival path. NtfySubscriber emits ntfy's own
    // flat JSON-stream envelope (id/time/event/topic/title/message -- see
    // core/net/NtfySubscriber.cpp's processLine() and
    // tests/core/net/NtfySubscriberTest.cpp's fixtures), which is NOT the
    // nested {title,body,data:{messageId,...}} envelope PushPayloadParser.h
    // documents -- the two shapes were confirmed different by reading both,
    // so this maps ntfy's fields directly instead of round-tripping through
    // PushPayloadParser (see task-41-report.md for the full analysis: ntfy's
    // own message id is the only reasonable messageId source here, and the
    // custom data.* fields this app cares about are not necessarily
    // preserved by a real ntfy relay). Unlike the Polling tier below,
    // nothing upstream of this signal persists the arrival -- NtfySubscriber
    // has no PushDao, and TransportStateMachine's own notificationReceived
    // forwarding in its .cpp constructor does no persistence either
    // (verified by reading both files) -- so this lambda calls
    // recordPushArrival() itself, mirroring the Distributor-tier lambda
    // above.
    QObject::connect(&transportStateMachine, &TransportStateMachine::notificationReceived, &transportStateMachine,
                      [&pushRepository, &notificationDispatcher](const QJsonObject& data) {
                          PushNotification payload;
                          payload.messageId = data.value(QStringLiteral("id")).toString();
                          payload.title = data.value(QStringLiteral("title")).toString();
                          payload.body = data.value(QStringLiteral("message")).toString();
                          pushRepository.recordPushArrival(payload, QDateTime::currentMSecsSinceEpoch());
                          notificationDispatcher.notify(payload);
                      });

    // Polling-tier arrivals: PushRepository::pullOnce() (invoked internally
    // by TransportStateMachine's own poll timer) already persists each
    // delivered item before this signal fires (core/domain/
    // PushRepository.cpp's pullOnce() calls m_pushDao.insertOrReplace() per
    // item) -- do not call recordPushArrival() again here.
    QObject::connect(&transportStateMachine, &TransportStateMachine::pollTick, &transportStateMachine,
                      [&notificationDispatcher](const QVector<PushNotification>& delivered) {
                          for (const PushNotification& payload : delivered)
                              notificationDispatcher.notify(payload);
                      });

    // Observability only -- TransportStateMachine.cpp itself has no logging
    // of its own tier transitions (and constraint item 4/task brief says
    // don't touch that file), so this is the only place a developer can see
    // Distributor/EmbeddedSubscriber/Polling transitions in the journal.
    // TransportTier is a plain "enum class" (no Q_ENUM), hence the int cast
    // rather than relying on a QDebug enum streaming operator that doesn't
    // exist for it.
    QObject::connect(&transportStateMachine, &TransportStateMachine::tierChanged, &transportStateMachine,
                      [](TransportTier tier) {
                          qDebug() << "main: TransportStateMachine tier changed:" << static_cast<int>(tier);
                      });

    // Registers whichever endpoint the now-active tier can actually be
    // reached at, closing the gap flagged in the comment above ntfyUrl's
    // construction. Distributor and EmbeddedSubscriber each have a real
    // address to register; Polling has none of its own, so the
    // previously-registered token (from the last Distributor/
    // EmbeddedSubscriber tier this session saw) is left alone rather than
    // registering nothing, since a stale-but-real address at least has a
    // chance of receiving a push again once that tier returns, and
    // deviceToken has no meaningful "unset" wire value to fall back to.
    QObject::connect(&transportStateMachine, &TransportStateMachine::tierChanged, &transportStateMachine,
                      [&pairingController, &pushConnector, &ntfyUrl, reregisterAndReport](TransportTier tier) {
                          switch (tier) {
                          case TransportTier::Distributor:
                              reregisterAndReport(pushConnector.endpoint());
                              pairingController.setDeviceToken(pushConnector.endpoint());
                              break;
                          case TransportTier::EmbeddedSubscriber:
                              // Empty when no topic could be persisted --
                              // registering an empty deviceToken would just
                              // overwrite a working one with nothing.
                              if (ntfyUrl.isEmpty())
                                  break;
                              reregisterAndReport(ntfyUrl);
                              pairingController.setDeviceToken(ntfyUrl);
                              break;
                          case TransportTier::Polling:
                              break;
                          }
                      });

    // Foreground/background is app-layer-owned state TransportStateMachine
    // needs (constraint item 4's setForegrounded input) -- QGuiApplication
    // already tracks this natively, no QML binding needed.
    // TransportStateMachine::m_foregrounded defaults to false (see its
    // header), so an initial call right after the connection above is
    // required even though nothing has "changed" yet -- otherwise a launch
    // that starts already-active would incorrectly stay treated as
    // backgrounded until the next real focus event.
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app,
                      [&transportStateMachine](Qt::ApplicationState state) {
                          qDebug() << "main: applicationStateChanged:" << state;
                          transportStateMachine.setForegrounded(state == Qt::ApplicationActive);
                      });
    transportStateMachine.setForegrounded(app.applicationState() == Qt::ApplicationActive);

    pushConnector.registerClient(QStringLiteral("KyPost push notifications"));

    const int exitCode = app.exec();

    // A clean quit should leave nothing behind, rather than relying on
    // five-minute timers that will never fire now the loop has stopped.
    mailController.clearEphemeralAttachments();

    // Relaunch AFTER the event loop has returned, never from inside it.
    // KDBusService(Unique) holds the com.urlxl.mail well-known name for this
    // process's lifetime; a child spawned while the parent still owned it
    // would be treated as a duplicate launch, activate the dying parent and
    // exit -- leaving the user staring at a closed app. Sequencing it here
    // removes that race by construction rather than papering over it with a
    // sleep. See app/security/AppRelauncher.h.
    // A failed relaunch is reported, not swallowed: the two paths that reach
    // it (Hostile Location Protection, and the wipe-after-10-failures reset)
    // both just destroyed local state, so a silent disappearance is the
    // worst possible ending.
    if (!AppRelauncher::performPendingRelaunch())
        return exitCode != 0 ? exitCode : 1;
    return exitCode;
}
