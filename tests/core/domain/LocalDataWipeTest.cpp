#include "domain/LocalDataWipe.h"

#include "stores/CursorStore.h"

#include "db/Database.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "security/AppLockStore.h"
#include "security/DatabaseKeyStore.h"
#include "stores/SecureStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

namespace {

// A SecureStore whose removals silently fail -- what a locked wallet or an
// absent Secret Service provider looks like. The wipe must NOTICE.
class UnremovableSecureStore : public SecureStore
{
public:
    bool set(const QString& key, const QString& value) override
    {
        m_values.insert(key, value);
        return true;
    }
    std::optional<QString> get(const QString& key) const override
    {
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool remove(const QString&) override { return false; }
    bool contains(const QString& key) const override { return m_values.contains(key); }

private:
    QHash<QString, QString> m_values;
};

QString writeDummyDatabase(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {}; // the caller's QVERIFY(QFile::exists(...)) reports it
    f.write("SQLite format 3\0not really, but it is a file with bytes in it", 60);
    f.close();
    return path;
}

} // namespace

// Covers the two data-destruction paths that used to live as lambdas inside
// main() and therefore had no test at all: the wipe after ten failed PIN
// attempts, and Hostile Location Protection being switched on.
//
// That absence was not theoretical. The pre-rename database (llamamail.db)
// was missing from BOTH handlers for months, so each reported a completed
// wipe while a byte-identical plaintext copy of every cached message and
// contact stayed on disk. Nothing could have caught it: main() is one
// unbroken chain of stack locals with no seam a test can reach.
class LocalDataWipeTest : public QObject
{
    Q_OBJECT

private slots:
    void wipeEverythingClearsCachesPairingAndLock();
    void wipeEverythingErasesTheSyncCursors();
    void wipeEverythingTakesThePreRenameDatabasesToo();
    void wipeEverythingUnlinksTheLiveDatabaseAndReopensAWritableOne();
    void wipeEverythingLeavesNoDatabaseFileWhenTheSessionWasInMemory();
    void wipeEverythingReportsAnUnremovablePairingCredential();
    void hostileLocationWipeUnlinksTheLiveDatabaseButKeepsThePairing();
    void hostileLocationWipeTakesTheDatabaseKeyWithTheFile();
    void wipeEverythingReportsAnUnremovableDatabaseKey();
    void accountReplacementKeepsTheDatabaseAndItsKey();
};

void LocalDataWipeTest::wipeEverythingClearsCachesPairingAndLock()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));
    QSqlQuery insert(database.handle());
    QVERIFY(insert.exec(QStringLiteral("INSERT INTO emails (message_id, folder, sender, subject, at_utc) "
                                        "VALUES ('m1', 'INBOX', 'a@b.c', 'hello', '2026-01-01T00:00:00Z')")));

    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));

    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("super-secret");
    QVERIFY(pairingStore.save(pairing));
    QVERIFY(appLockStore.setPin(QStringLiteral("419273")));

    // A contact photo on disk, which is cached mail-adjacent content.
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("contact-photos"))));
    writeDummyDatabase(dir.filePath(QStringLiteral("contact-photos/abc.jpg")));

    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("4242")));
    QVERIFY(cursorStore.setNotificationCursor(77));
    DatabaseKeyStore databaseKeyStore(secureStore);
    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeEverything();
    QVERIFY(result.complete());

    QSqlQuery count(database.handle());
    QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM emails")));
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 0);

    QVERIFY(!pairingStore.isPaired());
    QVERIFY(!appLockStore.lockEnabled());
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("contact-photos/abc.jpg"))));
}

