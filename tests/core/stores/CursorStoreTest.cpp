#include "stores/CursorStore.h"

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

    store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("1731000000123"));
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

    store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("100"));

    QCOMPARE(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")), QStringLiteral("100"));
    QVERIFY(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive")).isEmpty());
    QVERIFY(store.mailCursor(QStringLiteral("sub-2"), QStringLiteral("INBOX")).isEmpty());

    // Distinct slots, not last-write-wins.
    store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive"), QStringLiteral("200"));
    store.setMailCursor(QStringLiteral("sub-2"), QStringLiteral("INBOX"), QStringLiteral("300"));
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

    store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX/Receipts"), QStringLiteral("11"));
    store.setMailCursor(QStringLiteral("sub-1/INBOX"), QStringLiteral("Receipts"), QStringLiteral("22"));

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

    store.setContactBaseCursor(QStringLiteral("rev-42")); // bare-string cursor form
    QCOMPARE(store.contactBaseCursor(), QStringLiteral("rev-42"));
}

void CursorStoreTest::resetClearsBothCursors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("100"));
    store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("Archive"), QStringLiteral("150"));
    store.setContactBaseCursor(QStringLiteral("200"));
    QVERIFY(!store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    QVERIFY(!store.contactBaseCursor().isEmpty());

    store.reset();
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

    store.setNotificationCursor(12345);
    QCOMPARE(store.notificationCursor(), qint64(12345));
}

void CursorStoreTest::resetDoesNotTouchNotificationCursor()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CursorStore store(tempFilePath(dir, QStringLiteral("cursors.ini")));

    store.setMailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX"), QStringLiteral("100"));
    store.setContactBaseCursor(QStringLiteral("200"));
    store.setNotificationCursor(300);

    store.reset();

    QVERIFY(store.mailCursor(QStringLiteral("sub-1"), QStringLiteral("INBOX")).isEmpty());
    QVERIFY(store.contactBaseCursor().isEmpty());
    QCOMPARE(store.notificationCursor(), qint64(300));
}

QTEST_GUILESS_MAIN(CursorStoreTest)
#include "CursorStoreTest.moc"
