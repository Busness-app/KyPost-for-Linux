#include "FolderDao.h"

#include <QSqlQuery>
#include <QVariant>

namespace {

FolderRecord folderFromQuery(const QSqlQuery& query)
{
    FolderRecord record;
    record.path = query.value(QStringLiteral("path")).toString();
    record.parent = query.value(QStringLiteral("parent")).toString();
    record.deletable = query.value(QStringLiteral("deletable")).toInt() != 0;
    record.sourceMode = query.value(QStringLiteral("source_mode")).toString();
    return record;
}

QVector<FolderRecord> collect(QSqlQuery& query)
{
    QVector<FolderRecord> results;
    while (query.next())
        results.append(folderFromQuery(query));
    return results;
}

} // namespace

FolderDao::FolderDao(QSqlDatabase& db) : m_db(db)
{
}

bool FolderDao::insertOrReplace(const QString& path, const QString& parent, bool deletable,
                                 const QString& sourceMode)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO folders (path, parent, deletable, source_mode) "
        "VALUES (:path, :parent, :deletable, :source_mode)"));
    query.bindValue(QStringLiteral(":path"), path);
    query.bindValue(QStringLiteral(":parent"), parent);
    query.bindValue(QStringLiteral(":deletable"), deletable ? 1 : 0);
    query.bindValue(QStringLiteral(":source_mode"), sourceMode);
    return query.exec();
}

bool FolderDao::deleteByParent(const QString& parent)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM folders WHERE parent = :parent"));
    query.bindValue(QStringLiteral(":parent"), parent);
    return query.exec();
}

bool FolderDao::replaceParentSnapshot(const QString& parent, const QVector<MailFolder>& folders,
                                       const QString& sourceMode)
{
    if (!m_db.transaction())
        return false;

    if (!deleteByParent(parent)) {
        m_db.rollback();
        return false;
    }

    for (const MailFolder& folder : folders) {
        if (!insertOrReplace(folder.path, parent, folder.deletable, sourceMode)) {
            m_db.rollback();
            return false;
        }
    }

    return m_db.commit();
}

std::optional<FolderRecord> FolderDao::findByPath(const QString& path) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM folders WHERE path = :path"));
    query.bindValue(QStringLiteral(":path"), path);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return folderFromQuery(query);
}

QVector<FolderRecord> FolderDao::findByParent(const QString& parent) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM folders WHERE parent = :parent"));
    query.bindValue(QStringLiteral(":parent"), parent);
    if (!query.exec())
        return {};
    return collect(query);
}

QVector<FolderRecord> FolderDao::findAll() const
{
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT * FROM folders")))
        return {};
    return collect(query);
}

bool FolderDao::deleteByPath(const QString& path)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM folders WHERE path = :path"));
    query.bindValue(QStringLiteral(":path"), path);
    return query.exec();
}

bool FolderDao::deleteAll()
{
    QSqlQuery query(m_db);
    return query.exec(QStringLiteral("DELETE FROM folders"));
}
