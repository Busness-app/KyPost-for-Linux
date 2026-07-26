#include "db/Database.h"
#include "db/EmailDao.h"
#include "models/Email.h"

#include <QTest>

class EmailDaoTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void roundTripsInsertUpdateDelete();
    void findsByFolderAndAll();
    void deleteByFolderRemovesOnlyThatFolder();
    void replaceFolderSnapshotWipesOnlyTargetFolderAndInsertsGivenEmails();

private:
    Database m_db;
};

void EmailDaoTest::init()
{
    QVERIFY(m_db.open(QStringLiteral(":memory:")));
}

void EmailDaoTest::roundTripsInsertUpdateDelete()
{
    EmailDao dao(m_db.handle());

    Email email;
    email.messageId = QStringLiteral("msg-1");
    email.folder = QStringLiteral("INBOX");
    email.sender = QStringLiteral("a@example.com");
    email.sentTo = QStringLiteral("b@example.com");
    email.cc = QStringLiteral("c@example.com");
    email.bcc = QStringLiteral("d@example.com");
    email.subject = QStringLiteral("Subject");
    email.preview = QStringLiteral("Preview");
    email.body = QStringLiteral("Body text");
    email.label = QStringLiteral("Label");
    email.keywords = QStringList{QStringLiteral("urgent"), QStringLiteral("work")};
    email.status = QStringLiteral("unread");
    email.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    email.hasAttachments = true;
    email.sourceMode = QStringLiteral("sync");
    email.pgpEncrypted = true;
    email.pgpDecryptError = QStringLiteral("no secret key");

    QVERIFY(dao.insertOrReplace(email));

    auto found = dao.findById(email.messageId);
    QVERIFY(found.has_value());
    QCOMPARE(*found, email);

    Email updated = email;
    updated.subject = QStringLiteral("Updated subject");
    updated.body = std::nullopt;
    updated.keywords = QStringList{QStringLiteral("later")};
    updated.hasAttachments = false;
    // Also exercises clearing the PGP columns, so a stale decrypt error
    // can't survive a re-sync of a message that is no longer failing.
    updated.pgpEncrypted = false;
    updated.pgpDecryptError = QString();
    QVERIFY(dao.insertOrReplace(updated));

    auto refetched = dao.findById(email.messageId);
    QVERIFY(refetched.has_value());
    QCOMPARE(*refetched, updated);
    QVERIFY(!refetched->body.has_value());

    QVERIFY(dao.deleteById(email.messageId));
    QVERIFY(!dao.findById(email.messageId).has_value());
}

void EmailDaoTest::findsByFolderAndAll()
{
    EmailDao dao(m_db.handle());

    Email inboxEmail;
    inboxEmail.messageId = QStringLiteral("msg-inbox");
    inboxEmail.folder = QStringLiteral("INBOX");
    inboxEmail.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    QVERIFY(dao.insertOrReplace(inboxEmail));

    Email sentEmail;
    sentEmail.messageId = QStringLiteral("msg-sent");
    sentEmail.folder = QStringLiteral("Sent");
    sentEmail.atUtc = QStringLiteral("2026-01-02T00:00:00Z");
    QVERIFY(dao.insertOrReplace(sentEmail));

    QCOMPARE(dao.findByFolder(QStringLiteral("INBOX")).size(), 1);
    QCOMPARE(dao.findAll().size(), 2);

    QVERIFY(dao.deleteAll());
    QCOMPARE(dao.findAll().size(), 0);
}

void EmailDaoTest::deleteByFolderRemovesOnlyThatFolder()
{
    EmailDao dao(m_db.handle());

    Email inboxEmail;
    inboxEmail.messageId = QStringLiteral("msg-inbox");
    inboxEmail.folder = QStringLiteral("INBOX");
    inboxEmail.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    QVERIFY(dao.insertOrReplace(inboxEmail));

    Email sentEmail;
    sentEmail.messageId = QStringLiteral("msg-sent");
    sentEmail.folder = QStringLiteral("Sent");
    sentEmail.atUtc = QStringLiteral("2026-01-02T00:00:00Z");
    QVERIFY(dao.insertOrReplace(sentEmail));

    QVERIFY(dao.deleteByFolder(QStringLiteral("INBOX")));

    QVERIFY(!dao.findById(QStringLiteral("msg-inbox")).has_value());
    QVERIFY(dao.findById(QStringLiteral("msg-sent")).has_value());
}

void EmailDaoTest::replaceFolderSnapshotWipesOnlyTargetFolderAndInsertsGivenEmails()
{
    EmailDao dao(m_db.handle());

    Email sentEmail;
    sentEmail.messageId = QStringLiteral("msg-sent");
    sentEmail.folder = QStringLiteral("Sent");
    sentEmail.atUtc = QStringLiteral("2026-01-02T00:00:00Z");
    QVERIFY(dao.insertOrReplace(sentEmail));

    Email staleInboxEmail;
    staleInboxEmail.messageId = QStringLiteral("msg-stale");
    staleInboxEmail.folder = QStringLiteral("INBOX");
    staleInboxEmail.atUtc = QStringLiteral("2026-01-01T00:00:00Z");
    QVERIFY(dao.insertOrReplace(staleInboxEmail));

    Email newInboxEmail1;
    newInboxEmail1.messageId = QStringLiteral("msg-new-1");
    newInboxEmail1.folder = QStringLiteral("INBOX");
    newInboxEmail1.subject = QStringLiteral("New 1");
    newInboxEmail1.atUtc = QStringLiteral("2026-01-03T00:00:00Z");

    Email newInboxEmail2;
    newInboxEmail2.messageId = QStringLiteral("msg-new-2");
    newInboxEmail2.folder = QStringLiteral("INBOX");
    newInboxEmail2.subject = QStringLiteral("New 2");
    newInboxEmail2.atUtc = QStringLiteral("2026-01-04T00:00:00Z");

    QVERIFY(dao.replaceFolderSnapshot(QStringLiteral("INBOX"), { newInboxEmail1, newInboxEmail2 }));

    // The stale row in the replaced folder is gone.
    QVERIFY(!dao.findById(QStringLiteral("msg-stale")).has_value());

    // The two new rows round-trip.
    const QVector<Email> inboxEmails = dao.findByFolder(QStringLiteral("INBOX"));
    QCOMPARE(inboxEmails.size(), 2);
    QVERIFY(dao.findById(QStringLiteral("msg-new-1")).has_value());
    QCOMPARE(*dao.findById(QStringLiteral("msg-new-1")), newInboxEmail1);
    QVERIFY(dao.findById(QStringLiteral("msg-new-2")).has_value());
    QCOMPARE(*dao.findById(QStringLiteral("msg-new-2")), newInboxEmail2);

    // A row in a different folder survives untouched.
    QVERIFY(dao.findById(QStringLiteral("msg-sent")).has_value());
    QCOMPARE(*dao.findById(QStringLiteral("msg-sent")), sentEmail);
}

QTEST_GUILESS_MAIN(EmailDaoTest)
#include "EmailDaoTest.moc"
