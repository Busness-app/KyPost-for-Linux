#include "stores/CursorStore.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

class CursorStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAreEmpty();
    void mailCursorRoundTrips();
    void mailCursorIsScopedPerSubscriberAndFolder();
    void mailCursorKeysCannotCollideThroughSeparatorsInFolderNames();
    void contactBaseCursorRoundTrips();
    void resetClearsBothCursors();
    void notificationCursorDefaultsToZeroAndRoundTrips();
    void resetDoesNotTouchNotificationCursor();
    void aCursorThatCannotReachTheDiskIsReportedAsFailed();

private:
    QString tempFilePath(QTemporaryDir& dir, const QString& name) const;
};

QString CursorStoreTest::tempFilePath(QTemporaryDir& dir, const QString& name) const
{
    return dir.filePath(name);
}

void CursorStoreTest::defaultsAreEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    QVERIFY(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    QVERIFY(store.contactBaseCursor().isEmpty());
}

void CursorStoreTest::mailCursorRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    QVERIFY(store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("1731000000123")));
    QCOMPARE(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")),
             QStringLiteral("1731000000123"));
}

// The relay diffs a window per mailbox, so one folder's cursor is not a valid
// `since` for another; and nothing clears cursors.ini on unpair, so a
// re-pairing to a different account must not inherit the previous account's
// cursor. Both are answered by the key, not by a caller remembering to check.
void CursorStoreTest::mailCursorIsScopedPerSubscriberAndFolder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    QVERIFY(store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("100")));

    QCOMPARE(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")), QStringLiteral("100"));
    QVERIFY(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive")).isEmpty());
    QVERIFY(store.mailCursor(QStringLiteral("sub-2"), QStringLiteral("INBOX")).isEmpty());

    // Distinct slots, not last-write-wins.
    QVERIFY(store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive"), QStringLiteral("200")));
    QVERIFY(store.setMailCursor(QStringLiteral("sub-2"), QStringLiteral("INBOX"), QStringLiteral("300")));
    QCOMPARE(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")), QStringLiteral("100"));
    QCOMPARE(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive")), QStringLiteral("200"));
    QCOMPARE(store.mailCursor(QStringLiteral("sub-2"), QStringLiteral("INBOX")), QStringLiteral("300"));
}

// QSettings reads '/' as a group separator and IMAP folder paths contain it by
// design, so an unencoded key would let "INBOX/Receipts" and a subscriber
// named "INBOX" with folder "Receipts" land in the same slot -- one account
// reading another's cursor. Percent-encoding each half is what prevents it.
void CursorStoreTest::mailCursorKeysCannotCollideThroughSeparatorsInFolderNames()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    QVERIFY(store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX/Receipts"), QStringLiteral("11")));
    QVERIFY(store.setMailCursor(QStringLiteral("sub-1/INBOX"), QStringLiteral("Receipts"), QStringLiteral("22")));

    QCOMPARE(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX/Receipts")), QStringLiteral("11"));
    QCOMPARE(store.mailCursor(QStringLiteral("sub-1/INBOX"), QStringLiteral("Receipts")), QStringLiteral("22"));

    // Round-trips through the file, not just the in-memory QSettings cache.
    CursorStore reopened(tempFilePath(dir, QStringLiteral("cursors.ini")));
    QCOMPARE(reopened.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX/Receipts")), QStringLiteral("11"));
    QCOMPARE(reopened.mailCursor(QStringLiteral("sub-1/INBOX"), QStringLiteral("Receipts")), QStringLiteral("22"));
}

void CursorStoreTest::contactBaseCursorRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    QVERIFY(store.setContactBaseCursor(QStringLiteral("rev-42"))); // bare-string cursor form
    QCOMPARE(store.contactBaseCursor(), QStringLiteral("rev-42"));
}

void CursorStoreTest::resetClearsBothCursors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    QVERIFY(store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("100")));
    QVERIFY(store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive"), QStringLiteral("150")));
    QVERIFY(store.setContactBaseCursor(QStringLiteral("200")));
    QVERIFY(!store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    QVERIFY(!store.contactBaseCursor().isEmpty());

    QVERIFY(store.reset());
    // EVERY folder's cursor, not just the one the caller happened to name.
    QVERIFY(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    QVERIFY(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive")).isEmpty());
    QVERIFY(store.contactBaseCursor().isEmpty());
}

void CursorStoreTest::notificationCursorDefaultsToZeroAndRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    QCOMPARE(store.notificationCursor(), qint64(0));

    QVERIFY(store.setNotificationCursor(12345));
    QCOMPARE(store.notificationCursor(), qint64(12345));
}

void CursorStoreTest::resetDoesNotTouchNotificationCursor()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    QVERIFY(store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("100")));
    QVERIFY(store.setContactBaseCursor(QStringLiteral("200")));
    QVERIFY(store.setNotificationCursor(300));

    QVERIFY(store.reset());

    QVERIFY(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    QVERIFY(store.contactBaseCursor().isEmpty());
    QCOMPARE(store.notificationCursor(), qint64(300));
}

// The whole point of the bool these setters return.
//
// QSettings takes a setValue() into memory and reports a failed FILE write
// nowhere except status(), after a sync(). Before that was checked, a cursor
// write onto a read-only file or a full disk looked identical to a real one:
// the getter answered with the new value for the rest of the session, the
// repository reported the sync as a success, and the next launch came up
// holding the OLD cursor against a cache that had already moved on -- asking
// the relay for a diff against a window it no longer had.
//
// The failure is injected by putting a DIRECTORY where the ini file goes:
// nothing can write a file there, including root, which a read-only file
// cannot promise (QSettings writes via a temporary file and a rename, so a
// writable parent is enough to defeat chmod on the file itself).
void CursorStoreTest::aCursorThatCannotReachTheDiskIsReportedAsFailed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString blocked = tempFilePath(dir, QStringLiteral("cursors.ini"));
    QVERIFY(QDir().mkpath(blocked));

    CursorStore store(blocked);

    QVERIFY(!store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("100")));
    QVERIFY(!store.setContactBaseCursor(QStringLiteral("200")));
    QVERIFY(!store.setNotificationCursor(300));
    QVERIFY(!store.reset());
    QVERIFY(!store.wipeAll());

    // And nothing reached the disk, which is what the false was about: a
    // second store over the same path -- what the next launch does -- sees
    // none of it.
    CursorStore nextLaunch(blocked);
    QVERIFY(nextLaunch.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    QVERIFY(nextLaunch.contactBaseCursor().isEmpty());
    QCOMPARE(nextLaunch.notificationCursor(), qint64(0));
}

QTEST_GUILESS_MAIN(CursorStoreTest)
#include "CursorStoreTest.moc"
