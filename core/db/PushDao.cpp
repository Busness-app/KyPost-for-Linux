#include "PushDao.h"

#include <QSqlQuery>
#include <QVariant>

namespace {

PushRecord pushFromQuery(const QSqlQuery& query)
{
    PushRecord record;
    record.messageId = query.value(QStringLiteral("message_id")).toString();
    record.seq = query.value(QStringLiteral("seq")).toLongLong();
    record.receivedAt = query.value(QStringLiteral("received_at")).toString();
    record.consumed = query.value(QStringLiteral("consumed")).toInt() != 0;
    return record;
}

} // namespace

PushDao::PushDao(QSqlDatabase& db) : m_db(db)
{
}

bool PushDao::insertOrReplace(const QString& messageId, qint64 seq, const QString& receivedAt,
                               bool consumed)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO push_notifications (message_id, seq, received_at, consumed) "
        "VALUES (:message_id, :seq, :received_at, :consumed)"));
    query.bindValue(QStringLiteral(":message_id"), messageId);
    query.bindValue(QStringLiteral(":seq"), seq);
    query.bindValue(QStringLiteral(":received_at"), receivedAt);
    query.bindValue(QStringLiteral(":consumed"), consumed ? 1 : 0);
    return query.exec();
}

// One transaction for the whole pull batch. The alternative -- a loop of
// bare insertOrReplace() calls -- can stop halfway and leave the caller
// holding a cursor decision it has no basis for: some of these rows are
// stored, some are not, and nothing on the wire will mention the missing
// ones again.
bool PushDao::insertBatch(const QVector<PushRecord>& records)
{
    if (records.isEmpty())
        return true;

    if (!m_db.transaction())
        return false;

    for (const PushRecord& record : records) {
        if (!insertOrReplace(record.messageId, record.seq, record.receivedAt, record.consumed)) {
            m_db.rollback();
            return false;
        }
    }

    return m_db.commit();
}

std::optional<PushRecord> PushDao::findById(const QString& messageId) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM push_notifications WHERE message_id = :message_id"));
    query.bindValue(QStringLiteral(":message_id"), messageId);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return pushFromQuery(query);
}

QVector<PushRecord> PushDao::findUnconsumed() const
{
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT * FROM push_notifications WHERE consumed = 0")))
        return {};
    QVector<PushRecord> results;
    while (query.next())
        results.append(pushFromQuery(query));
    return results;
}

bool PushDao::existsWithSeq(qint64 seq) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT 1 FROM push_notifications WHERE seq = :seq LIMIT 1"));
    query.bindValue(QStringLiteral(":seq"), seq);
    return query.exec() && query.next();
}

QVector<PushRecord> PushDao::findRecent(int limit) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM push_notifications ORDER BY seq DESC LIMIT :limit"));
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec())
        return {};
    QVector<PushRecord> results;
    while (query.next())
        results.append(pushFromQuery(query));
    return results;
}

bool PushDao::markConsumed(const QString& messageId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE push_notifications SET consumed = 1 WHERE message_id = :message_id"));
    query.bindValue(QStringLiteral(":message_id"), messageId);
    return query.exec();
}

bool PushDao::deleteAll()
{
    QSqlQuery query(m_db);
    return query.exec(QStringLiteral("DELETE FROM push_notifications"));
}