// The regression that motivated extracting this at all.
void LocalDataWipeTest::wipeEverythingTakesThePreRenameDatabasesToo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));

    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));

    // Two candidates, matching main()'s legacyDbPaths: one beside the new
    // database, one in the pre-rename data directory.
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("LlamaMail"))));
    const QStringList legacy = {
        writeDummyDatabase(dir.filePath(QStringLiteral("llamamail.db"))),
        writeDummyDatabase(dir.filePath(QStringLiteral("LlamaMail/llamamail.db"))),
    };
    // ...and their sidecars, which can hold committed pages of their own.
    writeDummyDatabase(dir.filePath(QStringLiteral("llamamail.db-wal")));
    writeDummyDatabase(dir.filePath(QStringLiteral("llamamail.db-shm")));

    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("4242")));
    QVERIFY(cursorStore.setNotificationCursor(77));
    DatabaseKeyStore databaseKeyStore(secureStore);
    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, legacy);
    const LocalDataWipeResult result = wipe.wipeEverything();
    QVERIFY(result.legacyDatabasesRemoved);

    for (const QString& path : legacy)
        QVERIFY2(!QFile::exists(path), qPrintable(path));
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("llamamail.db-wal"))));
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("llamamail.db-shm"))));
}

// The database file and the key that decrypts it go together, and what
// replaces them has to be a profile the NEXT launch can read.
//
// This path used to keep the file for the relaunch to reopen, which stopped
// being possible once the key was erased with it. Unlinking alone is not
// enough either: the connection goes on answering queries against an inode
// with no name, so a session that keeps running -- which is exactly what
// TrackedWipe::recoverIfInterrupted() does, see TrackedWipeTest -- has no
// database anything can be kept in. Asserting that the old connection still
// answers a SELECT proves nothing; it answers precisely BECAUSE the unlinked
// inode is still alive. So the check here is durability: write through the
// post-wipe connection, then read it back through a separate Database that
// finds the file by NAME.
void LocalDataWipeTest::wipeEverythingUnlinksTheLiveDatabaseAndReopensAWritableOne()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));

    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("4242")));
    QVERIFY(cursorStore.setNotificationCursor(77));
    DatabaseKeyStore databaseKeyStore(secureStore);
    const QByteArray keyBefore = databaseKeyStore.create();
    QVERIFY(!keyBefore.isEmpty());
    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeEverything();
    QVERIFY(result.complete());
    QVERIFY(result.databaseKeyCleared);
    QVERIFY(result.currentDatabaseRemoved);

    // The pre-wipe key is gone. Whatever sits on that path now, it is not
    // the database this wipe was asked to destroy.
    const DatabaseKeyStore::Result keyAfter = databaseKeyStore.existing();
    QVERIFY(keyAfter.key != keyBefore);

    // The durability check comes FIRST, before anything the wipe reported
    // about itself: it is the property that matters, and it is the one an
    // assertion on the still-open connection cannot see.
    QSqlQuery insert(database.handle());
    QVERIFY(insert.exec(QStringLiteral("INSERT INTO emails (message_id, folder, at_utc) "
                                        "VALUES ('after-wipe', 'INBOX', '2026-01-01')")));

    Database nextLaunch;
    QVERIFY(nextLaunch.open(dbPath,
                             keyAfter.status == DatabaseKeyStore::Status::Found ? keyAfter.key : QByteArray()));
    QSqlQuery readBack(nextLaunch.handle());
    QVERIFY(readBack.exec(QStringLiteral("SELECT COUNT(*) FROM emails WHERE message_id = 'after-wipe'")));
    QVERIFY(readBack.next());
    QVERIFY2(readBack.value(0).toInt() == 1,
              "the post-wipe session wrote into an unlinked inode; the next launch cannot see it");

    QVERIFY(result.databaseReopenedAs.has_value());
    QVERIFY(*result.databaseReopenedAs != ProfileDatabaseMode::FailedToOpen);
}

