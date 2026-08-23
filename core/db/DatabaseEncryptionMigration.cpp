#include "db/DatabaseEncryptionMigration.h"

#include "db/Database.h"
#include "db/ProfileDatabase.h"
#include "db/SecurityWipe.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

// Row counts per user table, which is what "the copy matches" is checked
// against. Returns nullopt when the database could not be read at all --
// distinct from "an empty database", which is a legitimate answer.
std::optional<QMap<QString, int>> tableCounts(QSqlDatabase& db)
{
    QSqlQuery tables(db);
    if (!tables.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"))) {
        return std::nullopt;
    }

    QStringList names;
    while (tables.next())
        names.append(tables.value(0).toString());
    tables.finish();

    QMap<QString, int> counts;
    for (const QString& name : names) {
        QSqlQuery count(db);
        // Table names cannot be bound. These come from sqlite_master, not
        // from user input, and are quoted defensively.
        if (!count.exec(QStringLiteral("SELECT count(*) FROM \"%1\"").arg(name)) || !count.next())
            return std::nullopt;
        counts.insert(name, count.value(0).toInt());
    }
    return counts;
}

int userVersion(QSqlDatabase& db)
{
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next())
        return -1;
    return query.value(0).toInt();
}

} // namespace

DatabaseEncryptionMigration::DatabaseEncryptionMigration(const QString& databasePath)
    : m_databasePath(databasePath)
    , m_markerPath(databasePath + QStringLiteral(".encrypting"))
    , m_workingCopyPath(databasePath + QStringLiteral(".encrypted-new"))
    , m_supersededPath(databasePath + QStringLiteral(".plaintext-old"))
{
}

bool DatabaseEncryptionMigration::interrupted() const
{
    return QFile::exists(m_markerPath);
}

bool DatabaseEncryptionMigration::arm()
{
    QFile marker(m_markerPath);
    if (!marker.open(QIODevice::WriteOnly))
        return false;
    marker.write("converting this profile's database to an encrypted one\n");
    marker.close();
    return true;
}

bool DatabaseEncryptionMigration::disarm()
{
    if (!QFile::exists(m_markerPath))
        return true;
    return QFile::remove(m_markerPath);
}

// A marker that outlives its conversion only makes interrupted() answer true
// for a profile that is in fact settled -- worth saying, not worth failing.
void DatabaseEncryptionMigration::disarmOrWarn()
{
    if (!disarm())
        qWarning("DatabaseEncryptionMigration: the conversion marker at %s could not be removed",
                  qUtf8Printable(m_markerPath));
}

void DatabaseEncryptionMigration::discardWorkingCopy()
{
    // Worthless in every state -- there is none in which a half-written copy
    // is preferred to the original -- but a leftover one would make the next
    // attempt's ATTACH land on a partial file, so exportEncrypted() refuses
    // to start when this could not remove it.
    if (!SecurityWipe::removeDatabaseFiles(m_workingCopyPath))
        qWarning("DatabaseEncryptionMigration: the partial encrypted copy at %s could not be removed",
                  qUtf8Printable(m_workingCopyPath));
}

bool DatabaseEncryptionMigration::reconcile()
{
    // The swap died between the two renames: the original was moved aside and
    // the copy never took its place. Put the original back -- it is the only
    // complete database on this disk.
    if (!QFileInfo::exists(m_databasePath) && QFileInfo::exists(m_supersededPath)) {
        if (!QFile::rename(m_supersededPath, m_databasePath)) {
            // The one outcome this class exists to prevent. Saying "recovered"
            // here would let the caller open a fresh empty database over the
            // top of a profile whose mail is sitting under another name.
            qCritical("DatabaseEncryptionMigration: this profile's only complete database could not be "
                       "moved back to %s and is still at %s; refusing to go any further",
                       qUtf8Printable(m_databasePath), qUtf8Printable(m_supersededPath));
            return false;
        }
        discardWorkingCopy();
        return true;
    }

    // The swap completed but the secure delete did not. There is a verified
    // encrypted database in place, so the plaintext one is now purely an
    // exposure and goes.
    if (QFileInfo::exists(m_supersededPath) && databaseFileIsEncrypted(m_databasePath)) {
        if (!SecurityWipe::removeDatabaseFiles(m_supersededPath)) {
            // Not fatal: the mail is present and encrypted at its usual path.
            // The plaintext copy beside it is an exposure, and the next
            // launch retries -- which is strictly better than refusing to
            // open the encrypted database that is sitting right there.
            qCritical("DatabaseEncryptionMigration: a readable copy of this mail is still on disk at %s",
                       qUtf8Printable(m_supersededPath));
        }
        discardWorkingCopy();
        return true;
    }

    discardWorkingCopy();
    return true;
}

