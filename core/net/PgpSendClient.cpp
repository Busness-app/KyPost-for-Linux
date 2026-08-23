#include "net/PgpSendClient.h"

#include "net/PgpSendRequest.h"

#include "net/RelayAuth.h"
#include "pgp/PgpMimeWriter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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

    const QJsonObject body = pgpSendRequestBody(from, plan, to, cc, bcc, mode);

    // Measured on the real serialized request, not estimated from the
    // message: the multiplier is the delivery count, and the armor and base64
    // inflation compound, so an estimate would be wrong in whichever
    // direction is least convenient.
    if (const qint64 size = QJsonDocument(body).toJson(QJsonDocument::Compact).size();
        size > kMaxRequestBytes) {
        out.tooLarge = true;
        out.error = NetworkError::ResponseTooLarge;
        out.detail = QStringLiteral("request is %1 bytes; the relay reads at most %2")
                          .arg(size)
                          .arg(kMaxRequestBytes);
        return out;
    }

    const HttpClient::HttpResult result = m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/mail/send-pgp")), {}, body, auth.headerItems());

    if (result.error.has_value()) {
        out.error = result.error;
        // The relay refusing the size is the same answer as this client
        // refusing it, and the user's move is the same either way.
        out.tooLarge = result.statusCode == 413;
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
