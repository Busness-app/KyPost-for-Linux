#include "pgp/PgpMimeWriter.h"

#include <QRandomGenerator>

namespace {

// RFC 2047 caps an encoded-word at 75 characters including the delimiters.
// Base64 emits 4 characters per 3 input bytes, so this is the largest chunk
// of input whose encoded form still fits alongside "=?utf-8?B?" and "?=".
constexpr int kEncodedWordPayloadBytes = 42;

bool isPureAscii(const QString& value)
{
    for (const QChar c : value) {
        if (c.unicode() > 0x7E || c.unicode() < 0x20)
            return false;
    }
    return true;
}

// One RFC 2047 base64 encoded-word per chunk, folded with CRLF + space.
//
// Split on UTF-8 byte boundaries rather than characters: an encoded-word must
// decode on its own, so a multi-byte character straddling two words would
// decode to replacement characters in every conforming reader.
QString encodedWord(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    QStringList words;
    qsizetype at = 0;
    while (at < utf8.size()) {
        qsizetype take = qMin<qsizetype>(kEncodedWordPayloadBytes, utf8.size() - at);
        // Do not cut a UTF-8 sequence in half: continuation bytes are 10xxxxxx.
        while (take > 1 && at + take < utf8.size()
               && (static_cast<unsigned char>(utf8.at(at + take)) & 0xC0) == 0x80) {
            --take;
        }
        words.append(QStringLiteral("=?utf-8?B?")
                     + QString::fromLatin1(utf8.mid(at, take).toBase64()) + QStringLiteral("?="));
        at += take;
    }
    return words.join(QStringLiteral("\r\n "));
}

QString joinAddresses(const QStringList& addresses)
{
    QStringList cleaned;
    cleaned.reserve(addresses.size());
    for (const QString& address : addresses) {
        const QString value = mimeHeaderValue(address);
        if (!value.isEmpty())
            cleaned.append(value);
    }
    return cleaned.join(QStringLiteral(", "));
}

QByteArray contentTypeForMode(const QString& mode)
{
    return mode.compare(QStringLiteral("html"), Qt::CaseInsensitive) == 0
        ? QByteArrayLiteral("text/html; charset=utf-8")
        : QByteArrayLiteral("text/plain; charset=utf-8");
}

// A filename safe to sit inside a quoted-string parameter.
//
// Line breaks go first (mimeHeaderValue's job everywhere else), then the two
// characters that can end or escape a quoted string. A name carrying a quote
// would otherwise close the parameter early and let the rest be read as
// further parameters -- the same shape as header injection, one level down.
//
// RFC 2231 continuation encoding is NOT implemented, so a non-ASCII name
// travels as raw UTF-8 inside the quotes. That is technically non-conformant
// and universally understood; the alternative worth having is 2231, not
// mangling the user's filename into ASCII.
QString quotedFilename(const QString& raw)
{
    QString value = raw;
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.remove(QLatin1Char('"'));
    value.remove(QLatin1Char('\\'));
    value = value.trimmed();
    return value.isEmpty() ? QStringLiteral("attachment") : value;
}

// base64 wrapped at the conventional 76 characters. One unbroken line is legal
// base64 and is not legal MIME: RFC 5322 caps a line at 998 octets, and a
// 25 MB attachment on one line is refused or truncated by things along the way.
QByteArray wrappedBase64(const QByteArray& data)
{
    const QByteArray encoded = data.toBase64();
    QByteArray out;
    out.reserve(encoded.size() + encoded.size() / 76 * 2 + 2);
    for (qsizetype at = 0; at < encoded.size(); at += 76) {
        out += encoded.mid(at, 76);
        out += "\r\n";
    }
    return out;
}

} // namespace

QString mimeHeaderValue(const QString& raw)
{
    QString value = raw;
    // Both, and to a SPACE rather than removed: deleting them would silently
    // join two words, and the point is only that neither can end a header
    // line. Matches the server's mailmsg.SanitizeHeaderValue.
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value = value.trimmed();
    if (value.isEmpty() || isPureAscii(value))
        return value;
    return encodedWord(value);
}

