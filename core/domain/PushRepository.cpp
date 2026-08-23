#include "domain/PushRepository.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/HttpClient.h"
#include "net/NetworkError.h"
#include "net/PushNotificationClient.h"
#include "net/RelayAuth.h"
#include "stores/CursorStore.h"
#include "stores/SettingsStore.h"

#include <QDateTime>
#include <QDebug>
#include <QTimeZone>
#include <QUrl>

PushRepository::PushRepository(PushDao& pushDao, CursorStore& cursorStore, PushNotificationClient& client,
                                PairingStore& pairingStore, SettingsStore& settingsStore)
    : m_pushDao(pushDao)
    , m_cursorStore(cursorStore)
    , m_client(client)
    , m_pairingStore(pairingStore)
    , m_settingsStore(settingsStore)
{
}

QVector<PushRecord> PushRepository::history(int limit) const
{
    return m_pushDao.findRecent(limit);
}

bool PushRepository::markRead(const QString& messageId)
{
    return m_pushDao.markConsumed(messageId);
}

std::optional<PushRecord> PushRepository::recordPushArrival(const PushNotification& payload,
                                                              qint64 receivedAtEpochMs)
{
    qint64 seq = receivedAtEpochMs;
    while (m_pushDao.existsWithSeq(seq))
        ++seq;

    PushRecord record;
    record.messageId = payload.messageId;
    record.seq = seq;
    record.receivedAt =
        QDateTime::fromMSecsSinceEpoch(receivedAtEpochMs, QTimeZone::UTC).toString(Qt::ISODate);
    record.consumed = false;

    if (!m_pushDao.insertOrReplace(record.messageId, record.seq, record.receivedAt, record.consumed))
        return std::nullopt;
    return record;
}

QVector<PushNotification> PushRepository::pullOnce()
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return {};

    const QUrl endpoint(resolvePullEndpoint(pairing->serverBaseUrl));
    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    const qint64 lastCursor = m_cursorStore.notificationCursor();
    // Captured before the call, compared after it. This is the poll timer's
    // path: it runs unattended every 90 seconds and, because the pull still
    // blocks the GUI thread on a nested event loop, the executor's completion
    // handlers -- including the one that purges the previous account's data
    // and stores a replacement pairing -- are delivered DURING it. Without
    // the comparison below, the previous account's notifications are written
    // back into the emptied table and the notification cursor is advanced to
    // its position, so the new account then silently skips every push up to
    // that sequence number.
    const PairingIdentity requestedBy = identityOf(*pairing);
    const PullResult result = m_client.pull(endpoint, auth, lastCursor);

    if (result.error.has_value())
        return {}; // silent -- matches this method's documented no-outcome-type contract

    if (!m_pairingStore.stillCurrent(requestedBy))
        return {}; // not ours any more: nothing persisted, cursor left alone

    const QString receivedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QVector<PushRecord> records;
    QVector<PushNotification> delivered;
    for (const PullNotificationItem& item : result.notifications) {
        if (item.seq <= lastCursor)
            continue;
        records.append(PushRecord{ item.notification.messageId, item.seq, receivedAt, false });
        delivered.append(item.notification);
    }

    // Nothing is delivered until the whole batch is stored AND the cursor
    // that says so is on the disk. Both halves are load-bearing, in opposite
    // directions:
    //
    //   - persist, then advance: a notification shown from a row that never
    //     landed is gone from history the moment it is dismissed, and the
    //     relay will not mention it again once the cursor has moved past it.
    //   - advance, then deliver: a cursor still sitting in QSettings' memory
    //     is a cursor the next launch does not have, so every notification in
    //     this batch arrives a second time.
    //
    // Both failures cost the same thing -- this poll returns nothing and the
    // next one, 90 seconds later, asks for the same window again. That is the
    // only outcome here that is safe to be wrong about.
    if (!m_pushDao.insertBatch(records)) {
        qWarning("PushRepository: could not store a pull batch; the cursor stays put");
        return {};
    }

    if (!m_cursorStore.setNotificationCursor(result.cursor)) {
        qWarning("PushRepository: could not persist the notification cursor; not delivering this batch");
        return {};
    }

    return delivered;
}

QString PushRepository::resolvePullEndpoint(const QString& serverBaseUrl) const
{
    const QString stored = m_settingsStore.pullEndpoint();
    if (!stored.isEmpty())
        return stored;

    return joinUrlPath(QUrl(serverBaseUrl), QStringLiteral("api/notifications/native/pull")).toString();
}
