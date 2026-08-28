#include "pgp/MimeBodyReader.h"

#include <QList>
#include <QPair>
#include <QStringDecoder>

namespace {

// Bounds on attacker-controlled structure. Exceeding any of them stops the
// walk and keeps what was already found -- a message that renders partially
// is better than one that costs the process, and neither is worth an
// unbounded recursion on bytes a stranger chose.
//
// The numbers are generous against real mail: a signed, encrypted message
// with an alternative body and inline images nests about four deep, and a
// part count in the low hundreds means a mailing-list digest, not a message
// anybody is reading here.
constexpr int kMaxDepth = 8;
constexpr int kMaxParts = 64;
constexpr qsizetype kMaxHeaderBytes = 64 * 1024;
constexpr qsizetype kMaxWalkBytes = 8 * 1024 * 1024;

struct Entity
{
    QList<QPair<QByteArray, QByteArray>> headers; // field name lowercased, value unfolded
    QByteArray body;
    bool hasMimeHeaders = false; // Content-Type or MIME-Version was present
};

// Finds the blank line that ends a header block, tolerating both CRLF and
// bare LF. Mail arrives with both, and a decrypted part is whatever the
// sending client wrote rather than anything a server normalised.
qsizetype headerBlockEnd(const QByteArray& entity, qsizetype* bodyStart)
{
    const qsizetype crlf = entity.indexOf("\r\n\r\n");
    const qsizetype lf = entity.indexOf("\n\n");
    if (crlf >= 0 && (lf < 0 || crlf <= lf)) {
        *bodyStart = crlf + 4;
        return crlf;
    }
    if (lf >= 0) {
        *bodyStart = lf + 2;
        return lf;
    }
    return -1;
}

Entity parseEntity(const QByteArray& raw)
{
    Entity entity;

    qsizetype bodyStart = 0;
    const qsizetype headerEnd = headerBlockEnd(raw, &bodyStart);
    if (headerEnd < 0 || headerEnd > kMaxHeaderBytes) {
        // No header block, or one too large to be a real one. Either way the
        // bytes are not something to read structure out of.
        entity.body = raw;
        return entity;
    }

    entity.body = raw.mid(bodyStart);

    const QByteArray block = raw.left(headerEnd);
    QByteArray pending;
    const QList<QByteArray> lines = block.split('\n');
    auto flush = [&entity, &pending]() {
        if (pending.isEmpty())
            return;
        const qsizetype colon = pending.indexOf(':');
        if (colon > 0) {
            const QByteArray name = pending.left(colon).trimmed().toLower();
            const QByteArray value = pending.mid(colon + 1).trimmed();
            entity.headers.append({ name, value });
            if (name == "content-type" || name == "mime-version")
                entity.hasMimeHeaders = true;
        }
        pending.clear();
    };

    for (const QByteArray& rawLine : lines) {
        QByteArray line = rawLine;
        if (line.endsWith('\r'))
            line.chop(1);
        // A line beginning with whitespace continues the one before it
        // (RFC 5322 folding). Folded into a single space, which is what the
        // fold represented.
        if (!line.isEmpty() && (line.startsWith(' ') || line.startsWith('\t'))) {
            pending += ' ';
            pending += line.trimmed();
            continue;
        }
        flush();
        pending = line;
    }
    flush();

    return entity;
}

QByteArray headerOf(const Entity& entity, const char* name)
{
    for (const auto& header : entity.headers) {
        if (header.first == name)
            return header.second;
    }
    return {};
}

// The bare `type/subtype`, lowercased, with any parameters dropped.
QByteArray mimeTypeOf(const Entity& entity)
{
    const QByteArray value = headerOf(entity, "content-type");
    if (value.isEmpty())
        return "text/plain"; // RFC 2045's default for an entity that names none
    const qsizetype semicolon = value.indexOf(';');
    return (semicolon < 0 ? value : value.left(semicolon)).trimmed().toLower();
}

// One parameter out of a header value: `; name=value` or `; name="value"`.
// Case-insensitive on the name, as RFC 2045 requires.
QByteArray parameterOf(const QByteArray& headerValue, const char* name)
{
    const QByteArray needle = QByteArray(name).toLower();
    qsizetype at = 0;
    while ((at = headerValue.indexOf(';', at)) >= 0) {
        ++at;
        qsizetype segmentEnd = headerValue.indexOf(';', at);
        if (segmentEnd < 0)
            segmentEnd = headerValue.size();
        const qsizetype equals = headerValue.indexOf('=', at);
        // A parameter carrying no `=` of its own is skipped, never fatal.
        // Searching unbounded took the NEXT parameter's `=` instead and
        // swallowed both segments, so `multipart/mixed; flowed; boundary="b1"`
        // yielded no boundary at all -- and a sender writes that header.
        if (equals < 0 || equals > segmentEnd) {
            at = segmentEnd;
            continue;
        }
        const QByteArray key = headerValue.mid(at, equals - at).trimmed().toLower();
        qsizetype valueStart = equals + 1;
        while (valueStart < headerValue.size() && headerValue[valueStart] == ' ')
            ++valueStart;

        QByteArray value;
        if (valueStart < headerValue.size() && headerValue[valueStart] == '"') {
            const qsizetype closing = headerValue.indexOf('"', valueStart + 1);
            if (closing < 0)
                return {};
            value = headerValue.mid(valueStart + 1, closing - valueStart - 1);
            at = closing;
        } else {
            qsizetype end = headerValue.indexOf(';', valueStart);
            if (end < 0)
                end = headerValue.size();
            value = headerValue.mid(valueStart, end - valueStart).trimmed();
            at = end;
        }
        if (key == needle)
            return value;
    }
    return {};
}

QByteArray decodeQuotedPrintable(const QByteArray& input)
{
    QByteArray out;
    out.reserve(input.size());
    for (qsizetype i = 0; i < input.size(); ++i) {
        const char c = input.at(i);
        if (c != '=') {
            out.append(c);
            continue;
        }
        // Soft line break: `=` at end of line means the line was folded and
        // neither the `=` nor the break is part of the content.
        if (i + 1 < input.size() && input.at(i + 1) == '\n') {
            ++i;
            continue;
        }
        if (i + 2 < input.size() && input.at(i + 1) == '\r' && input.at(i + 2) == '\n') {
            i += 2;
            continue;
        }
        if (i + 2 < input.size()) {
            bool ok = false;
            const int byte = input.mid(i + 1, 2).toInt(&ok, 16);
            if (ok) {
                out.append(static_cast<char>(byte));
                i += 2;
                continue;
            }
        }
        // A stray `=` that decodes to nothing is kept verbatim rather than
        // dropped: it is far more likely to be a literal equals sign in prose
        // than a malformed escape, and silently deleting characters from a
        // message is worse than leaving one in.
        out.append(c);
    }
    return out;
}

QByteArray decodeTransfer(const QByteArray& body, const QByteArray& encoding)
{
    const QByteArray normalized = encoding.trimmed().toLower();
    if (normalized == "base64")
        return QByteArray::fromBase64(body);
    if (normalized == "quoted-printable")
        return decodeQuotedPrintable(body);
    // 7bit, 8bit, binary, absent, or something we do not know: the bytes are
    // the bytes. An unknown encoding is NOT an error worth losing the message
    // over, and guessing would be worse.
    return body;
}

QString decodeText(const QByteArray& bytes, const QByteArray& charset)
{
    if (!charset.isEmpty()) {
        QStringDecoder decoder(charset.constData());
        if (decoder.isValid()) {
            QString decoded = decoder(bytes);
            // hasError() catches a byte sequence that is not valid in the
            // charset the sender named. Falling through to UTF-8 would just
            // produce different mojibake, so the decoder's own replacement
            // characters are kept -- they at least came from the charset the
            // sender claimed.
            return decoded;
        }
    }
    // No charset, or one this build has no codec for. UTF-8 is the only
    // defensible default: it is what modern mail uses, and it degrades to
    // ASCII exactly.
    return QString::fromUtf8(bytes);
}

// Splits a multipart body on its boundary. Returns the parts between the
// delimiters, dropping the preamble before the first and anything after the
// closing `--boundary--`.
QList<QByteArray> splitOnBoundary(const QByteArray& body, const QByteArray& boundary)
{
    QList<QByteArray> parts;
    if (boundary.isEmpty())
        return parts;

    const QByteArray delimiter = "--" + boundary;
    qsizetype searchFrom = 0;
    qsizetype partStart = -1;

    while (searchFrom <= body.size()) {
        const qsizetype at = body.indexOf(delimiter, searchFrom);
        if (at < 0)
            break;

        // A delimiter counts only at the start of a line, or the sequence is
        // just bytes that happen to appear inside a part's content.
        if (at != 0 && body.at(at - 1) != '\n') {
            searchFrom = at + delimiter.size();
            continue;
        }

        // RFC 2046: the delimiter is the boundary followed by optional linear
        // whitespace and then a line break, or by "--" to close. Anything
        // else is a LONGER boundary that merely starts with this one --
        // "--b20" when we are splitting on "b2".
        //
        // Both checks happen before the pending part is closed off, and that
        // ordering is the whole point. An earlier version rejected the
        // candidate correctly but had already appended the part ending at
        // it, so the real part was truncated at the impostor and the
        // remainder -- headers and all -- was walked as a fresh entity. An
        // entity naming no Content-Type defaults to text/plain (RFC 2045),
        // so raw MIME source was handed up as the message the sender wrote.
        // Reproduced at depth 2 with boundaries "b2" and "b20"; the sender
        // chooses both.
        qsizetype cursor = at + delimiter.size();
        while (cursor < body.size() && (body.at(cursor) == ' ' || body.at(cursor) == '\t'))
            ++cursor;

        const bool closing = body.mid(cursor, 2) == "--";
        const bool terminated = closing || cursor >= body.size() || body.at(cursor) == '\r'
            || body.at(cursor) == '\n';
        if (!terminated) {
            searchFrom = at + delimiter.size();
            continue;
        }

        if (partStart >= 0) {
            qsizetype end = at;
            // The line break before a delimiter belongs to the delimiter, not
            // to the part -- RFC 2046 is explicit, and keeping it appends a
            // phantom blank line to every part.
            if (end > partStart && body.at(end - 1) == '\n')
                --end;
            if (end > partStart && body.at(end - 1) == '\r')
                --end;
            parts.append(body.mid(partStart, end - partStart));
        }

        if (closing)
            return parts; // anything after the closing delimiter is epilogue

        const qsizetype nextLine = body.indexOf('\n', cursor);
        if (nextLine < 0)
            return parts;
        partStart = nextLine + 1;
        searchFrom = partStart;
    }

    return parts;
}

void walk(const QByteArray& raw, int depth, int& partBudget, qsizetype& byteBudget, MimeBody& out)
{
    if (depth > kMaxDepth || partBudget <= 0 || raw.size() > byteBudget)
        return;
    --partBudget;
    byteBudget -= raw.size();

    const Entity entity = parseEntity(raw);
    const QByteArray contentType = headerOf(entity, "content-type");
    const QByteArray type = mimeTypeOf(entity);

    if (type.startsWith("multipart/")) {
        const QList<QByteArray> parts = splitOnBoundary(entity.body, parameterOf(contentType, "boundary"));
        for (const QByteArray& part : parts) {
            if (partBudget <= 0)
                return;
            walk(part, depth + 1, partBudget, byteBudget, out);
        }
        return;
    }

    if (type != "text/html" && type != "text/plain")
        return; // an attachment, an image, a signature: nothing to read here

    const QByteArray decoded =
        decodeTransfer(entity.body, headerOf(entity, "content-transfer-encoding"));
    const QString text = decodeText(decoded, parameterOf(contentType, "charset"));

    // A blank part is real content and is taken when nothing else is there,
    // but a later non-blank sibling wins -- mirroring kypost-android's
    // PgpMimeReader, so a message does not render as an empty page on one
    // client and as text on the other.
    QString& slot = (type == "text/html") ? out.html : out.plain;
    if (slot.isEmpty() || slot.trimmed().isEmpty())
        slot = text;
}

} // namespace

MimeBody readMimeBody(const QByteArray& entity)
{
    MimeBody out;
    if (entity.isEmpty())
        return out;

    // Inline PGP: no MIME entity at all, just the message. Detected by the
    // ABSENCE of Content-Type/MIME-Version rather than by the shape of the
    // first line, because prose beginning "Note: something" parses as a
    // header field perfectly well and eating the first paragraph of somebody's
    // mail is not a failure they can see or recover from.
    const Entity parsed = parseEntity(entity);
    if (!parsed.hasMimeHeaders) {
        out.plain = QString::fromUtf8(entity);
        return out;
    }

    int partBudget = kMaxParts;
    qsizetype byteBudget = kMaxWalkBytes;
    walk(entity, 0, partBudget, byteBudget, out);

    // Structure said MIME and the walk found no readable text: an entity
    // whose only parts are attachments, or one whose boundary this parser
    // does not recognise. Both leave the reader with nothing, which the
    // caller has to be able to tell apart from an empty message -- so
    // nothing is invented here.
    return out;
}