bool DatabaseEncryptionMigration::exportEncrypted(const QByteArray& key)
{
    // reconcile() tried and failed to remove it. ATTACH would open that
    // partial file and sqlcipher_export would write into it.
    if (QFileInfo::exists(m_workingCopyPath)) {
        qWarning("DatabaseEncryptionMigration: a previous attempt's partial copy is still at %s; not "
                  "converting this launch",
                  qUtf8Printable(m_workingCopyPath));
        return false;
    }

    Database source;
    if (!source.open(m_databasePath)) {
        qWarning("DatabaseEncryptionMigration: could not open the plaintext database");
        return false;
    }

    const int sourceVersion = userVersion(source.handle());
    if (sourceVersion < 0)
        return false;

    QSqlQuery attach(source.handle());
    if (!attach.exec(QStringLiteral("ATTACH DATABASE '%1' AS encrypted KEY \"x'%2'\"")
                          .arg(m_workingCopyPath, QString::fromLatin1(key.toHex())))) {
        qWarning("DatabaseEncryptionMigration: ATTACH failed: %s",
                  qUtf8Printable(attach.lastError().text()));
        return false;
    }
    attach.finish();

    QSqlQuery exportQuery(source.handle());
    const bool exported = exportQuery.exec(QStringLiteral("SELECT sqlcipher_export('encrypted')"));
    exportQuery.finish();
    if (!exported) {
        qWarning("DatabaseEncryptionMigration: sqlcipher_export failed: %s",
                  qUtf8Printable(exportQuery.lastError().text()));
        return false;
    }

    // sqlcipher_export copies the CONTENTS, not `PRAGMA user_version`. Left
    // at 0, the copy looks to Database::open() like a brand-new database and
    // every migration is replayed over rows that already have the changes --
    // which fails, on a database that is otherwise perfectly good.
    QSqlQuery version(source.handle());
    const bool versionCopied =
        version.exec(QStringLiteral("PRAGMA encrypted.user_version = %1").arg(sourceVersion));
    version.finish();
    if (!versionCopied)
        return false;

    QSqlQuery detach(source.handle());
    const bool detached = detach.exec(QStringLiteral("DETACH DATABASE encrypted"));
    detach.finish();
    return detached;
}

bool DatabaseEncryptionMigration::verifyCopyMatchesOriginal(const QByteArray& key) const
{
    Database original;
    if (!original.open(m_databasePath))
        return false;
    const std::optional<QMap<QString, int>> before = tableCounts(original.handle());
    const int beforeVersion = userVersion(original.handle());
    if (!before.has_value() || beforeVersion < 0)
        return false;

    Database copy;
    if (!copy.open(m_workingCopyPath, key)) {
        qWarning("DatabaseEncryptionMigration: the encrypted copy does not open with the key it was written with");
        return false;
    }
    const std::optional<QMap<QString, int>> after = tableCounts(copy.handle());
    const int afterVersion = userVersion(copy.handle());
    if (!after.has_value())
        return false;

    if (beforeVersion != afterVersion) {
        qWarning("DatabaseEncryptionMigration: schema version %d became %d in the copy", beforeVersion,
                  afterVersion);
        return false;
    }
    if (*before != *after) {
        qWarning("DatabaseEncryptionMigration: the encrypted copy does not hold the same rows");
        return false;
    }
    return true;
}

