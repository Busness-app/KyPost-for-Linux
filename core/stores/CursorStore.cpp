#include "stores/CursorStore.h"

#include <QUrl>

namespace {
// The pre-scoping single mail cursor. Never read any more -- a global cursor
// has no correct interpretation once cursors are per (subscriber, folder) --
// but reset() still names it so an upgraded profile does not keep the stale
// value forever in a file nothing else touches.
constexpr auto kLegacyMailCursorKey = "sync/mailCursor";
constexpr auto kMailCursorGroup = "sync/mail";
constexpr auto kContactBaseCursorKey = "sync/contactBaseCursor";
constexpr auto kNotificationCursorKey = "sync/notificationCursor";

// QSettings reads '/' as a group separator, and both halves of the key are
// attacker-adjacent free text: an IMAP folder path contains '/' by design
// ("INBOX/Receipts"), and a subscriber id is whatever the relay minted.
// Percent-encoding leaves only [A-Za-z0-9-._~%], so neither half can inject a
// group boundary and collide with another folder's slot.
QString mailCursorKey(const QString& subscriberId, const QString& folder)
{
    return QLatin1String(kMailCursorGroup) + QLatin1Char('/')
        + QString::fromLatin1(QUrl::toPercentEncoding(subscriberId)) + QLatin1Char('/')
        + QString::fromLatin1(QUrl::toPercentEncoding(folder));
}
} // namespace

CursorStore::CursorStore(const QString& filePath)
    : m_settings(filePath, QSettings::IniFormat)
{
}

QString CursorStore::mailCursor(const QString& subscriberId, const QString& folder) const
{
    return m_settings.value(mailCursorKey(subscriberId, folder), QString()).toString();
}

void CursorStore::setMailCursor(const QString& subscriberId, const QString& folder, const QString& cursor)
{
    m_settings.setValue(mailCursorKey(subscriberId, folder), cursor);
}

QString CursorStore::contactBaseCursor() const
{
    return m_settings.value(kContactBaseCursorKey, QString()).toString();
}

void CursorStore::setContactBaseCursor(const QString& cursor)
{
    m_settings.setValue(kContactBaseCursorKey, cursor);
}

qint64 CursorStore::notificationCursor() const
{
    return m_settings.value(kNotificationCursorKey, qint64(0)).toLongLong();
}

void CursorStore::setNotificationCursor(qint64 cursor)
{
    m_settings.setValue(kNotificationCursorKey, cursor);
}

bool CursorStore::wipeAll()
{
    m_settings.clear();
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}

void CursorStore::reset()
{
    m_settings.remove(kMailCursorGroup); // the whole per-(subscriber, folder) tree
    m_settings.remove(kLegacyMailCursorKey);
    m_settings.remove(kContactBaseCursorKey);
    m_settings.sync();
}
