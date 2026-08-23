#pragma once

#include "models/Email.h"

#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

class EmailDao
{
public:
    explicit EmailDao(QSqlDatabase& db);

    bool insertOrReplace(const Email& email);

    // Folder-scoped because (folder, message_id) is the PRIMARY KEY: the same
    // messageId legitimately exists in more than one mailbox (migration 006).
    // A caller that knows which folder it is looking at must say so, or it
    // gets whichever copy SQLite happened to return.
    std::optional<Email> findById(const QString& folder, const QString& messageId) const;

    // For the callers that genuinely have only a messageId -- opening mail
    // from a desktop notification, where the payload carries no folder.
    // Returns nullopt when the id is cached in MORE than one folder, not an
    // arbitrary one of them: silently opening the Archive copy of a message
    // the notification was announcing in INBOX is a wrong-message bug, and
    // there is no information here with which to break the tie. The caller
    // surfaces "refresh and try again" instead.
    std::optional<Email> findUniqueById(const QString& messageId) const;

    // Every folder holding this messageId, sorted, so a caller can tell the
    // three cases apart: none (not cached), one (open it), several (ask).
    //
    // findUniqueById() answers only the middle case and collapses the other
    // two into nullopt, which is right for callers that just want the row and
    // wrong for the one that has to explain itself to a user. The id stopped
    // being globally unique in migration 006 -- the PRIMARY KEY is
    // (folder, message_id), because the relay serves the same id from every
    // mailbox that holds it.
    QStringList foldersContaining(const QString& messageId) const;

    QVector<Email> findByFolder(const QString& folder) const;
    QVector<Email> findAll() const;
    bool deleteById(const QString& folder, const QString& messageId);
    bool deleteAll();

    bool deleteByFolder(const QString& folder);

    // Wipes `folder` and inserts every email in `emails` (each with .folder
    // already set to `folder` by the caller), wrapped in one transaction so
    // a partial failure doesn't leave the folder half-replaced.
    bool replaceFolderSnapshot(const QString& folder, const QVector<Email>& emails);

private:
    QSqlDatabase& m_db;
};
