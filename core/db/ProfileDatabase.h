#pragma once

#include "security/DatabaseKeyStore.h"

#include <QString>

class Database;

// How this profile's database ended up being opened.
//
// A value, not a sentence: core/ owns the outcome and app/ owns the wording
// (AGENTS.md 6c). The UI turns these into something a person can act on.
enum class ProfileDatabaseMode
{
    EncryptedOnDisk,      // the intended state
    PlaintextOnDisk,      // a profile that predates encryption, or a build without SQLCipher
    InMemoryNoKeyStorage, // SQLCipher is here, but no key could be read or written
    FailedToOpen,         // nothing worked; the caller has no database at all
};

// What to do about a given profile, as a pure function of what was found.
//
// Separated from the doing so it can be tested exhaustively without a
// keyring, a disk, or a Database. Every branch below is a decision somebody
// could get backwards, and several of them cost the user their mail.
struct ProfileDatabaseInputs
{
    bool sqlCipherAvailable = false;
    DatabaseKeyStore::Status keyStatus = DatabaseKeyStore::Status::Unreadable;
    bool databaseFileExists = false;
    // Whether the existing file is already encrypted. Meaningless when
    // databaseFileExists is false.
    bool databaseFileIsEncrypted = false;
};

ProfileDatabaseMode chooseProfileDatabaseMode(const ProfileDatabaseInputs& inputs);

// True when `path` holds a file that is NOT a plaintext SQLite database.
//
// Sniffed from the header rather than inferred from whether a key exists:
// the two can disagree (a key written but the database not yet converted,
// say) and the file itself is the only thing that knows. A zero-length file
// counts as no database at all -- SQLite would happily initialise one there.
bool databaseFileIsEncrypted(const QString& path);

// Opens the profile database according to the decision above, creating and
// storing a key when that is what the decision calls for.
//
// Never falls back from encrypted to plaintext. If a key exists, the
// database is opened with it or not at all: silently reopening an encrypted
// profile in the clear would be the same class of failure as `PRAGMA key`
// against ordinary SQLite (see Database::open).
ProfileDatabaseMode openProfileDatabase(Database& database, DatabaseKeyStore& keyStore, const QString& path);
