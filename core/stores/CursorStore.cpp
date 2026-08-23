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

bool CursorStore::setMailCursor(const QString& subscriberId, const QString& folder, const QString& cursor)
{
    m_settings.setValue(mailCursorKey(subscriberId, folder), cursor);
    return flush();
}

QString CursorStore::contactBaseCursor() const
{
    return m_settings.value(kContactBaseCursorKey, QString()).toString();
}

bool CursorStore::setContactBaseCursor(const QString& cursor)
{
    m_settings.setValue(kContactBaseCursorKey, cursor);
    return flush();
}

qint64 CursorStore::notificationCursor() const
{
    return m_settings.value(kNotificationCursorKey, qint64(0)).toLongLong();
}

bool CursorStore::setNotificationCursor(qint64 cursor)
{
    m_settings.setValue(kNotificationCursorKey, cursor);
    return flush();
}

bool CursorStore::wipeAll()
{
    m_settings.clear();
    return flush();
}

bool CursorStore::reset()
{
    m_settings.remove(kMailCursorGroup); // the whole per-(subscriber, folder) tree
    m_settings.remove(kLegacyMailCursorKey);
    m_settings.remove(kContactBaseCursorKey);
    return flush();
}

// Every mutator goes through here rather than leaving the value in
// QSettings' in-memory copy. setValue() alone reports nothing: a read-only
// file or a full disk is visible only after sync(), through status(), and
// until then the getter happily returns a value that is not on the disk. A
// cursor that only ever existed in memory is the worst of both worlds -- the
// sync that wrote it reported success, and the next launch resumes from an
// older position against a cache that has already moved on.
bool CursorStore::flush()
{
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}
