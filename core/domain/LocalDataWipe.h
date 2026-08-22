#pragma once

#include <QString>
#include <QStringList>

class AppLockStore;
class CursorStore;
class Database;
class PairingStore;
class SettingsStore;

// Erases everything on this device that an attacker could otherwise read.
//
// Extracted from two lambdas inside main(). Both were security-critical --
// one runs after ten failed PIN attempts, the other when Hostile Location
// Protection is switched on -- and neither had a single test, because
// main() is 1,100 lines of stack locals with no seam a test can reach. The
// bug class that hides there is not hypothetical: the pre-rename database
// was invisible to both handlers for months, so each reported success while
// a byte-identical plaintext copy of every cached message and contact stayed
// on disk.
//
// Deliberately reports WHAT failed rather than a bare bool. The caller
// cannot undo a partial wipe, but it can (and does) name each failure in the
// journal, and "wiped" with the pairing credential still in the keychain is
// a materially different situation from a clean one.
struct LocalDataWipeResult
{
    bool tablesWiped = true;
    bool currentDatabaseRemoved = true;
    bool legacyDatabasesRemoved = true;
    bool photoCacheCleared = true;
    bool syncCursorsCleared = true;
    bool pairingCleared = true;
    bool lockCleared = true;

    bool complete() const
    {
        return tablesWiped && currentDatabaseRemoved && legacyDatabasesRemoved && photoCacheCleared
            && syncCursorsCleared && pairingCleared && lockCleared;
    }
};

// Every path that erases local data, in one place so both callers agree on
// what "everything" means.
//
// `legacyDatabasePaths` are the pre-rename profiles (llamamail.db, in both
// the old and new data directories). They are separate FILES, so
// Database::wipeAllTables() -- which scrubs the open connection -- cannot
// reach them; they have to be named explicitly or they survive.
class LocalDataWipe
{
public:
    LocalDataWipe(Database& database, PairingStore& pairingStore, AppLockStore& appLockStore,
                  SettingsStore& settingsStore, CursorStore& cursorStore, const QString& dataDir,
                  const QString& currentDatabasePath, const QStringList& legacyDatabasePaths);

    // The wipe-after-ten-failed-PIN-attempts path: local caches, the pairing
    // credential, AND the lock itself. The lock goes too on purpose --
    // leaving a PIN behind is a hint that there was an account here.
    LocalDataWipeResult wipeEverything();

    // Hostile Location Protection being switched ON. Erases what is already
    // on disk (the mode would otherwise "protect" a machine still holding
    // every cached message) but keeps the pairing and the lock: the user is
    // not being wiped, they are changing where their mail lives.
    LocalDataWipeResult wipeOnDiskDataOnly();

private:
    LocalDataWipeResult wipeCaches(bool removeCurrentDatabaseFile);

    Database& m_database;
    PairingStore& m_pairingStore;
    AppLockStore& m_appLockStore;
    SettingsStore& m_settingsStore;
    CursorStore& m_cursorStore;
    QString m_dataDir;
    QString m_currentDatabasePath;
    QStringList m_legacyDatabasePaths;
};
