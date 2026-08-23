#include "db/ProfileDatabase.h"

#include "db/Database.h"
#include "db/DatabaseEncryptionMigration.h"

#include <QFile>
#include <QFileInfo>

namespace {

// Every SQLite database begins with this, NUL included. SQLCipher encrypts
// from the first byte, so an encrypted file does not.
constexpr char kSqliteMagic[] = "SQLite format 3";
constexpr int kSqliteMagicLength = 15;

} // namespace

ProfileDatabaseMode chooseProfileDatabaseMode(const ProfileDatabaseInputs& inputs)
{
    // A build with no SQLCipher cannot encrypt anything, and refusing to run
    // would make such a build useless rather than safe. It is a packaging
    // state, not a user-facing security decision -- the shipped Flatpak and
    // CI both have SQLCipher, and Database::open() refuses a key here rather
    // than pretending.
    if (!inputs.sqlCipherAvailable)
        return ProfileDatabaseMode::PlaintextOnDisk;

    switch (inputs.keyStatus) {
    case DatabaseKeyStore::Status::Found:
        // A key exists, so this profile is an encrypted one. A file that has
        // not caught up -- a conversion interrupted part-way -- is finished
        // by openProfileDatabase(); the intent either way is encryption.
        return ProfileDatabaseMode::EncryptedOnDisk;

    case DatabaseKeyStore::Status::Absent:
        // No key, whether or not a database is already here.
        //
        // An existing plaintext profile is CONVERTED rather than left as it
        // was (user's call, 2026-08-22): most people never open Settings, so
        // an opt-in would leave most mail in plaintext indefinitely. The
        // conversion itself is written so that losing the mail is never the
        // failure mode -- see DatabaseEncryptionMigration -- and if it does
        // fail, openProfileDatabase() falls back to opening the untouched
        // plaintext database rather than leaving the user with nothing.
        return ProfileDatabaseMode::EncryptedOnDisk;

    case DatabaseKeyStore::Status::Unreadable:
    case DatabaseKeyStore::Status::Corrupt:
        // Nothing may be concluded about this profile's key, so nothing may
        // be written under a new one -- see DatabaseKeyStore's header for
        // what minting a replacement costs.
        //
        // An existing file is opened as it stands: if it is encrypted we have
        // no key for it and will fail, and if it is plaintext, refusing does
        // not un-write it.
        if (inputs.databaseFileExists)
            return ProfileDatabaseMode::PlaintextOnDisk;
        // A new profile with nowhere to keep a key. In memory, so nothing
        // this session caches ever reaches the disk unencrypted. The mail is
        // on the relay and is re-fetched next launch; what is NOT acceptable
        // is quietly writing it out in the clear.
        return ProfileDatabaseMode::InMemoryNoKeyStorage;
    }

    return ProfileDatabaseMode::InMemoryNoKeyStorage;
}

bool databaseFileIsEncrypted(const QString& path)
{
    QFile file(path);
    if (!file.exists() || file.size() == 0)
        return false; // no database here at all
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray header = file.read(kSqliteMagicLength);
    return header != QByteArray(kSqliteMagic, kSqliteMagicLength);
}

ProfileDatabaseMode openProfileDatabase(Database& database, DatabaseKeyStore& keyStore, const QString& path)
{
    ProfileDatabaseInputs inputs;
    inputs.sqlCipherAvailable = Database::encryptionAvailable();
    inputs.databaseFileExists = QFileInfo::exists(path) && QFileInfo(path).size() > 0;
    inputs.databaseFileIsEncrypted = databaseFileIsEncrypted(path);

    const DatabaseKeyStore::Result key = keyStore.existing();
    inputs.keyStatus = key.status;

    switch (chooseProfileDatabaseMode(inputs)) {
    case ProfileDatabaseMode::EncryptedOnDisk: {
        QByteArray material = key.key;
        if (inputs.keyStatus == DatabaseKeyStore::Status::Absent) {
            material = keyStore.create();
            if (material.isEmpty()) {
                // The store refused the write. Falling through to a plaintext
                // file would put this profile's mail on disk in the clear
                // under a decision the user never made.
                return database.open(QStringLiteral(":memory:")) ? ProfileDatabaseMode::InMemoryNoKeyStorage
                                                                  : ProfileDatabaseMode::FailedToOpen;
            }
        }
        // Convert an existing plaintext profile before opening it. Safe to
        // call unconditionally: it is a no-op when there is nothing to
        // convert, and it repairs the leftovers of an interrupted run.
        DatabaseEncryptionMigration migration(path);
        const DatabaseEncryptionMigration::Status migrated = migration.run(material);
        if (migrated == DatabaseEncryptionMigration::Status::Stranded) {
            // An interrupted conversion left this profile's only complete
            // database under another name and it could not be moved back.
            // Opening `path` now would create an empty encrypted database on
            // top of that, and the mail would be gone for good; the next
            // launch retries the restore instead.
            qCritical("ProfileDatabase: this profile's database is mid-conversion and could not be "
                       "restored; refusing to open anything at %s",
                       qUtf8Printable(path));
            return ProfileDatabaseMode::FailedToOpen;
        }
        if (migrated == DatabaseEncryptionMigration::Status::Failed && QFileInfo::exists(path)
            && !databaseFileIsEncrypted(path)) {
            // The conversion did not happen and the plaintext database is
            // still there, untouched. Opening it is strictly better than
            // handing the user an app with no mail in it; the next launch
            // tries again. Reported honestly so the UI can say so.
            qCritical("ProfileDatabase: this profile's database could not be encrypted; opening it as it is");
            if (!database.open(path))
                return ProfileDatabaseMode::FailedToOpen;
            return ProfileDatabaseMode::PlaintextOnDisk;
        }

        if (!database.open(path, material))
            return ProfileDatabaseMode::FailedToOpen;
        return ProfileDatabaseMode::EncryptedOnDisk;
    }

    case ProfileDatabaseMode::PlaintextOnDisk:
        if (!database.open(path))
            return ProfileDatabaseMode::FailedToOpen;
        return ProfileDatabaseMode::PlaintextOnDisk;

    case ProfileDatabaseMode::InMemoryNoKeyStorage:
        if (!database.open(QStringLiteral(":memory:")))
            return ProfileDatabaseMode::FailedToOpen;
        return ProfileDatabaseMode::InMemoryNoKeyStorage;

    case ProfileDatabaseMode::FailedToOpen:
    // Never produced by chooseProfileDatabaseMode() -- it is a decision about
    // the data directory, taken before this function is reached.
    case ProfileDatabaseMode::InMemoryUnprotectedDirectory:
        break;
    }

    return ProfileDatabaseMode::FailedToOpen;
}
