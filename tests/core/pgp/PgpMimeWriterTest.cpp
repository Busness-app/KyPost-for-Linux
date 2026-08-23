#include "pgp/PgpMimeWriter.h"

#include <QTest>

namespace {

const QString kArmor = QStringLiteral("-----BEGIN PGP MESSAGE-----\r\n\r\nhQEMA0abc\r\n=xY1z\r\n"
                                       "-----END PGP MESSAGE-----");

OutgoingMessage sampleMessage()
{
    OutgoingMessage message;
    message.from = QStringLiteral("me@example.com");
    message.to = { QStringLiteral("you@example.com") };
    message.subject = QStringLiteral("Quarterly numbers");
    message.body = QStringLiteral("Attached.");
    message.mode = QStringLiteral("plain");
    message.date = QStringLiteral("Sat, 23 Aug 2026 12:00:00 +0000");
    return message;
}

// The header block only, so a test asserting a header is absent cannot be
// satisfied (or defeated) by the body.
QByteArray headerBlockOf(const QByteArray& message)
{
    const qsizetype end = message.indexOf("\r\n\r\n");
    return end < 0 ? message : message.left(end);
}

// Whether a header of this name is actually PRESENT, which means a line that
// begins with it -- not the name appearing anywhere in the block.
//
// The distinction is the entire question an injection test asks. A sanitized
// "you@example.com  Bcc: attacker@evil.example" contains the text "Bcc:" and
// injects nothing, because it is one To value with no line break in it; a
// substring search cannot tell that apart from a real added header, and
// reports the safe case as a breach.
bool hasHeader(const QByteArray& headerBlock, const QByteArray& name)
{
    for (const QByteArray& line : headerBlock.split('\n')) {
        QByteArray trimmed = line;
        if (trimmed.endsWith('\r'))
            trimmed.chop(1);
        if (trimmed.startsWith(name))
            return true;
    }
    return false;
}

} // namespace

class PgpMimeWriterTest : public QObject
{
    Q_OBJECT

private slots:
    void theDeliveryCarriesEveryHeaderTheRelayRequires();
    void theDeliveryCarriesNoHeaderTheRelayForbids();
    void theRealSubjectNeverAppearsOnTheOuterEnvelope();
    void aSubjectCannotInjectAnotherHeader();
    void anAddressCannotInjectAnotherHeader();
    void thereIsExactlyOneFrom();
    void theEncryptedPartIsTheArmorVerbatim();
    void theDeliveryIsMultipartEncryptedWithThePgpProtocol();
    void asciiHeadersAreEmittedUnchanged();
    void nonAsciiHeadersBecomeEncodedWords();
    void aLongNonAsciiSubjectFoldsWithoutSplittingACharacter();
    void protectedContentCarriesTheSubjectWhereBothKindsOfClientFindIt();
    void protectedContentWithNoSubjectHasNoLegacyPart();
    void htmlIsDeclaredAsHtml();
    void aBoundaryCannotOccurInArmor();
    void aBoundaryIsNotReusedBetweenCalls();
};

void PgpMimeWriterTest::theDeliveryCarriesEveryHeaderTheRelayRequires()
{
    const QByteArray headers =
        headerBlockOf(pgpMimeDelivery(sampleMessage(), kArmor, randomMimeBoundary()));

    // handleMailSendPGP's requiredDeliveryHeaders, exactly.
    QVERIFY2(headers.contains("From: me@example.com"), "missing From");
    QVERIFY2(headers.contains("To: you@example.com"), "missing To");
    QVERIFY2(headers.contains("Subject: "), "missing Subject");
    QVERIFY2(headers.contains("Date: Sat, 23 Aug 2026 12:00:00 +0000"), "missing Date");
    QVERIFY2(headers.contains("MIME-Version: 1.0"), "missing MIME-Version");
}

// The relay refuses these outright: a client supplying them asserts a delivery
// history that did not happen, and a forged Authentication-Results is what the
// anti-phishing scan reads.
void PgpMimeWriterTest::theDeliveryCarriesNoHeaderTheRelayForbids()
{
    OutgoingMessage message = sampleMessage();
    message.cc = { QStringLiteral("cc@example.com") };
    const QByteArray headers = headerBlockOf(pgpMimeDelivery(message, kArmor, randomMimeBoundary()));

    QVERIFY2(!hasHeader(headers, "Bcc:"), "a Bcc header was emitted");
    QVERIFY2(!hasHeader(headers, "Received:"), "a Received header was emitted");
    QVERIFY2(!hasHeader(headers, "Authentication-Results:"), "an Authentication-Results header was emitted");
    QVERIFY2(!hasHeader(headers, "Return-Path:"), "a Return-Path header was emitted");
}

