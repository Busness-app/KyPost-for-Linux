#include "domain/PgpMessageState.h"

#include "models/Email.h"

#include <QTest>

class PgpMessageStateTest : public QObject
{
    Q_OBJECT

private slots:
    void classifies_data();
    void classifies();
    void emailOverloadReadsTheModelFields();
};

void PgpMessageStateTest::classifies_data()
{
    QTest::addColumn<bool>("pgpEncrypted");
    QTest::addColumn<QString>("decryptError");
    QTest::addColumn<std::optional<QString>>("body");
    QTest::addColumn<PgpMessageState>("expected");

    const std::optional<QString> noBody;
    const std::optional<QString> nullBody = std::nullopt;
    const std::optional<QString> emptyBody = QStringLiteral("");
    const std::optional<QString> blankBody = QStringLiteral("   \n\t ");
    const std::optional<QString> realBody = QStringLiteral("Hello there");

    // Not encrypted: every other field is irrelevant. Guards against a
    // future reordering that checks the error or body first.
    QTest::newRow("plain, no body") << false << QString() << noBody << PgpMessageState::None;
    QTest::newRow("plain, with body") << false << QString() << realBody << PgpMessageState::None;
    QTest::newRow("plain, stray error") << false << QStringLiteral("ignored") << realBody << PgpMessageState::None;

    // Error wins over the body. The server sets the error AND leaves the
    // body empty, so if the body were checked first this row would be
    // ClientProtected and send the user to webmail for a message that will
    // fail there too.
    QTest::newRow("error, null body") << true << QStringLiteral("no secret key") << nullBody
                                       << PgpMessageState::DecryptFailed;
    QTest::newRow("error, empty body") << true << QStringLiteral("no secret key") << emptyBody
                                        << PgpMessageState::DecryptFailed;
    QTest::newRow("error, with body") << true << QStringLiteral("bad mdc") << realBody
                                       << PgpMessageState::DecryptFailed;
    // Whitespace-only error must not count as an error, or a server sending
    // "" padded with a newline would mask a client-protected message.
    QTest::newRow("blank error is not an error") << true << QStringLiteral("  \n ") << nullBody
                                                  << PgpMessageState::ClientProtected;

    // Encrypted, no error, readable body: the server decrypted it.
    QTest::newRow("server-decrypted") << true << QString() << realBody << PgpMessageState::DecryptedByServer;

    // Encrypted, no error, no body: nobody but the user can read it.
    QTest::newRow("client-protected, null body") << true << QString() << nullBody
                                                  << PgpMessageState::ClientProtected;
    QTest::newRow("client-protected, empty body") << true << QString() << emptyBody
                                                   << PgpMessageState::ClientProtected;
    // A body of only whitespace is not readable content; treating it as
    // DecryptedByServer would render a blank message with no explanation,
    // which is the exact bug this feature exists to remove.
    QTest::newRow("client-protected, blank body") << true << QString() << blankBody
                                                   << PgpMessageState::ClientProtected;
}

void PgpMessageStateTest::classifies()
{
    QFETCH(bool, pgpEncrypted);
    QFETCH(QString, decryptError);
    QFETCH(std::optional<QString>, body);
    QFETCH(PgpMessageState, expected);

    QCOMPARE(pgpMessageStateOf(pgpEncrypted, decryptError, body), expected);
}

void PgpMessageStateTest::emailOverloadReadsTheModelFields()
{
    Email email;
    QCOMPARE(pgpMessageStateOf(email), PgpMessageState::None);

    email.pgpEncrypted = true;
    QCOMPARE(pgpMessageStateOf(email), PgpMessageState::ClientProtected);

    email.body = QStringLiteral("readable");
    QCOMPARE(pgpMessageStateOf(email), PgpMessageState::DecryptedByServer);

    email.pgpDecryptError = QStringLiteral("boom");
    QCOMPARE(pgpMessageStateOf(email), PgpMessageState::DecryptFailed);
}

QTEST_APPLESS_MAIN(PgpMessageStateTest)
#include "PgpMessageStateTest.moc"
