#pragma once

#include "net/NetworkError.h"

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

class HttpClient;
struct RelayAuth;

// Response from GET {serverBaseUrl}/api/contacts/{id}/photo. Must degrade
// gracefully on 401/any HTTP error -- empty photoBytes, error set, never a
// crash -- per this feature's Global Constraints (task-3-brief.md, same
// wording as GroupsClient's identical rule in task-2-brief.md): the backend
// endpoint this depends on is an unverified external dependency in a
// separate repo, never assume it's deployed.
struct ContactPhotoFetchResult
{
    std::optional<NetworkError> error;
    QString detail; // human-readable detail on error; empty otherwise
    QByteArray photoBytes;
};

// Second per-resource GET client in this repo, alongside GroupsClient (see
// core/net/GroupsClient.h for the shape this mirrors: same constructor
// shape, same error-handling branch, same "raw HttpClient::get() through to
// a result struct" flow) -- fetches one contact's photo bytes on demand.
//
// Unlike GroupsClient's fixed /api/groups path, this endpoint carries the
// contact's uid as a path segment (/api/contacts/{id}/photo) -- the only
// runtime value this repo interpolates into a URL path. endpointFor() in the
// .cpp percent-encodes it to exactly one segment and refuses the handful of
// values encoding cannot make safe; see the comment there for what went
// through the raw concatenation this replaced.
class ContactPhotoClient
{
public:
    // Ceiling for one photo, well under HttpClient's generic 8 MiB default.
    // This is an avatar: the default let a relay hand back 8 MiB per contact
    // viewed, which is both an allocation and -- via ContactPhotoCache -- a
    // write to the user's disk.
    static constexpr qint64 kMaxPhotoBytes = 2LL * 1024 * 1024;

    explicit ContactPhotoClient(HttpClient& httpClient);

    ContactPhotoFetchResult fetch(const QUrl& serverBaseUrl, const QString& contactUid, const RelayAuth& auth) const;

private:
    HttpClient& m_httpClient;
};
