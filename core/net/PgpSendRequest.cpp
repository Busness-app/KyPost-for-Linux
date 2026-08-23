#include "net/PgpSendRequest.h"

#include "pgp/PgpMimeWriter.h"

#include <QJsonArray>

namespace {

QJsonArray toJsonArray(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values)
        array.append(value);
    return array;
}

} // namespace

QJsonObject pgpSendRequestBody(const QString& from, const PgpSendPlan& plan, const QStringList& to,
                                const QStringList& cc, const QStringList& bcc, const QString& mode)
{
    QJsonArray deliveries;
    for (const PgpDelivery& delivery : plan.deliveries) {
        QJsonObject entry;
        entry[QStringLiteral("recipients")] = toJsonArray(delivery.smtpRecipients);
        entry[QStringLiteral("ciphertext")] = QString::fromUtf8(delivery.message);
        deliveries.append(entry);
    }

    QJsonObject body;
    body[QStringLiteral("from")] = from;
    // The placeholder, never the real subject. The relay's own comment says
    // this field is accepted and ignored and that no client should start
    // reading it -- the real subject is a protected header inside the
    // ciphertext, which is the entire point of this path.
    body[QStringLiteral("subject")] = kOuterPlaceholderSubject;
    body[QStringLiteral("deliveries")] = deliveries;
    body[QStringLiteral("to")] = toJsonArray(to);
    body[QStringLiteral("cc")] = toJsonArray(cc);
    body[QStringLiteral("bcc")] = toJsonArray(bcc);
    body[QStringLiteral("mode")] = mode;

    // sentCopyEncrypted asserts the copy is ciphertext. The relay stores a copy
    // only when it is, so claiming it for anything else would be asking the
    // relay to file the plaintext of a message it cannot read -- on the
    // account's IMAP host, which holds no key at all.
    if (!plan.sentCopy.isEmpty()) {
        body[QStringLiteral("sentCopy")] = QString::fromUtf8(plan.sentCopy);
        body[QStringLiteral("sentCopyEncrypted")] = true;
    }

    return body;
}