QString randomMimeBoundary(const QByteArray& mustNotOccurIn)
{
    // The affixes matter as much as the randomness: OpenPGP armor is base64
    // plus dashes and newlines, so a boundary containing '_' cannot appear in
    // the encrypted part however the bytes fall.
    for (int attempt = 0; attempt < 8; ++attempt) {
        quint32 words[4] = { 0, 0, 0, 0 };
        QRandomGenerator::system()->generate(std::begin(words), std::end(words));
        QString boundary = QStringLiteral("=_kypost_");
        for (const quint32 word : words)
            boundary += QStringLiteral("%1").arg(word, 8, 16, QLatin1Char('0'));
        boundary += QStringLiteral("_=");

        // 128 bits makes an accidental collision impossible in practice, but
        // the content here is a body the caller composed and the check costs
        // one scan -- so it is checked rather than reasoned about.
        if (!mustNotOccurIn.contains(boundary.toUtf8()))
            return boundary;
    }
    return QStringLiteral("=_kypost_fallback_=");
}

QByteArray protectedContent(const OutgoingMessage& message, const QString& boundary)
{
    const QString subject = mimeHeaderValue(message.subject);
    const QByteArray boundaryUtf8 = boundary.toUtf8();

    QByteArray out;
    if (!subject.isEmpty())
        out += "Subject: " + subject.toUtf8() + "\r\n";
    out += "Content-Type: multipart/mixed; boundary=\"" + boundaryUtf8
        + "\"; protected-headers=\"v1\"\r\n";
    out += "\r\n";

    // The "legacy display" part: a client that does not understand protected
    // headers still renders this, so the subject is not simply invisible to
    // everyone but Thunderbird.
    if (!subject.isEmpty()) {
        out += "--" + boundaryUtf8 + "\r\n";
        out += "Content-Type: text/rfc822-headers; protected-headers=\"v1\"\r\n";
        out += "Content-Disposition: inline\r\n";
        out += "\r\n";
        out += "Subject: " + subject.toUtf8() + "\r\n";
        out += "\r\n";
    }

    out += "--" + boundaryUtf8 + "\r\n";
    out += "Content-Type: " + contentTypeForMode(message.mode) + "\r\n";
    out += "\r\n";
    out += message.body.toUtf8();
    // Per RFC 2046 the CRLF before a delimiter belongs to the delimiter, so it
    // is written unconditionally rather than assumed to be at the end of the
    // body.
    out += "\r\n";

    for (const MailAttachmentUpload& attachment : message.attachments) {
        const QByteArray name = quotedFilename(attachment.name).toUtf8();
        const QByteArray type = attachment.mimeType.trimmed().isEmpty()
            ? QByteArrayLiteral("application/octet-stream")
            : mimeHeaderValue(attachment.mimeType).toUtf8();

        out += "--" + boundaryUtf8 + "\r\n";
        out += "Content-Type: " + type + "; name=\"" + name + "\"\r\n";
        out += "Content-Transfer-Encoding: base64\r\n";
        out += "Content-Disposition: attachment; filename=\"" + name + "\"\r\n";
        out += "\r\n";
        out += wrappedBase64(attachment.data);
    }

    out += "--" + boundaryUtf8 + "--\r\n";
    return out;
}

QByteArray pgpMimeDelivery(const OutgoingMessage& message, const QString& armoredCiphertext,
                            const QString& boundary)
{
    const QByteArray boundaryUtf8 = boundary.toUtf8();

    QByteArray out;
    // Exactly one From. The relay refuses duplicates rather than resolving
    // them, because a second From above a signed one is a standard way to make
    // what a verifier checks and what a reader sees differ.
    out += "From: " + mimeHeaderValue(message.from).toUtf8() + "\r\n";
    out += "To: " + joinAddresses(message.to).toUtf8() + "\r\n";
    if (!message.cc.isEmpty())
        out += "Cc: " + joinAddresses(message.cc).toUtf8() + "\r\n";
    // The placeholder, never message.subject. The real one is inside the
    // ciphertext.
    out += "Subject: " + kOuterPlaceholderSubject.toUtf8() + "\r\n";
    out += "Date: " + mimeHeaderValue(message.date).toUtf8() + "\r\n";
    out += "MIME-Version: 1.0\r\n";
    out += "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\""
        + boundaryUtf8 + "\"\r\n";
    out += "\r\n";

    out += "--" + boundaryUtf8 + "\r\n";
    out += "Content-Type: application/pgp-encrypted\r\n";
    out += "Content-Description: PGP/MIME version identification\r\n";
    out += "\r\n";
    out += "Version: 1\r\n";
    out += "\r\n";

    out += "--" + boundaryUtf8 + "\r\n";
    out += "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n";
    out += "Content-Description: OpenPGP encrypted message\r\n";
    out += "Content-Disposition: inline; filename=\"encrypted.asc\"\r\n";
    out += "\r\n";
    out += armoredCiphertext.toUtf8();
    out += "\r\n";
    out += "--" + boundaryUtf8 + "--\r\n";
    return out;
}
