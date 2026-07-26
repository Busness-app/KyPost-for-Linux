#include "mail/EmailListModel.h"

#include "models/Email.h"

#include <QTest>

class EmailListModelTest : public QObject
{
    Q_OBJECT

private slots:
    void rowCountAndEmailAtReflectSetEmails();
    void dataRoundTripsEveryRoleForAPopulatedRow();
    void pgpMarkerRolesReflectMessageState();
    void emailAtOutOfRangeReturnsDefaultConstructedEmail();
    void dataOutOfRangeReturnsInvalidVariant();

private:
    static Email sampleEmail();
};

Email EmailListModelTest::sampleEmail()
{
    Email email;
    email.messageId = QStringLiteral("m1");
    email.folder = QStringLiteral("INBOX");
    email.sender = QStringLiteral("alice@example.com");
    email.sentTo = QStringLiteral("bob@example.com");
    email.cc = QStringLiteral("cc@example.com");
    email.bcc = QStringLiteral("bcc@example.com");
    email.subject = QStringLiteral("Hello");
    email.preview = QStringLiteral("Preview text");
    email.body = QStringLiteral("Body text");
    email.label = QStringLiteral("important");
    email.keywords = { QStringLiteral("Work"), QStringLiteral("Urgent") };
    email.status = QStringLiteral("unread");
    email.atUtc = QStringLiteral("2026-07-01T12:00:00Z");
    email.hasAttachments = true;
    email.sourceMode = QStringLiteral("plain");
    return email;
}

void EmailListModelTest::rowCountAndEmailAtReflectSetEmails()
{
    EmailListModel model;
    QCOMPARE(model.rowCount(), 0);

    const QVector<Email> emails = { sampleEmail(), sampleEmail() };
    model.setEmails(emails);

    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.emailAt(0).messageId, QStringLiteral("m1"));
    QCOMPARE(model.emailAt(1).messageId, QStringLiteral("m1"));
}

void EmailListModelTest::dataRoundTripsEveryRoleForAPopulatedRow()
{
    EmailListModel model;
    model.setEmails({ sampleEmail() });

    const QModelIndex index = model.index(0, 0);
    QVERIFY(index.isValid());

    QCOMPARE(model.data(index, EmailListModel::MessageIdRole).toString(), QStringLiteral("m1"));
    QCOMPARE(model.data(index, EmailListModel::FolderRole).toString(), QStringLiteral("INBOX"));
    QCOMPARE(model.data(index, EmailListModel::SenderRole).toString(), QStringLiteral("alice@example.com"));
    QCOMPARE(model.data(index, EmailListModel::SentToRole).toString(), QStringLiteral("bob@example.com"));
    QCOMPARE(model.data(index, EmailListModel::CcRole).toString(), QStringLiteral("cc@example.com"));
    QCOMPARE(model.data(index, EmailListModel::BccRole).toString(), QStringLiteral("bcc@example.com"));
    QCOMPARE(model.data(index, EmailListModel::SubjectRole).toString(), QStringLiteral("Hello"));
    QCOMPARE(model.data(index, EmailListModel::PreviewRole).toString(), QStringLiteral("Preview text"));
    QCOMPARE(model.data(index, EmailListModel::BodyRole).toString(), QStringLiteral("Body text"));
    QCOMPARE(model.data(index, EmailListModel::LabelRole).toString(), QStringLiteral("important"));
    QCOMPARE(model.data(index, EmailListModel::KeywordsRole).toStringList(),
             QStringList({ QStringLiteral("Work"), QStringLiteral("Urgent") }));
    QCOMPARE(model.data(index, EmailListModel::StatusRole).toString(), QStringLiteral("unread"));
    QCOMPARE(model.data(index, EmailListModel::AtUtcRole).toString(), QStringLiteral("2026-07-01T12:00:00Z"));
    QCOMPARE(model.data(index, EmailListModel::HasAttachmentsRole).toBool(), true);
    QCOMPARE(model.data(index, EmailListModel::SourceModeRole).toString(), QStringLiteral("plain"));
    // sampleEmail() carries no PGP content, so both marker roles are empty.
    QVERIFY(model.data(index, EmailListModel::PgpMarkerRole).toString().isEmpty());
    QVERIFY(model.data(index, EmailListModel::PgpMarkerAccessibleNameRole).toString().isEmpty());

    // roleNames() must expose exactly these 17 role-name strings for QML.
    const QHash<int, QByteArray> roles = model.roleNames();
    QCOMPARE(roles.size(), 17);
    QCOMPARE(roles.value(EmailListModel::MessageIdRole), QByteArrayLiteral("messageId"));
    QCOMPARE(roles.value(EmailListModel::KeywordsRole), QByteArrayLiteral("keywords"));
    QCOMPARE(roles.value(EmailListModel::HasAttachmentsRole), QByteArrayLiteral("hasAttachments"));
    QCOMPARE(roles.value(EmailListModel::SourceModeRole), QByteArrayLiteral("sourceMode"));
    QCOMPARE(roles.value(EmailListModel::PgpMarkerRole), QByteArrayLiteral("pgpMarker"));
    QCOMPARE(roles.value(EmailListModel::PgpMarkerAccessibleNameRole),
             QByteArrayLiteral("pgpMarkerAccessibleName"));
}

void EmailListModelTest::pgpMarkerRolesReflectMessageState()
{
    Email clientProtected = sampleEmail();
    clientProtected.pgpEncrypted = true;
    clientProtected.body = std::nullopt;

    Email decryptFailed = sampleEmail();
    decryptFailed.pgpEncrypted = true;
    decryptFailed.body = std::nullopt;
    decryptFailed.pgpDecryptError = QStringLiteral("no secret key");

    Email serverDecrypted = sampleEmail();
    serverDecrypted.pgpEncrypted = true;
    serverDecrypted.body = QStringLiteral("readable");

    EmailListModel model;
    model.setEmails({ clientProtected, decryptFailed, serverDecrypted });

    const auto marker = [&model](int row) {
        return model.data(model.index(row, 0), EmailListModel::PgpMarkerRole).toString();
    };
    const auto spoken = [&model](int row) {
        return model.data(model.index(row, 0), EmailListModel::PgpMarkerAccessibleNameRole).toString();
    };

    QCOMPARE(marker(0), QStringLiteral("\U0001F512"));
    QCOMPARE(marker(1), QStringLiteral("\u26A0"));
    // Server-decrypted rows read normally, so they are deliberately unmarked.
    QVERIFY(marker(2).isEmpty());

    QVERIFY(!spoken(0).isEmpty());
    QVERIFY(!spoken(1).isEmpty());
    QVERIFY(spoken(2).isEmpty());
}

void EmailListModelTest::emailAtOutOfRangeReturnsDefaultConstructedEmail()
{
    EmailListModel model;
    model.setEmails({ sampleEmail() });

    QCOMPARE(model.emailAt(-1), Email());
    QCOMPARE(model.emailAt(1), Email());
}

void EmailListModelTest::dataOutOfRangeReturnsInvalidVariant()
{
    EmailListModel model;
    model.setEmails({ sampleEmail() });

    QVERIFY(!model.data(model.index(1, 0), EmailListModel::MessageIdRole).isValid());
    QVERIFY(!model.data(QModelIndex(), EmailListModel::MessageIdRole).isValid());
}

QTEST_GUILESS_MAIN(EmailListModelTest)
#include "EmailListModelTest.moc"
