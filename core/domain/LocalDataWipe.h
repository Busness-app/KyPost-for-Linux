#pragma once

#include "db/ProfileDatabase.h"

#include <QString>
#include <QStringList>

#include <optional>

class AppLockStore;
class CursorStore;
class Database;
class DatabaseKeyStore;
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
    bool databaseKeyCleared = true;
    bool legacyDatabasesRemoved = true;
    bool photoCacheCleared = true;
    bool syncCursorsCleared = true;
    bool pairingCleared = true;
    bool lockCleared = true;

    // How the replacement profile was opened after the wipe unlinked the
    // live database, or nullopt when this wipe had no database to reopen
    // (see LocalDataWipe::wipeEverything). FailedToOpen means the caller has
    // no database at all and must not go on using the old connection.
    //
    // Deliberately NOT part of complete(): that answers "is everything
    // erased", and a reopen that failed is a broken session rather than
    // surviving data. The caller acts on it separately.
    std::optional<ProfileDatabaseMode> databaseReopenedAs;

    bool complete() const
    {
        return tablesWiped && currentDatabaseRemoved && databaseKeyCleared && legacyDatabasesRemoved
            && photoCacheCleared && syncCursorsCleared && pairingCleared && lockCleared;
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
    LocalDataWipe(Database& database, DatabaseKeyStore& databaseKeyStore, PairingStore& pairingStore,
                  AppLockStore& appLockStore, SettingsStore& settingsStore, CursorStore& cursorStore,
                  const QString& dataDir, const QString& currentDatabasePath,
                  const QStringList& legacyDatabasePaths);

    // The wipe-after-ten-failed-PIN-attempts path: local caches, the pairing
    // credential, AND the lock itself. The lock goes too on purpose --
    // leaving a PIN behind is a hint that there was an account here.
    //
    // The database file goes with them. It used to be kept, emptied, for the
    // relaunch to reopen -- but the SQLCipher key is erased here now, and an
    // encrypted file whose key no longer exists is one openProfileDatabase()
    // can only treat as fatal. Which is exactly why the key is erased only
    // once the file is confirmed gone: see wipeCaches().
    //
    // A REPLACEMENT profile is then opened on the same path, reported in
    // `databaseReopenedAs`, and this is not a nicety. Two of the three
    // callers relaunch immediately, but TrackedWipe::recoverIfInterrupted()
    // -- which finishes an interrupted wipe at startup -- returns into a
    // full normal session. Without the reopen that session is left holding a
    // connection whose file has no name: writes through it either fail
    // outright or land in an inode no later launch can reach. The user
    // re-pairs (the wipe cleared the pairing, so they are shown that screen)
    // and syncs, while cursors.ini -- erased by this wipe and repopulated by
    // that same sync -- tells the next launch it is already up to date, so
    // the mail is permanently absent locally with nothing left to force a
    // resync.
    //
    // Skipped when this session was never on that file (":memory:", under
    // Hostile Location Protection or an unprotected data directory):
    // creating it there is the one thing those modes exist to prevent.
    LocalDataWipeResult wipeEverything();

    // Hostile Location Protection being switched ON. Erases what is already
    // on disk (the mode would otherwise "protect" a machine still holding
    // every cached message) but keeps the pairing and the lock: the user is
    // not being wiped, they are changing where their mail lives. The
    // database key is not kept -- leaving it behind would mean any recovered
    // copy of the file it just unlinked is still readable. Unless the unlink
    // failed: see wipeCaches().
    //
    // Never reopens, unlike wipeEverything(): the caller is on its way to
    // relaunching into ":memory:", and re-creating the file on disk is
    // exactly what this mode was asked to prevent.
    LocalDataWipeResult wipeOnDiskDataOnly();

    // Account replacement: erase the PREVIOUS account's cached data while
    // keeping the pairing (which by this point describes the NEW account) and
    // the lock (which belongs to the person holding the device, not to the
    // account).
    //
    // Needed because no table in this schema carries a subscriber column.
    // Cached mail, contacts, groups, photos and sync cursors are stored
    // per-device, not per-account, so anything that survives pairing a
    // different account is readable by whoever paired it. That is not a
    // theoretical mixing of state: it is one person's mail shown to another.
    //
    // Deliberately NOT wipeEverything(): this runs at the moment a new
    // pairing has just been proven, and destroying the credential that was
    // established a moment ago would strand the device.
    LocalDataWipeResult wipeCachedAccountData();

private:
    // `removeCurrentDatabase` takes the database file AND the key that
    // decrypts it. They are one unit: a file with no key cannot be opened
    // again, and a key with no file names an account this device was told to
    // forget.
    //
    // The key goes only once the file is confirmed gone. An unlink that
    // failed -- a non-writable profile directory, an EPERM -- is reported
    // through currentDatabaseRemoved, and keeping the key for the file that
    // survived is what leaves that state recoverable rather than fatal.
    LocalDataWipeResult wipeCaches(bool removeCurrentDatabase);

    // True when the live connection is open on m_currentDatabasePath, i.e.
    // when unlinking that file would pull the ground out from under this
    // session. Must be asked BEFORE the file goes: afterwards the connection
    // still answers queries against a nameless inode, and nothing
    // distinguishes that from a session that was always in memory.
    bool databaseIsLiveOnDisk() const;

    Database& m_database;
    DatabaseKeyStore& m_databaseKeyStore;
    PairingStore& m_pairingStore;
    AppLockStore& m_appLockStore;
    SettingsStore& m_settingsStore;
    CursorStore& m_cursorStore;
    QString m_dataDir;
    QString m_currentDatabasePath;
    QStringList m_legacyDatabasePaths;
};
