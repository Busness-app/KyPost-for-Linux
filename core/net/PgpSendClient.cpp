#include "net/PgpSendClient.h"

#include "net/RelayAuth.h"
#include "pgp/PgpMimeWriter.h"

#include <QJsonArray>
#include <QJsonObject>

namespace {

QJsonArray toJsonArray(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values)
        array.append(value);
    return array;
}

} // namespace

PgpSendClient::PgpSendClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

PgpSendResult PgpSendClient::send(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                   const QString& from, const PgpSendPlan& plan,
                                   const QStringList& to, const QStringList& cc,
                                   const QStringList& bcc, const QString& mode) const
{
    PgpSendResult out;

    if (plan.status != PgpSendPlanStatus::Built || plan.deliveries.isEmpty()) {
        // Refused here rather than sent and rejected: a plan that did not build
        // is one whose recipients could not all be encrypted to, and the whole
        // point of the planner is that such a send does not happen.
        out.error = NetworkError::InvalidUrl;
        out.detail = QStringLiteral("no deliveries to send");
        return out;
    }

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

    const HttpClient::HttpResult result = m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/mail/send-pgp")), {}, body, auth.headerItems());

    if (result.error.has_value()) {
        out.error = result.error;
        // Failures on this endpoint arrive as plain text, not JSON, so the body
        // is the detail when there is one.
        const QString bodyText = QString::fromUtf8(result.body).trimmed();
        out.detail = !result.detail.isEmpty() ? result.detail
            : !bodyText.isEmpty()             ? bodyText
                                               : QStringLiteral("Send failed with status %1")
                                                     .arg(result.statusCode);
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode send response: %1").arg(errorString);
        return out;
    }

    out.ok = decoded->value(QStringLiteral("ok")).toBool();
    // Absent means NOT saved. The field is the relay's assertion that a copy
    // was filed, and a missing assertion is not one.
    out.sentSaved = decoded->value(QStringLiteral("sentSaved")).toBool();
    out.warningDetail = decoded->value(QStringLiteral("warning")).toString().trimmed();
    out.warned = !out.warningDetail.isEmpty();

    // The planner already knows there will be no copy when the sender has no
    // key of their own. Reporting that as the relay having lost one would
    // point the user at the wrong problem.
    if (plan.sentCopy.isEmpty())
        out.sentSaved = false;

    return out;
}