// The mirror image, and the reason the reopen is conditional. Under Hostile
// Location Protection -- or a data directory that could not be made
// owner-only -- main() opens ":memory:" and never touches the on-disk path,
// while LocalDataWipe still gets that path so a leftover file is erased.
// Reopening there would CREATE the very file those modes exist to keep off
// this disk.
void LocalDataWipeTest::wipeEverythingLeavesNoDatabaseFileWhenTheSessionWasInMemory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(QStringLiteral(":memory:")));
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));
    DatabaseKeyStore databaseKeyStore(secureStore);

    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeEverything();

    QVERIFY(result.complete());
    QVERIFY2(!result.databaseReopenedAs.has_value(),
              "a session running in memory was handed an on-disk profile by the wipe");
    QVERIFY2(!QFile::exists(dbPath), "the wipe created a database file on a disk that must hold none");
    QCOMPARE(databaseKeyStore.existing().status, DatabaseKeyStore::Status::Absent);
}

// "Wiped" while the device secret is still in the keychain is a materially
// different situation, and the caller can only say so if this reports it.
void LocalDataWipeTest::wipeEverythingReportsAnUnremovablePairingCredential()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));

    UnremovableSecureStore secureStore;
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));

    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("super-secret");
    QVERIFY(pairingStore.save(pairing));

    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("4242")));
    QVERIFY(cursorStore.setNotificationCursor(77));
    DatabaseKeyStore databaseKeyStore(secureStore);
    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeEverything();

    QVERIFY(!result.pairingCleared);
    QVERIFY(!result.complete());
    // The caches still went, which is why this is a per-step result rather
    // than one bool: the caller reports precisely what survived.
    QVERIFY(result.tablesWiped);
}

void LocalDataWipeTest::hostileLocationWipeUnlinksTheLiveDatabaseButKeepsThePairing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));

    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));

    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("super-secret");
    QVERIFY(pairingStore.save(pairing));
    QVERIFY(appLockStore.setPin(QStringLiteral("419273")));

    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));
    QVERIFY(cursorStore.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("4242")));
    QVERIFY(cursorStore.setNotificationCursor(77));
    DatabaseKeyStore databaseKeyStore(secureStore);
    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeOnDiskDataOnly();
    QVERIFY(result.complete());

    // Nothing left on disk -- the next launch opens ":memory:".
    QVERIFY(!QFile::exists(dbPath));

    // But the user is not being wiped: they are still paired and still
    // locked. Taking their credential here would be a bug, not a feature.
    QVERIFY(pairingStore.isPaired());
    QVERIFY(appLockStore.lockEnabled());
}

// Unlinking the database while leaving its key in the keyring erases nothing
// an attacker cannot undo: a backup, a removal that lost a race, or the
// unallocated blocks the file used to occupy are all still readable by
// whoever finds the key that is still sitting there.
void LocalDataWipeTest::hostileLocationWipeTakesTheDatabaseKeyWithTheFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));

    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));

    DatabaseKeyStore databaseKeyStore(secureStore);
    QVERIFY(!databaseKeyStore.create().isEmpty());
    QCOMPARE(databaseKeyStore.existing().status, DatabaseKeyStore::Status::Found);

    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeOnDiskDataOnly();

    QVERIFY(result.databaseKeyCleared);
    QVERIFY(result.complete());
    QVERIFY(!QFile::exists(dbPath));
    QCOMPARE(databaseKeyStore.existing().status, DatabaseKeyStore::Status::Absent);
}

// A key that could not be removed is REPORTED, never assumed gone -- same
// reason the pairing credential is. The keyring still names an account this
// device was told to forget.
void LocalDataWipeTest::wipeEverythingReportsAnUnremovableDatabaseKey()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));

    UnremovableSecureStore secureStore;
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));

    DatabaseKeyStore databaseKeyStore(secureStore);
    QVERIFY(!databaseKeyStore.create().isEmpty());

    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeEverything();

    QVERIFY(!result.databaseKeyCleared);
    QVERIFY(!result.complete());
    QCOMPARE(databaseKeyStore.existing().status, DatabaseKeyStore::Status::Found);
    // The rest of the wipe still ran: this is a per-step result so the caller
    // can say exactly what survived.
    QVERIFY(result.tablesWiped);
}