// The whole reason this path exists. The relay cannot read the ciphertext, so
// a real subject on the outside would be the one part of the message it -- and
// every hop after it -- could still read.
void PgpMimeWriterTest::theRealSubjectNeverAppearsOnTheOuterEnvelope()
{
    OutgoingMessage message = sampleMessage();
    message.subject = QStringLiteral("Redundancies confirmed");

    const QByteArray delivery = pgpMimeDelivery(message, kArmor, randomMimeBoundary());

    QVERIFY2(!delivery.contains("Redundancies"), "the real subject travelled in cleartext");
    QVERIFY2(delivery.contains("Subject: [Encrypted] Email Sent by KyPost"),
             "the placeholder subject is not byte-identical to the server's");
}

// A Subject is typed by the user and relayed verbatim, so a newline in it
// would otherwise add headers of the sender's -- or a phisher's -- choosing.
//
// Measured, not assumed: deleting the CR/LF replacement does not actually
// reopen this, because a value containing CR or LF is not pure ASCII and so
// takes the encoded-word path instead, which is base64 and has no line breaks
// either. Both assertions below are kept -- the first is the property that
// matters, the second is what detects the replacement going missing.
void PgpMimeWriterTest::aSubjectCannotInjectAnotherHeader()
{
    OutgoingMessage message = sampleMessage();
    message.subject = QStringLiteral("Hello\r\nBcc: attacker@evil.example");

    // Injected through the PROTECTED subject, which is the one place the real
    // subject is written out.
    const QByteArray content = protectedContent(message, randomMimeBoundary());

    QVERIFY2(!hasHeader(content, "Bcc:"), "a Bcc header was injected through the subject");
    // Two spaces: CR and LF each become one, rather than the pair being
    // deleted. Removing them would silently join two words, and the property
    // that matters is only that neither can end a header line.
    QVERIFY2(content.contains("Subject: Hello  Bcc: attacker@evil.example"),
             "the line breaks were not neutralised into spaces");
}

void PgpMimeWriterTest::anAddressCannotInjectAnotherHeader()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("you@example.com\r\nBcc: attacker@evil.example") };

    const QByteArray headers = headerBlockOf(pgpMimeDelivery(message, kArmor, randomMimeBoundary()));

    QVERIFY2(!hasHeader(headers, "Bcc:"), "a Bcc header was injected through a recipient address");
    // It survives as inert text on the To line, which is the point: there is
    // no line break left in it, so it cannot become a header of its own.
    QVERIFY(headers.contains("To: you@example.com  Bcc: attacker@evil.example"));
}

// The relay refuses a second From rather than choosing between them, because
// which one a receiving MTA honours is not ours to guess.
void PgpMimeWriterTest::thereIsExactlyOneFrom()
{
    const QByteArray headers =
        headerBlockOf(pgpMimeDelivery(sampleMessage(), kArmor, randomMimeBoundary()));
    QCOMPARE(headers.count("From: "), 1);
}

void PgpMimeWriterTest::theEncryptedPartIsTheArmorVerbatim()
{
    const QByteArray delivery = pgpMimeDelivery(sampleMessage(), kArmor, randomMimeBoundary());

    QVERIFY2(delivery.contains(kArmor.toUtf8()), "the ciphertext was altered on the way out");
    QVERIFY(delivery.contains("Content-Type: application/octet-stream; name=\"encrypted.asc\""));
    // RFC 3156's version part, which receiving clients key off.
    QVERIFY(delivery.contains("Content-Type: application/pgp-encrypted"));
    QVERIFY(delivery.contains("Version: 1"));
}

void PgpMimeWriterTest::theDeliveryIsMultipartEncryptedWithThePgpProtocol()
{
    const QByteArray headers =
        headerBlockOf(pgpMimeDelivery(sampleMessage(), kArmor, randomMimeBoundary()));

    // The relay parses this with mime.ParseMediaType and refuses anything that
    // is not exactly this media type and protocol -- "the armor marker appears
    // somewhere" is explicitly not enough for it.
    QVERIFY(headers.contains("Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\""));
}

// Byte-parity with the server for the common case: an ASCII subject must not
// arrive encoded from one client and raw from the other.
void PgpMimeWriterTest::asciiHeadersAreEmittedUnchanged()
{
    QCOMPARE(mimeHeaderValue(QStringLiteral("Quarterly numbers")), QStringLiteral("Quarterly numbers"));
    QCOMPARE(mimeHeaderValue(QStringLiteral("me@example.com")), QStringLiteral("me@example.com"));
}

