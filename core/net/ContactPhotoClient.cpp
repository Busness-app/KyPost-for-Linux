#include "net/ContactPhotoClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

namespace {

// Builds "api/contacts/<contactUid>/photo" and joins it onto serverBaseUrl's
// path via the shared joinUrlPath() helper (see HttpClient.h).
//
// contactUid is ONE opaque segment and is encoded as one. It used to be
// concatenated raw, on the belief that setPath() would encode it: setPath()
// encodes '?' and '#' but leaves '/' alone, because a slash is a legal path
// character. So a uid of "../../api/notifications/native/pull" -- reachable
// through contact sync or a vCard import, neither of which this client
// controls -- built a URL that resolves to a DIFFERENT authenticated
// endpoint on the same origin, and the device secret went to it.
//
// Encoding cannot rescue a uid that IS a dot segment: "." and ".." contain
// no character an encoder touches. Those are refused, along with an empty
// uid (which would yield "//" and a path one segment short). nullopt means
// "no request", not "request that fails" -- the credentials never leave.
std::optional<QUrl> endpointFor(const QUrl& serverBaseUrl, const QString& contactUid)
{
    const QString segment = QString::fromLatin1(QUrl::toPercentEncoding(contactUid));
    if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String(".."))
        return std::nullopt;

    return joinUrlPath(serverBaseUrl,
                        QStringLiteral("api/contacts/") + segment + QStringLiteral("/photo"));
}

} // namespace

ContactPhotoClient::ContactPhotoClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

ContactPhotoFetchResult ContactPhotoClient::fetch(const QUrl& serverBaseUrl, const QString& contactUid,
                                                    const RelayAuth& auth) const
{
    ContactPhotoFetchResult out;

    const std::optional<QUrl> endpoint = endpointFor(serverBaseUrl, contactUid);
    if (!endpoint.has_value()) {
        out.error = NetworkError::InvalidUrl;
        out.detail = QStringLiteral("Contact uid is not a usable URL path segment");
        return out;
    }

    const HttpClient::HttpResult result =
        m_httpClient.get(*endpoint, {}, auth.headerItems(), {}, kMaxPhotoBytes);

    // Covers 401/403/5xx/transport failures alike -- HttpClient::get()
    // already maps the status code to NetworkError, so every non-2xx path
    // lands here and returns empty photoBytes rather than throwing/crashing,
    // same reasoning as GroupsClient::fetch().
    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty()
            ? result.detail
            : QStringLiteral("Contact photo fetch failed with status %1").arg(result.statusCode);
        return out;
    }

    // No JSON parsing here, unlike GroupsClient -- the response body is the
    // photo's raw bytes verbatim, per task-3-brief.md ("raw bytes"), so
    // there's no Decoding-error branch to add.
    out.photoBytes = result.body;
    return out;
}