// The one path that must NOT touch the key: the app goes on running on this
// connection and syncs the new account into it. Taking the key here would
// leave an encrypted file nothing can open again.
void LocalDataWipeTest::accountReplacementKeepsTheDatabaseAndItsKey()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));

    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    CursorStore cursorStore(dir.filePath(QStringLiteral("cursors.ini")));

    DatabaseKeyStore databaseKeyStore(secureStore);
    const QByteArray key = databaseKeyStore.create();
    QVERIFY(!key.isEmpty());

    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    QVERIFY(wipe.wipeCachedAccountData().complete());

    QVERIFY(QFile::exists(dbPath));
    const DatabaseKeyStore::Result stored = databaseKeyStore.existing();
    QCOMPARE(stored.status, DatabaseKeyStore::Status::Found);
    QCOMPARE(stored.key, key);
}


// cursors.ini survived every wipe path: CursorStore::reset() existed and had
// no caller anywhere in the app. It holds no mail content, but it is a plain
// INI file naming the subscriber id and every mailbox this device ever
// synced -- and since mail cursors became per (subscriber, folder) it names
// strictly more of them than before.
//
// It also has to go for correctness: the wipe empties the mail tables, so a
// surviving cursor would have the next sync request a delta against a cache
// that no longer exists, and everything before that cursor would never be
// re-fetched.
void LocalDataWipeTest::wipeEverythingErasesTheSyncCursors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    Database database;
    QVERIFY(database.open(dbPath));

    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    AppLockStore appLockStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));

    // Seeded through a CursorStore that is then destroyed, so the values are
    // genuinely FLUSHED to disk before the wipe runs -- which is the real
    // situation: cursors.ini is a file left behind by a previous session.
    // Writing and asserting through one live QSettings would have proved
    // nothing; the first version of this test did exactly that and passed
    // while the file did not yet exist.
    const QString cursorsPath = dir.filePath(QStringLiteral("cursors.ini"));
    {
        CursorStore seed(cursorsPath);
        QVERIFY(seed.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("4242")));
        QVERIFY(seed.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("Work/Legal"), QStringLiteral("99")));
        QVERIFY(seed.setContactBaseCursor(QStringLiteral("rev-7")));
        QVERIFY(seed.setNotificationCursor(77));
    }

    {
        QFile before(cursorsPath);
        QVERIFY2(before.exists(), "the seeded cursors must be on disk before the wipe");
        QVERIFY(before.open(QIODevice::ReadOnly));
        const QByteArray seeded = before.readAll();
        QVERIFY(seeded.contains("Legal"));
        QVERIFY(seeded.contains("sub-1"));
        QVERIFY(seeded.contains("4242"));
    }

    CursorStore cursorStore(cursorsPath);
    QCOMPARE(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")), QStringLiteral("4242"));

    DatabaseKeyStore databaseKeyStore(secureStore);
    LocalDataWipe wipe(database, databaseKeyStore, pairingStore, appLockStore, settingsStore, cursorStore,
                        dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeEverything();

    QVERIFY(result.syncCursorsCleared);
    QVERIFY(result.complete());

    // Every cursor, including the notification one reset() deliberately
    // spares -- a wipe is not a tooOld reconciliation.
    QVERIFY(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    QVERIFY(cursorStore.mailCursor(QStringLiteral("sub-1"), QStringLiteral("Work/Legal")).isEmpty());
    QVERIFY(cursorStore.contactBaseCursor().isEmpty());
    QCOMPARE(cursorStore.notificationCursor(), qint64(0));

    // And on disk, not just in the QSettings cache: the folder names must not
    // be readable by anyone who opens the file afterwards.
    QFile file(cursorsPath);
    if (file.exists()) {
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray remaining = file.readAll();
        QVERIFY2(!remaining.contains("Legal"), "a wiped cursors.ini must not still name synced mailboxes");
        QVERIFY2(!remaining.contains("sub-1"), "a wiped cursors.ini must not still name the subscriber");
        QVERIFY2(!remaining.contains("4242"), "a wiped cursors.ini must not still hold a cursor value");
    }
}

QTEST_GUILESS_MAIN(LocalDataWipeTest)
#include "LocalDataWipeTest.moc"
