#include "mail/PgpMessagePresentation.h"

#include <QTest>
#include <QUrl>
#include <QUrlQuery>

class PgpMessagePresentationTest : public QObject
{
    Q_OBJECT

private slots:
    void marksOnlyTheUnreadableStates();
    void everyMarkerHasAnAccessibleName();
    void bannerCopyExistsForEveryStateExceptNone();
    void decryptFailedBannerIncludesTheServersReason();
    void buildsWebmailUrl();
    void rejectsUnsafeWebmailBaseUrls_data();
    void rejectsUnsafeWebmailBaseUrls();
    void webmailMailboxUrlTargetsTheMailboxWithNoMessageId();
    void webmailMailboxUrlRejectsAnythingButHttps();
    void webmailMailboxUrlDropsAnyBaseQueryOrFragment();
};

void PgpMessagePresentationTest::marksOnlyTheUnreadableStates()
{
    QCOMPARE(pgpRowMarker(PgpMessageState::ClientProtected), QStringLiteral("\U0001F512"));
    QCOMPARE(pgpRowMarker(PgpMessageState::DecryptFailed), QStringLiteral("⚠"));

    // Deliberately unmarked: these rows open and read normally, so a glyph
    // would appear on most rows of a server-mode mailbox carrying nothing
    // the user can act on.
    QVERIFY(pgpRowMarker(PgpMessageState::None).isEmpty());
    QVERIFY(pgpRowMarker(PgpMessageState::DecryptedByServer).isEmpty());
}

void PgpMessagePresentationTest::everyMarkerHasAnAccessibleName()
{
    // A glyph with no spoken equivalent is invisible to a screen reader, so
    // the two must agree on which states are marked.
    for (const PgpMessageState state : { PgpMessageState::None, PgpMessageState::ClientProtected,
                                          PgpMessageState::DecryptFailed, PgpMessageState::DecryptedByServer }) {
        QCOMPARE(pgpRowMarker(state).isEmpty(), pgpRowMarkerAccessibleName(state).isEmpty());
    }
}

void PgpMessagePresentationTest::bannerCopyExistsForEveryStateExceptNone()
{
    QVERIFY(pgpBannerTitle(PgpMessageState::None).isEmpty());
    QVERIFY(pgpBannerBody(PgpMessageState::None, QString()).isEmpty());

    for (const PgpMessageState state : { PgpMessageState::ClientProtected, PgpMessageState::DecryptFailed,
                                          PgpMessageState::DecryptedByServer }) {
        QVERIFY(!pgpBannerTitle(state).isEmpty());
        QVERIFY(!pgpBannerBody(state, QString()).isEmpty());
    }
}

void PgpMessagePresentationTest::decryptFailedBannerIncludesTheServersReason()
{
    const QString withReason = pgpBannerBody(PgpMessageState::DecryptFailed, QStringLiteral("no secret key"));
    QVERIFY(withReason.contains(QStringLiteral("no secret key")));

    // A blank reason must not produce a dangling "failed: " with nothing
    // after it.
    const QString withoutReason = pgpBannerBody(PgpMessageState::DecryptFailed, QStringLiteral("   "));
    QVERIFY(!withoutReason.trimmed().endsWith(QLatin1Char(':')));
    QVERIFY(!withoutReason.isEmpty());
}

