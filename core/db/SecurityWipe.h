#pragma once

#include <QString>

// Erases the on-disk database and any cached content alongside it.
//
// Used by two flows that want the same guarantee for different reasons:
// turning Hostile Location Protection on (nothing may remain on disk), and
// the app lock's wipe-after-10-failed-attempts.
//
// Free functions rather than a class: there is no state to hold, and the
// caller must have already destroyed its Database before calling (the file
// cannot be removed reliably while a connection has it open).
namespace SecurityWipe {

// Deletes `dbPath` plus every SQLite sidecar it may have left behind.
//
// The sidecars matter: this repo sets no explicit `journal_mode` pragma, so
// it must not assume rollback-journal-only. `-wal` and `-shm` under WAL mode
// can each still hold committed page data, so deleting only the main file
// could leave readable mail behind -- the one outcome this function exists
// to prevent. A file that does not exist is not an error.
//
// Returns false only if a file that DOES exist could not be removed.
bool removeDatabaseFiles(const QString& dbPath);

// Recursively empties `cacheDir` (contact photos, and anything a future
// cache puts there) without removing the directory itself, so the app can
// keep writing to it. Missing directory is not an error.
bool clearCacheDirectory(const QString& cacheDir);

} // namespace SecurityWipe