void PgpMimeWriterTest::nonAsciiHeadersBecomeEncodedWords()
{
    const QString encoded = mimeHeaderValue(QStringLiteral("Café résumé"));

    QVERIFY2(encoded.startsWith(QStringLiteral("=?utf-8?B?")), "non-ASCII was emitted raw in a header");
    QVERIFY(encoded.endsWith(QStringLiteral("?=")));
    // And it round-trips.
    const QString payload = encoded.mid(10, encoded.size() - 12);
    QCOMPARE(QString::fromUtf8(QByteArray::fromBase64(payload.toLatin1())),
             QStringLiteral("Café résumé"));
}

// An encoded-word must decode on its own, so a multi-byte character split
// across two of them decodes to replacement characters in every conforming
// reader.
void PgpMimeWriterTest::aLongNonAsciiSubjectFoldsWithoutSplittingACharacter()
{
    const QString subject = QStringLiteral("é").repeated(60);
    const QString encoded = mimeHeaderValue(subject);

    QVERIFY2(encoded.contains(QStringLiteral("\r\n ")), "a long header was not folded");

    QString decoded;
    for (const QString& word : encoded.split(QStringLiteral("\r\n "))) {
        QVERIFY(word.startsWith(QStringLiteral("=?utf-8?B?")) && word.endsWith(QStringLiteral("?=")));
        const QString payload = word.mid(10, word.size() - 12);
        decoded += QString::fromUtf8(QByteArray::fromBase64(payload.toLatin1()));
    }
    QCOMPARE(decoded, subject);
    QVERIFY2(!decoded.contains(QChar(0xFFFD)), "a character was split across two encoded-words");
}

void PgpMimeWriterTest::protectedContentCarriesTheSubjectWhereBothKindsOfClientFindIt()
{
    const QByteArray content = protectedContent(sampleMessage(), QStringLiteral("=_kypost_test_="));

    // On the wrapper, for a client that understands protected headers...
    QVERIFY(content.startsWith("Subject: Quarterly numbers\r\n"));
    QVERIFY(content.contains("protected-headers=\"v1\""));
    // ...and in the legacy-display part, for every client that does not.
    QVERIFY(content.contains("Content-Type: text/rfc822-headers; protected-headers=\"v1\""));
    QCOMPARE(content.count("Subject: Quarterly numbers"), 2);
    QVERIFY(content.contains("Attached."));
}

void PgpMimeWriterTest::protectedContentWithNoSubjectHasNoLegacyPart()
{
    OutgoingMessage message = sampleMessage();
    message.subject = QString();

    const QByteArray content = protectedContent(message, QStringLiteral("=_kypost_test_="));

    QVERIFY2(!content.contains("Subject:"), "an empty subject was written out anyway");
    QVERIFY2(!content.contains("text/rfc822-headers"),
             "an empty legacy-display part was emitted for a message with no subject");
    QVERIFY(content.contains("Attached."));
}

void PgpMimeWriterTest::htmlIsDeclaredAsHtml()
{
    OutgoingMessage message = sampleMessage();
    message.mode = QStringLiteral("html");
    message.body = QStringLiteral("<p>Attached.</p>");

    const QByteArray content = protectedContent(message, QStringLiteral("=_kypost_test_="));

    QVERIFY(content.contains("Content-Type: text/html; charset=utf-8"));
    QVERIFY(!content.contains("Content-Type: text/plain"));
}

// OpenPGP armor is base64 plus dashes and newlines. A boundary containing '_'
// cannot occur inside the encrypted part however the bytes fall, which is a
// stronger statement than "128 bits is a lot".
void PgpMimeWriterTest::aBoundaryCannotOccurInArmor()
{
    const QString boundary = randomMimeBoundary();

    QVERIFY2(boundary.contains(QLatin1Char('_')),
             "the boundary uses only characters that armor can also contain");
    QVERIFY(boundary.startsWith(QStringLiteral("=_kypost_")));
}

void PgpMimeWriterTest::aBoundaryIsNotReusedBetweenCalls()
{
    QVERIFY(randomMimeBoundary() != randomMimeBoundary());

    // And a value that does occur in the content is not handed back.
    const QString first = randomMimeBoundary();
    const QString second = randomMimeBoundary(first.toUtf8());
    QVERIFY(second != first);
}

QTEST_GUILESS_MAIN(PgpMimeWriterTest)
#include "PgpMimeWriterTest.moc"
