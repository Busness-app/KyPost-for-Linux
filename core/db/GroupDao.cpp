#include "GroupDao.h"

#include <QSqlQuery>

namespace {

Group groupFromQuery(const QSqlQuery& query)
{
    Group group;
    group.id = query.value(QStringLiteral("id")).toString();
    group.name = query.value(QStringLiteral("name")).toString();
    group.rev = query.value(QStringLiteral("rev")).toLongLong();
    return group;
}

} // namespace

GroupDao::GroupDao(QSqlDatabase& db) : m_db(db)
{
}

bool GroupDao::insertOrReplace(const Group& group)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO groups (id, name, rev) VALUES (:id, :name, :rev)"));
    query.bindValue(QStringLiteral(":id"), group.id);
    query.bindValue(QStringLiteral(":name"), group.name);
    query.bindValue(QStringLiteral(":rev"), group.rev);
    return query.exec();
}

std::optional<Group> GroupDao::findById(const QString& id) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM groups WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return groupFromQuery(query);
}

bool GroupDao::deleteAll()
{
    QSqlQuery query(m_db);
    return query.exec(QStringLiteral("DELETE FROM groups"));
}

// Same shape as FolderDao::replaceParentSnapshot: /api/groups answers with
// the account's whole group list, so a group deleted server-side is only
// ever observable as an absence from it. An upsert loop can never see that
// absence, which is why a deleted group used to stay in the local name-cache
// forever and keep labelling contacts with a group they are no longer in.
bool GroupDao::replaceSnapshot(const QVector<Group>& groups)
{
    if (!m_db.transaction())
        return false;

    if (!deleteAll()) {
        m_db.rollback();
        return false;
    }

    for (const Group& group : groups) {
        if (!insertOrReplace(group)) {
            m_db.rollback();
            return false;
        }
    }

    return m_db.commit();
}

QVector<Group> GroupDao::findAll() const
{
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT * FROM groups")))
        return {};
    QVector<Group> results;
    while (query.next())
        results.append(groupFromQuery(query));
    return results;
}
