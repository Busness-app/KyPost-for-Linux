#include "db/ProfileDatabase.h"

#include "db/Database.h"

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
        // A key exists, so this profile is an encrypted one. Whether the file
        // on disk has caught up is a separate question -- an unconverted file
        // alongside an existing key means a migration was interrupted, which
        // is not something this function repairs. Reported as plaintext so
        // the caller can see the disagreement rather than have it hidden.
        if (inputs.databaseFileExists && !inputs.databaseFileIsEncrypted)
            return ProfileDatabaseMode::PlaintextOnDisk;
        return ProfileDatabaseMode::EncryptedOnDisk;

    case DatabaseKeyStore::Status::Absent:
        // No key, and a database already here: a profile from before
        // encryption existed. Opened as it is, because refusing would not
        // remove the plaintext already on this disk -- it would only take the
        // user's mail away while leaving the exposure exactly where it was.
        if (inputs.databaseFileExists)
            return ProfileDatabaseMode::PlaintextOnDisk;
        // No key and no database: a new profile, and the one case where a
        // key gets minted.
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
        break;
    }

    return ProfileDatabaseMode::FailedToOpen;
}
