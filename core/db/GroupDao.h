#pragma once

#include "models/Group.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <optional>

// DAO over the `groups` table (see core/db/migrations/003_extended_contact_
// fields.sql), mirroring ContactDao's shape but for the much simpler
// {id, name, rev} Group struct -- no JSON-blob columns, no optional fields.
// Full-replace cache: GroupsRepository::refresh() hands the whole response
// to replaceSnapshot() once per contact sync cycle; there is no delta/cursor
// tracking here (see task-2-brief.md).
class GroupDao
{
public:
    explicit GroupDao(QSqlDatabase& db);

    bool insertOrReplace(const Group& group);

    // Wipes every row and inserts `groups` in its place, in one transaction.
    // The only correct way to apply a /api/groups response: see the .cpp.
    [[nodiscard]] bool replaceSnapshot(const QVector<Group>& groups);

    std::optional<Group> findById(const QString& id) const;
    QVector<Group> findAll() const;
    bool deleteAll();

private:
    QSqlDatabase& m_db;
};
