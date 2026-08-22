#include "domain/LocalDataWipe.h"

#include "db/Database.h"
#include "db/SecurityWipe.h"
#include "domain/PairingStore.h"
#include "security/AppLockStore.h"
#include "stores/CursorStore.h"
#include "stores/SettingsStore.h"

#include <QFile>

LocalDataWipe::LocalDataWipe(Database& database, PairingStore& pairingStore, AppLockStore& appLockStore,
                              SettingsStore& settingsStore, CursorStore& cursorStore,
                              const QString& dataDir, const QString& currentDatabasePath,
                              const QStringList& legacyDatabasePaths)
    : m_database(database)
    , m_pairingStore(pairingStore)
    , m_appLockStore(appLockStore)
    , m_settingsStore(settingsStore)
    , m_cursorStore(cursorStore)
    , m_dataDir(dataDir)
    , m_currentDatabasePath(currentDatabasePath)
    , m_legacyDatabasePaths(legacyDatabasePaths)
{
}

LocalDataWipeResult LocalDataWipe::wipeCaches(bool removeCurrentDatabaseFile)
{
    LocalDataWipeResult result;

    // Tables FIRST, always, even when the file is about to be unlinked. The
    // connection is still open, so the unlink cannot be relied on -- and
    // emptying the tables means the content is gone even if the removal
    // loses a race with a straggling writer.
    result.tablesWiped = m_database.wipeAllTables();

    // Only the Hostile Location Protection path unlinks the live database:
    // it is about to relaunch into an in-memory one, so nothing should
    // remain. The wipe-after-ten-failures path relaunches into this same
    // file and needs it to still be there, emptied.
    if (removeCurrentDatabaseFile && !m_currentDatabasePath.isEmpty())
        result.currentDatabaseRemoved = SecurityWipe::removeDatabaseFiles(m_currentDatabasePath);

    // The pre-rename profiles are separate FILES, which wipeAllTables()
    // cannot reach -- it scrubs one connection. They were missing from both
    // callers for months, so each reported a completed wipe while a
    // byte-identical plaintext copy of the same mail and contacts sat on
    // disk. Named explicitly, and each one's sidecars with it.
    for (const QString& legacyDbPath : m_legacyDatabasePaths) {
        if (QFile::exists(legacyDbPath))
            result.legacyDatabasesRemoved = SecurityWipe::removeDatabaseFiles(legacyDbPath)
                && result.legacyDatabasesRemoved;
    }

    result.photoCacheCleared =
        SecurityWipe::clearCacheDirectory(m_dataDir + QStringLiteral("/contact-photos"));

    // cursors.ini survived every wipe path until now: CursorStore::reset()
    // existed but had no caller anywhere in the app. It is not mail content,
    // but it is a separate file naming the subscriber id and every mailbox
    // this device synced -- and since mail cursors became per (subscriber,
    // folder) it names strictly more of them than it used to. A wipe that
    // leaves behind "this machine synced INBOX, Work/Legal and Archive for
    // subscriber X" has not wiped.
    //
    // It must also go for a correctness reason, not only a privacy one: the
    // wipe empties the mail tables, so a surviving cursor would have the next
    // sync ask for a delta against a cache that no longer exists, and the
    // messages before that cursor would never be re-fetched.
    result.syncCursorsCleared = m_cursorStore.wipeAll();

    return result;
}

LocalDataWipeResult LocalDataWipe::wipeCachedAccountData()
{
    // The database FILE stays: the app keeps running on this connection and
    // is about to sync the new account into it. wipeAllTables() empties the
    // rows, which is what "the previous account's data is gone" means here.
    return wipeCaches(/*removeCurrentDatabaseFile=*/false);
}

LocalDataWipeResult LocalDataWipe::wipeEverything()
{
    LocalDataWipeResult result = wipeCaches(/*removeCurrentDatabaseFile=*/false);

    // Every result checked and aggregated, because all of these genuinely
    // fail: SecureStore writes fail on any machine with no reachable Secret
    // Service. A silently-failed removal here means the device secret
    // survives a wipe the app has already told the user it performed.
    result.pairingCleared = m_pairingStore.clear();
    result.lockCleared = m_appLockStore.clear();

    // Not part of the result: this is a preference, not a secret, and a
    // failure to reset it leaks nothing.
    m_settingsStore.setDeliveryMode(QString());

    return result;
}

LocalDataWipeResult LocalDataWipe::wipeOnDiskDataOnly()
{
    return wipeCaches(/*removeCurrentDatabaseFile=*/true);
}
