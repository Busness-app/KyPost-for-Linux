#include "domain/LocalDataWipe.h"

#include "db/Database.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "security/AppLockStore.h"
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
    void wipeEverythingTakesThePreRenameDatabasesToo();
    void wipeEverythingKeepsTheLiveDatabaseFileUsable();
    void wipeEverythingReportsAnUnremovablePairingCredential();
    void hostileLocationWipeUnlinksTheLiveDatabaseButKeepsThePairing();
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

    LocalDataWipe wipe(database, pairingStore, appLockStore, settingsStore, dir.path(), dbPath, {});
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

    LocalDataWipe wipe(database, pairingStore, appLockStore, settingsStore, dir.path(), dbPath, legacy);
    const LocalDataWipeResult result = wipe.wipeEverything();
    QVERIFY(result.legacyDatabasesRemoved);

    for (const QString& path : legacy)
        QVERIFY2(!QFile::exists(path), qPrintable(path));
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("llamamail.db-wal"))));
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("llamamail.db-shm"))));
}

// The ten-failure path relaunches into this same database, so the file must
// survive -- emptied, not unlinked. (Hostile Location Protection is the
// opposite; see below.)
void LocalDataWipeTest::wipeEverythingKeepsTheLiveDatabaseFileUsable()
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

    LocalDataWipe wipe(database, pairingStore, appLockStore, settingsStore, dir.path(), dbPath, {});
    QVERIFY(wipe.wipeEverything().complete());

    QVERIFY(QFile::exists(dbPath));
    QSqlQuery stillWorks(database.handle());
    QVERIFY(stillWorks.exec(QStringLiteral("SELECT COUNT(*) FROM emails")));
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

    LocalDataWipe wipe(database, pairingStore, appLockStore, settingsStore, dir.path(), dbPath, {});
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

    LocalDataWipe wipe(database, pairingStore, appLockStore, settingsStore, dir.path(), dbPath, {});
    const LocalDataWipeResult result = wipe.wipeOnDiskDataOnly();
    QVERIFY(result.complete());

    // Nothing left on disk -- the next launch opens ":memory:".
    QVERIFY(!QFile::exists(dbPath));

    // But the user is not being wiped: they are still paired and still
    // locked. Taking their credential here would be a bug, not a feature.
    QVERIFY(pairingStore.isPaired());
    QVERIFY(appLockStore.lockEnabled());
}

QTEST_GUILESS_MAIN(LocalDataWipeTest)
#include "LocalDataWipeTest.moc"
