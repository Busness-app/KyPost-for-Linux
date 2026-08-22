#pragma once

#include <QSettings>
#include <QString>

// Persists mail/contact sync cursors via QSettings rather than a SQLite
// table (chosen over a new schema table because a cursor is a single
// scalar-per-stream value with no query/join needs; see task 5 report).
// Cursors are stored as QString since the backend's `since`/`cursor` and
// `baseCursor` values may arrive as bare number or string on the wire.
class CursorStore
{
public:
    explicit CursorStore(const QString& filePath);

    // Scoped to (subscriberId, folder), matching kypost-android's
    // MailCheckpoint. One global mail cursor was wrong twice over: the relay
    // diffs a window per mailbox, so INBOX's cursor applied to Archive asks
    // for a diff against a window Archive never had; and nothing wipes
    // cursors.ini on unpair, so re-pairing a different account inherited the
    // previous account's cursor and got a delta against a cache it did not
    // own. Keying by subscriber makes the second case miss the lookup and
    // fall back to since=0, which is the correct answer by construction.
    QString mailCursor(const QString& subscriberId, const QString& folder) const;
    void setMailCursor(const QString& subscriberId, const QString& folder, const QString& cursor);

    QString contactBaseCursor() const;
    void setContactBaseCursor(const QString& cursor);

    // Independent of mailCursor/contactBaseCursor -- not touched by reset(),
    // which stays scoped to mail+contact (see PushRepository, task 23).
    // Stored as qint64 (not QString like the other two cursors) since
    // PushNotificationClient::pull's cursor is always a numeric seq, never a
    // bare-string form.
    qint64 notificationCursor() const; // 0 if never set
    void setNotificationCursor(qint64 cursor);

    // Clears every mail cursor and the contact cursor back to empty/absent,
    // leaving the notification cursor alone. Storage primitive for
    // ContactSyncRepository's tooOld-response reconciliation, which is a
    // contacts-only event.
    void reset();

    // Everything, notification cursor included, for the wipe paths. Reports
    // whether the file actually took the change: QSettings swallows a failed
    // write (read-only file, full disk) and reports it only through status(),
    // and a wipe that silently left cursors.ini intact is a wipe that lied.
    bool wipeAll();

private:
    QSettings m_settings;
};
