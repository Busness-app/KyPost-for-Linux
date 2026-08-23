#pragma once

#include <QByteArray>
#include <QString>

// The readable text of a decrypted OpenPGP message.
//
// Both forms are kept rather than one "best" body. A message carrying only a
// plain part must not render as an empty page, and the caller -- not this
// parser -- decides which surface it has to render into.
struct MimeBody
{
    QString html;
    QString plain;

    bool isEmpty() const { return html.isEmpty() && plain.isEmpty(); }
    bool operator==(const MimeBody&) const = default;
};

// Extracts the readable text from the decrypted body of an OpenPGP message.
//
// WHY THIS IS HAND-WRITTEN rather than KMime. `libkf6mime-dev` is not in
// Ubuntu noble, which is what CI builds on; taking it from the KDE neon
// archive layered on top is exactly the shape that broke the build on
// 2026-08-23 (neon's libgpgmepp-dev requires gpgme >= 2.0.0, noble carries
// 1.x). core/'s QtCore/QtNetwork/QtSql-only boundary points the same way.
// The scope here is genuinely small -- one MIME entity, two content types we
// care about -- so this is a bounded parser rather than a general one, and it
// does not try to be KMime.
//
// WHAT IT DELIBERATELY DOES NOT DO: RFC 2231 parameter continuations
// (`boundary*0=`), message/rfc822 recursion, and any attachment handling. An
// entity using them yields a boundary this parser does not recognise, the
// multipart fails to split, and the fallback below applies. Losing formatting
// is an acceptable failure; guessing at a structure is not.
//
// INPUT IS ATTACKER-CONTROLLED -- it is whatever the sender encrypted, and
// the relay never saw it, so nothing upstream has sanity-checked its shape.
// Nesting depth, part count and header size are all bounded; see the
// constants in the .cpp. Exceeding a bound stops the walk and returns
// whatever was already found, never a partial part.
//
// Bytes that are not a MIME entity at all -- inline PGP, which is still
// common -- are returned whole as `plain`. That case is detected by requiring
// a Content-Type or MIME-Version header, NOT by guessing from the shape of
// the first line: plain prose beginning "Note: ..." parses as a header field
// perfectly well, and treating it as one would silently eat the first
// paragraph of the message.
MimeBody readMimeBody(const QByteArray& entity);
