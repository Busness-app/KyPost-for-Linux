#pragma once

#include "models/MailFolder.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <optional>

// No Folder model exists in core/models (Task 3 didn't define one) — the
// `folders` table is addressed by raw fields per the task-4 brief, so this
// DAO defines its own minimal read-side record type.
struct FolderRecord
{
    QString path;
    QString parent;
    bool deletable = false;
    QString sourceMode;

    bool operator==(const FolderRecord&) const = default;
};

class FolderDao
{
public:
    explicit FolderDao(QSqlDatabase& db);

    bool insertOrReplace(const QString& path, const QString& parent, bool deletable,
                          const QString& sourceMode);
    std::optional<FolderRecord> findByPath(const QString& path) const;
    QVector<FolderRecord> findByParent(const QString& parent) const;
    QVector<FolderRecord> findAll() const;
    bool deleteByPath(const QString& path);
    bool deleteByParent(const QString& parent);
    bool deleteAll();

    // Wipes every row under `parent` and inserts `folders` in its place,
    // wrapped in one transaction so a partial failure doesn't leave the
    // parent half-replaced. Same shape as EmailDao::replaceFolderSnapshot.
    // Deletions on the server are only observable as an absence, so a
    // replace (not an upsert loop) is what actually removes a folder that
    // has gone away.
    bool replaceParentSnapshot(const QString& parent, const QVector<MailFolder>& folders,
                                const QString& sourceMode);

private:
    QSqlDatabase& m_db;
};
