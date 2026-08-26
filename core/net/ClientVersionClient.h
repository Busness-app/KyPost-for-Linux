#pragma once

#include "net/NetworkError.h"

#include <QString>
#include <QUrl>
#include <optional>

class HttpClient;
struct RelayAuth;

// What the paired server reports about the newest published Linux client
// release. This client does NOT decide whether an update is available: the
// comparison happens against the compiled-in KYPOST_VERSION, which is the
// left-hand side and stays on this side of the wire.
struct ClientVersionResult
{
    // Empty when the server has nothing to report: before its first check
    // completes, while the newest release is still inside its soak window, or
    // when the repository has published no releases. All ordinary states.
    QString latestVersion;
    // RFC3339, exactly as the server sent it. Empty if never checked.
    QString checkedAt;
    // False when the server has no such endpoint (404). Every server released
    // before this feature is in that state, so it must read as "no
    // information" rather than as a failure.
    bool supported = true;
    std::optional<NetworkError> error;
};

// GET {serverBaseUrl}/api/client/version.
//
// Goes to the paired server, NOT to GitHub. That is what keeps the update
// check inside the certificate pin and adds no third-party egress -- see
// docs/superpowers/specs/2026-08-25-linux-update-check-design.md.
class ClientVersionClient
{
public:
    explicit ClientVersionClient(HttpClient& httpClient);
    ClientVersionResult fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const;

private:
    HttpClient& m_httpClient;
};
