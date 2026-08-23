#pragma once

#include "pgp/PgpMimeWriter.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// Turns one composed message into the exact set of ciphertexts the relay's
// POST /api/mail/send/pgp expects, and the encrypted copy for Sent.
//
// The split matches the server's own buildPGPDeliveries, because both clients
// have to produce the same mail:
//
//   * ONE delivery for all To and Cc recipients together, encrypted to all of
//     their keys -- they can see each other anyway;
//   * ONE delivery PER Bcc recipient, encrypted to that recipient alone, so a
//     blind recipient never appears in another's encryption headers. That is
//     the whole reason the wire format is a list rather than one message: a
//     single ciphertext encrypted to everyone would name every Bcc recipient
//     in its key IDs, which is the disclosure Bcc exists to prevent.
//
// Every delivery carries the SAME visible headers -- To and Cc, never Bcc --
// so a blind recipient sees exactly what the others do. Only the SMTP envelope
// and the set of keys differ.

// One entry of the send request: who it goes to over SMTP, and the complete
// RFC 5322 message to relay verbatim.
struct PgpDelivery
{
    QStringList smtpRecipients;
    QByteArray message; // a full RFC 3156 multipart/encrypted message

    bool operator==(const PgpDelivery&) const = default;
};

enum class PgpSendPlanStatus
{
    Built,

    // At least one recipient has no usable key. The send is refused rather
    // than downgraded: the relay's own plaintext fallback works by storing the
    // message on the relay, which is the thing client-side protection exists
    // to prevent, and silently dropping a recipient is worse still.
    RecipientWithoutKey,

    // No secret key for the sender, so nothing can be signed. Not a warning:
    // an unsigned message from an account that normally signs is a downgrade
    // the recipient cannot see.
    SigningUnavailable,

    EncryptionFailed,
    EngineUnavailable,
};

struct PgpSendPlan
{
    PgpSendPlanStatus status = PgpSendPlanStatus::EngineUnavailable;
    QVector<PgpDelivery> deliveries;

    // The message to file in Sent, encrypted to the sender's own key. Empty
    // when there is none.
    QByteArray sentCopy;

    // THE SENT COPY IS NEVER STORED IN CLEARTEXT. The relay refuses a copy
    // that does not claim to be encrypted, and it is right to: the copy is
    // APPENDed to the account's IMAP host, which is somebody else's machine
    // holding no key at all. A readable copy there would put the body and the
    // real subject of every encrypted message in the clear -- the exact
    // disclosure the sender chose this mode to prevent.
    //
    // So when the sender has no key of their own, no copy is saved. That is a
    // real cost and must NOT be silent: the message did go out, and the user
    // has to be told their outbox will not have it.
    bool sentCopyUnavailable = false;

    // Named, so the user is told WHO could not be written to rather than that
    // "encryption failed".
    QStringList recipientsWithoutKeys;
    QString detail; // for a log line; never user-facing wording
};

// Builds the plan.
//
// `message` supplies From, To, Cc, Subject, body, mode and Date. Bcc is passed
// separately and deliberately never reaches OutgoingMessage, which has no
// field for it -- a Bcc header on a delivery is one the relay refuses outright.
//
// `fingerprintsByAddress` maps every recipient address to the key to encrypt
// to. An address missing from it, or mapped to an empty fingerprint, is a
// recipient without a key and fails the whole plan. Fingerprints rather than
// addresses because an address can match more than one key in a keyring, and
// "whichever gpg picked" is not a decision this code may make silently.
//
// `senderFingerprint` is the sender's own key, used only for the Sent copy.
// Empty means no copy is produced and sentCopyUnavailable is set. Signing uses
// `message.from` instead, so a sender who can sign but has no encryption key
// still sends -- they just lose the copy.
PgpSendPlan buildPgpSendPlan(const OutgoingMessage& message, const QStringList& bcc,
                             const QHash<QString, QString>& fingerprintsByAddress,
                             const QString& senderFingerprint,
                             const QString& homeDirectory = QString());
