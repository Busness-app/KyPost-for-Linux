#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Builds the two MIME structures a client-encrypted send is made of.
//
// The relay's POST /api/mail/send-pgp relays these bytes VERBATIM -- it
// synthesizes no headers and cannot read inside the ciphertext -- so whatever
// this produces is the entire message as far as the receiving MTA is
// concerned. handleMailSendPGP validates the shape and refuses anything that
// is not a complete RFC 5322 message; the rules below are that contract, not
// this file's preferences.
//
// Matched against the server's own pgpmail.protectContent /
// buildEncryptedEnvelope rather than written from the RFCs alone: both KyPost
// clients have to produce the same mail, and a receiving Thunderbird or K-9
// has to recognise it.

// One outgoing message, before any OpenPGP work.
//
// `to`/`cc` are the addresses that appear in the DELIVERY HEADERS. They are
// not the SMTP recipient list -- a BCC recipient gets their own delivery whose
// headers do not mention them, which is why there is no bcc field here at all
// and why a Bcc header is one this writer must never emit.
struct OutgoingMessage
{
    QString from; // exactly one address; the relay binds this to the caller
    QStringList to;
    QStringList cc;
    QString subject; // the REAL subject -- see kOuterPlaceholderSubject
    QString body;
    QString mode; // "plain" or "html"
    // RFC 5322 date, passed in rather than read from the clock: core/ stays
    // testable, and the caller already knows when it is sending.
    QString date;
};

// What the outer, unencrypted envelope carries in place of the real subject.
//
// Byte-identical to the server's pgpmail.OuterPlaceholderSubject. It is not
// decoration: the real subject travels only inside the ciphertext, as a
// protected header, and an outer Subject that leaked it would undo the point
// of encrypting on a path the relay explicitly cannot read.
inline const QString kOuterPlaceholderSubject = QStringLiteral("[Encrypted] Email Sent by KyPost");

// A header value that cannot inject another header.
//
// CR and LF become spaces, on every value without exception -- subject,
// addresses, the lot -- because all of them originate in the compose screen.
// A Subject containing "\r\nBcc: attacker@example.com" would otherwise add a
// recipient the sender never typed, and the relay relays bytes verbatim.
//
// There are two independent defences here, which is worth knowing before
// changing either. Removing this replacement does NOT open the injection back
// up: a value containing CR or LF is by definition not pure ASCII, so it takes
// the encoded-word path below and comes out base64, which has no line breaks
// in it either. The replacement is the primary and legible defence; the
// encoding is a second one that happens to cover the same ground.
//
// Non-ASCII is RFC 2047 encoded-word wrapped, which is what RFC 5322 requires
// of a header field and what the server's own ExtractProtectedSubject decodes.
// Pure-ASCII values are emitted unchanged, so the common case is byte-for-byte
// what the server produces.
QString mimeHeaderValue(const QString& raw);

// A MIME boundary that cannot occur in the content it delimits.
//
// 128 bits from the system CSPRNG, wrapped in characters OpenPGP armor never
// contains. Callers hand it the content so a value that does occur is
// regenerated rather than assumed away.
QString randomMimeBoundary(const QByteArray& mustNotOccurIn = {});

// The plaintext entity that gets encrypted: Protected Headers v1, the
// "memoryhole" convention draft-ietf-lamps-header-protection describes and
// that Thunderbird, Mutt and K-9 both emit and consume.
//
// The real subject appears twice on purpose -- once on the wrapper's own
// headers so an aware client shows it, and once in a leading
// text/rfc822-headers "legacy display" part so every other client still
// renders it readably. Scope is Subject-only, the LAMPS baseline, matching
// the server.
QByteArray protectedContent(const OutgoingMessage& message, const QString& boundary);

// The RFC 3156 multipart/encrypted delivery around an already-armored
// ciphertext.
//
// Carries From, To, Subject (the placeholder) and Date, which are exactly the
// four headers the relay requires, and never Bcc, Received,
// Authentication-Results or Return-Path, which are the four it refuses.
QByteArray pgpMimeDelivery(const OutgoingMessage& message, const QString& armoredCiphertext,
                            const QString& boundary);
