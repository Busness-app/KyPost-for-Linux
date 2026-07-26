#pragma once

#include "models/MailFolder.h"
#include "net/NetworkError.h"

#include <QString>
#include <QUrl>
#include <QVector>
#include <optional>

class HttpClient;
struct RelayAuth;

// GET /api/inbox/folders[?parent=] -> {parent, folders: [{path, deletable}]}
struct FolderListResult
{
    std::optional<NetworkError> error;
    QString detail;
    QString parent; // echoed back by the backend; "" for the top level
    QVector<MailFolder> folders;
};

// Shared shape for the three mutating verbs. `folder` is the resulting full
// path (created, renamed-to, or deleted), so a caller can refresh or select
// it without re-deriving the path-joining rule the server used.
struct FolderMutationResult
{
    std::optional<NetworkError> error;
    QString detail;
    bool ok = false;
    QString folder;
    QString parent;
};

// Mailbox listing and management.
//
// Modeled on GroupsClient's request/parse shape. The one structural
// difference: the backend refuses to rename or delete a built-in mailbox or
// any top-level folder, so those failures come back as 400 with a plain-text
// body (http.Error, not writeJSON) -- unlike every JSON endpoint this repo
// otherwise talks to. `detail` therefore falls back to the raw body text,
// which is already human-readable, rather than trying to decode it.
class FolderClient
{
public:
    explicit FolderClient(HttpClient& httpClient);

    // `parent` empty lists the top level; "Archive" lists Archive's
    // subfolders, which is the case that motivated this client.
    FolderListResult list(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& parent) const;

    FolderMutationResult create(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& parent,
                                 const QString& name) const;
    FolderMutationResult rename(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& folder,
                                 const QString& name) const;
    FolderMutationResult remove(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& folder) const;

private:
    HttpClient& m_httpClient;
};
