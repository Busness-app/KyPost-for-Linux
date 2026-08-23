#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <optional>

// The existing PushNotification model (core/models/PushNotification.h)
// shapes the push *payload* envelope, not the push_notifications dedup/
// consumed-tracking table's columns — so this DAO defines its own record
// type matching the table's fields (message_id, seq, received_at, consumed).
struct PushRecord
{
    QString messageId;
    qint64 seq = 0;
    QString receivedAt;
    bool consumed = false;

    bool operator==(const PushRecord&) const = default;
};

class PushDao
{
public:
    explicit PushDao(QSqlDatabase& db);

    bool insertOrReplace(const QString& messageId, qint64 seq, const QString& receivedAt,
                          bool consumed);

    // Every record or none, in one transaction. See the .cpp for why a pull
    // batch must not be applied row-by-row.
    [[nodiscard]] bool insertBatch(const QVector<PushRecord>& records);
    std::optional<PushRecord> findById(const QString& messageId) const;
    QVector<PushRecord> findUnconsumed() const;
    bool existsWithSeq(qint64 seq) const;
    QVector<PushRecord> findRecent(int limit) const; // ORDER BY seq DESC LIMIT limit
    bool markConsumed(const QString& messageId);
    bool deleteAll();

private:
    QSqlDatabase& m_db;
};