bool DatabaseEncryptionMigration::swapInEncrypted(const QByteArray& key)
{
    // Renamed aside rather than deleted. Between these two renames is the
    // only moment where the profile has no database at its usual path, and
    // reconcile() puts it back if we die inside it.
    if (!QFile::rename(m_databasePath, m_supersededPath)) {
        qWarning("DatabaseEncryptionMigration: could not move the plaintext database aside");
        return false;
    }
    if (!QFile::rename(m_workingCopyPath, m_databasePath)) {
        qWarning("DatabaseEncryptionMigration: could not put the encrypted database in place");
        if (!QFile::rename(m_supersededPath, m_databasePath)) // and back it goes
            qCritical("DatabaseEncryptionMigration: and the plaintext database could not be put back");
        return false;
    }

    // Proven at its final path, under its final name, before the only other
    // copy of this mail is destroyed.
    {
        Database finalCheck;
        if (!finalCheck.open(m_databasePath, key)) {
            qWarning("DatabaseEncryptionMigration: the encrypted database does not open in place; reverting");
            if (!QFile::rename(m_databasePath, m_workingCopyPath)) {
                // The encrypted file is occupying the profile's path and will
                // not move. run() calls reconcile(), which reports that the
                // plaintext original is stranded rather than pretending.
                qCritical("DatabaseEncryptionMigration: the unusable encrypted database could not be "
                           "moved out of the way");
                return false;
            }
            if (!QFile::rename(m_supersededPath, m_databasePath))
                qCritical("DatabaseEncryptionMigration: the plaintext database could not be put back");
            return false;
        }
    }

    // Last, and only now. secure_delete/VACUUM cannot help here -- this is a
    // whole file going -- so SecurityWipe removes it and its sidecars, which
    // under WAL can each still hold committed page data.
    if (!SecurityWipe::removeDatabaseFiles(m_supersededPath)) {
        // The encrypted database is in place and working; the plaintext copy
        // beside it is an exposure the next launch's reconcile() will retry.
        qCritical("DatabaseEncryptionMigration: the plaintext database could not be deleted after "
                   "conversion; a readable copy of this mail is still on disk at %s",
                   qUtf8Printable(m_supersededPath));
        return true;
    }
    return true;
}

DatabaseEncryptionMigration::Status DatabaseEncryptionMigration::run(const QByteArray& key)
{
    // The marker deliberately stays armed on Stranded: it is the only record
    // that this profile is mid-conversion, and disarming it would make the
    // next launch treat a stranded database as a routine one.
    if (!reconcile())
        return Status::Stranded;

    if (key.size() != Database::kRawKeyBytes)
        return Status::Failed;
    if (!Database::encryptionAvailable())
        return Status::Failed;

    // Nothing to convert: no database yet, or one that is already encrypted.
    if (!QFileInfo::exists(m_databasePath) || QFileInfo(m_databasePath).size() == 0) {
        disarm();
        return Status::NotNeeded;
    }
    if (databaseFileIsEncrypted(m_databasePath)) {
        disarm();
        return Status::NotNeeded;
    }

    if (!arm()) {
        // Refused rather than attempted. Without the marker, a conversion cut
        // short leaves a half-swapped profile that the next launch has no way
        // to recognise -- and this is the one operation in the app that can
        // lose a user's whole mail store.
        qCritical("DatabaseEncryptionMigration: could not record that a conversion started; not converting");
        return Status::Failed;
    }

    if (!exportEncrypted(key) || !verifyCopyMatchesOriginal(key)) {
        discardWorkingCopy();
        disarmOrWarn();
        return Status::Failed; // the plaintext database is untouched
    }

    if (!swapInEncrypted(key)) {
        // Whatever half-finished rename it left, reconcile() is the code that
        // knows how to undo it -- and it is the only thing here that can tell
        // "put back" apart from "stranded under another name".
        if (!reconcile())
            return Status::Stranded;
        disarmOrWarn();
        return Status::Failed;
    }

    disarmOrWarn();
    return Status::Migrated;
}