void PgpMessagePresentationTest::buildsWebmailUrl()
{
    const QUrl url = webmailReadUrl(QUrl(QStringLiteral("https://mail.urlxl.com")),
                                     QStringLiteral("INBOX"), QStringLiteral("abc-123"));
    QCOMPARE(url.scheme(), QStringLiteral("https"));
    QCOMPARE(url.host(), QStringLiteral("mail.urlxl.com"));
    QCOMPARE(url.path(), QStringLiteral("/read"));
    QCOMPARE(url.query(), QStringLiteral("mailbox=INBOX&message=abc-123"));

    // A message id with URL-significant characters must survive intact
    // rather than splitting the query.
    const QUrl escaped = webmailReadUrl(QUrl(QStringLiteral("https://mail.urlxl.com")),
                                         QStringLiteral("Archive/2026"), QStringLiteral("a&b=c d"));
    QCOMPARE(QUrlQuery(escaped).queryItemValue(QStringLiteral("message"), QUrl::FullyDecoded),
             QStringLiteral("a&b=c d"));
    QCOMPARE(QUrlQuery(escaped).queryItemValue(QStringLiteral("mailbox"), QUrl::FullyDecoded),
             QStringLiteral("Archive/2026"));

    // A base URL carrying its own query or fragment must not leak into the
    // link.
    const QUrl noisy = webmailReadUrl(QUrl(QStringLiteral("https://mail.urlxl.com/?a=b#frag")),
                                       QStringLiteral("INBOX"), QStringLiteral("m1"));
    QCOMPARE(noisy.query(), QStringLiteral("mailbox=INBOX&message=m1"));
    QVERIFY(!noisy.hasFragment());

    // Mailbox is optional.
    const QUrl noMailbox = webmailReadUrl(QUrl(QStringLiteral("https://mail.urlxl.com")),
                                           QString(), QStringLiteral("m1"));
    QCOMPARE(noMailbox.query(), QStringLiteral("message=m1"));
}

void PgpMessagePresentationTest::rejectsUnsafeWebmailBaseUrls_data()
{
    QTest::addColumn<QString>("base");
    QTest::addColumn<QString>("messageId");

    // This URL is handed to Qt.openUrlExternally(), so anything that isn't a
    // real https host must produce nothing at all.
    QTest::newRow("http downgrade") << "http://mail.urlxl.com" << "m1";
    QTest::newRow("file") << "file:///etc/passwd" << "m1";
    QTest::newRow("javascript") << "javascript:alert(1)" << "m1";
    QTest::newRow("no host") << "https:///read" << "m1";
    QTest::newRow("empty") << "" << "m1";
    QTest::newRow("relative") << "/read" << "m1";
    QTest::newRow("empty message id") << "https://mail.urlxl.com" << "";
}

void PgpMessagePresentationTest::rejectsUnsafeWebmailBaseUrls()
{
    QFETCH(QString, base);
    QFETCH(QString, messageId);

    QVERIFY(webmailReadUrl(QUrl(base), QStringLiteral("INBOX"), messageId).isEmpty());
}

void PgpMessagePresentationTest::webmailMailboxUrlTargetsTheMailboxWithNoMessageId()
{
    const QUrl url = webmailMailboxUrl(QUrl(QStringLiteral("https://mail.example.com")),
                                        QStringLiteral("Drafts"));

    QCOMPARE(url.toString(), QStringLiteral("https://mail.example.com/read?mailbox=Drafts"));
}

// Same containment rule as webmailReadUrl: this URL goes to an external
// browser, so a pairing holding a file://, javascript: or downgraded http
// base must never produce one.
void PgpMessagePresentationTest::webmailMailboxUrlRejectsAnythingButHttps()
{
    QVERIFY(webmailMailboxUrl(QUrl(QStringLiteral("http://mail.example.com")),
                              QStringLiteral("Drafts")).isEmpty());
    QVERIFY(webmailMailboxUrl(QUrl(QStringLiteral("file:///etc/passwd")),
                              QStringLiteral("Drafts")).isEmpty());
    QVERIFY(webmailMailboxUrl(QUrl(QStringLiteral("javascript:alert(1)")),
                              QStringLiteral("Drafts")).isEmpty());
    QVERIFY(webmailMailboxUrl(QUrl(), QStringLiteral("Drafts")).isEmpty());
    QVERIFY(webmailMailboxUrl(QUrl(QStringLiteral("https://")), QStringLiteral("Drafts")).isEmpty());
}

// A base URL carrying its own query or fragment must not leak into the link.
void PgpMessagePresentationTest::webmailMailboxUrlDropsAnyBaseQueryOrFragment()
{
    const QUrl url = webmailMailboxUrl(
        QUrl(QStringLiteral("https://mail.example.com/?tracking=1#section")), QStringLiteral("Drafts"));

    QCOMPARE(url.toString(), QStringLiteral("https://mail.example.com/read?mailbox=Drafts"));
}

QTEST_APPLESS_MAIN(PgpMessagePresentationTest)
#include "PgpMessagePresentationTest.moc"
