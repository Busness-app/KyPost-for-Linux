#include "net/MfaResponseClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include <QJsonDocument>
#include <QJsonObject>

MfaResponseClient::MfaResponseClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

MfaResponseResult MfaResponseClient::respond(const QUrl& serverBaseUrl, const QString& challengeId,
                                              const QString& deviceId, const QString& deviceSecret, bool approve,
                                              const QString& matchDigits) const
{
    QJsonObject body;
    body[QStringLiteral("challengeId")] = challengeId;
    body[QStringLiteral("approve")] = approve;
    // Never on a deny: the safe answer must not depend on reading a number off
    // another screen, and the server ignores the field there anyway.
    body[QStringLiteral("matchDigits")] = approve ? matchDigits : QString();

    const RelayAuth auth{ deviceId, deviceSecret };
    const HttpClient::HttpResult result = m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/mfa/push/respond")), {}, body, auth.headerItems());

    MfaResponseResult out;

    if (result.error.has_value()) {
        switch (*result.error) {
        case NetworkError::Unauthorized:
            out.outcome = MfaResponseOutcome::Unauthorized;
            return out;
        case NetworkError::Conflict: {
            // 409 — challenge already resolved. status is optional here per
            // the brief; surface it when the server included it.
            out.outcome = MfaResponseOutcome::Rejected;
            const QJsonDocument doc = QJsonDocument::fromJson(result.body);
            if (doc.isObject()) {
                const QString status = doc.object().value(QStringLiteral("status")).toString();
                if (!status.isEmpty())
                    out.status = status;
            }
            return out;
        }
        default:
            out.outcome = MfaResponseOutcome::Failure;
            out.detail = !result.detail.isEmpty()
                ? result.detail
                : QStringLiteral("MFA response request failed with status %1").arg(result.statusCode);
            return out;
        }
    }

    QString errorString;
    const std::optional<QJsonObject> json = decodeJsonObject(result.body, &errorString);
    if (!json.has_value()) {
        out.outcome = MfaResponseOutcome::Failure;
        out.detail = QStringLiteral("Failed to decode MFA response: %1").arg(errorString);
        return out;
    }

    out.outcome = MfaResponseOutcome::Success;
    out.status = json->value(QStringLiteral("status")).toString();
    return out;
}
