#include "net/FolderClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace {

const QString kFoldersPath = QStringLiteral("api/inbox/folders");

// The mutating verbs answer 400/502 with a plain-text body (http.Error),
// not JSON, so there is nothing to decode -- the body IS the message
// ("built-in folders cannot be renamed", "folder must have a parent
// mailbox"). Prefer it over a status-code string whenever it looks like
// text, and cap the length so a stray HTML error page can't become a
// multi-kilobyte toast.
QString detailFromResult(const HttpClient::HttpResult& result, const QString& fallback)
{
    if (!result.detail.isEmpty())
        return result.detail;

    const QString body = QString::fromUtf8(result.body).trimmed();
    if (!body.isEmpty() && !body.startsWith(QLatin1Char('{')) && !body.startsWith(QLatin1Char('<'))
        && body.size() <= 200) {
        return body;
    }
    return fallback;
}

FolderMutationResult mutationFromResult(const HttpClient::HttpResult& result, const QString& verb)
{
    FolderMutationResult out;
    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = detailFromResult(
            result, QStringLiteral("Folder %1 failed with status %2").arg(verb).arg(result.statusCode));
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode folder %1 response: %2").arg(verb, errorString);
        return out;
    }

    const QJsonObject json = *decoded;
    out.ok = json.value(QStringLiteral("ok")).toBool();
    out.parent = json.value(QStringLiteral("parent")).toString();
    // create answers with `folder`, rename with `renamed`, delete with
    // `folder`. Normalise to one field so callers don't branch on the verb.
    out.folder = json.contains(QStringLiteral("renamed"))
        ? json.value(QStringLiteral("renamed")).toString()
        : json.value(QStringLiteral("folder")).toString();
    return out;
}

} // namespace

FolderClient::FolderClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

FolderListResult FolderClient::list(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                     const QString& parent) const
{
    QList<QPair<QString, QString>> query;
    // Sent even when empty: the backend trims and treats "" as "top level",
    // and always echoes `parent` back, so this keeps request and response
    // symmetric.
    query.append({ QStringLiteral("parent"), parent });

    const HttpClient::HttpResult result =
        m_httpClient.get(joinUrlPath(serverBaseUrl, kFoldersPath), query, auth.headerItems());

    FolderListResult out;
    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = detailFromResult(
            result, QStringLiteral("Folder list failed with status %1").arg(result.statusCode));
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode folder list response: %1").arg(errorString);
        return out;
    }

    const QJsonObject json = *decoded;
    out.parent = json.value(QStringLiteral("parent")).toString();

    const QJsonArray array = json.value(QStringLiteral("folders")).toArray();
    out.folders.reserve(array.size());
    for (const QJsonValue& value : array) {
        const QJsonObject obj = value.toObject();
        MailFolder folder;
        folder.path = obj.value(QStringLiteral("path")).toString();
        folder.deletable = obj.value(QStringLiteral("deletable")).toBool();
        // Not per-folder on the wire; stamped from the response's echoed
        // parent so FolderDao rows are addressable by parent.
        folder.parent = out.parent;
        if (!folder.path.isEmpty())
            out.folders.append(folder);
    }

    return out;
}

FolderMutationResult FolderClient::create(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                           const QString& parent, const QString& name) const
{
    QJsonObject body;
    body[QStringLiteral("parent")] = parent;
    body[QStringLiteral("name")] = name;

    return mutationFromResult(
        m_httpClient.post(joinUrlPath(serverBaseUrl, kFoldersPath), {}, body, auth.headerItems()),
        QStringLiteral("create"));
}

FolderMutationResult FolderClient::rename(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                           const QString& folder, const QString& name) const
{
    QJsonObject body;
    body[QStringLiteral("folder")] = folder;
    body[QStringLiteral("name")] = name;

    return mutationFromResult(
        m_httpClient.put(joinUrlPath(serverBaseUrl, kFoldersPath), {}, body, auth.headerItems()),
        QStringLiteral("rename"));
}

FolderMutationResult FolderClient::remove(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                           const QString& folder) const
{
    QList<QPair<QString, QString>> query;
    query.append({ QStringLiteral("folder"), folder });

    return mutationFromResult(
        m_httpClient.del(joinUrlPath(serverBaseUrl, kFoldersPath), query, auth.headerItems()),
        QStringLiteral("delete"));
}
