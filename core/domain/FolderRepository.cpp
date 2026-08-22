#include "domain/FolderRepository.h"

#include "db/FolderDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/FolderClient.h"
#include "net/RelayAuth.h"

#include <QUrl>
#include <algorithm>

namespace {

// Matches the value MailRepository's rows carry, so a mixed cache stays
// self-consistent if a future source mode is ever introduced.
const QString kSourceMode = QStringLiteral("relay");

MailRepositoryOutcome outcomeFromNetworkError(NetworkError error)
{
    switch (error) {
    case NetworkError::Unauthorized:
        return MailRepositoryOutcome::Unauthorized;
    case NetworkError::ServiceUnavailable:
        return MailRepositoryOutcome::ServiceUnavailable;
    default:
        return MailRepositoryOutcome::Retry;
    }
}

} // namespace

FolderRepository::FolderRepository(FolderClient& client, FolderDao& folderDao, PairingStore& pairingStore)
    : m_client(client)
    , m_folderDao(folderDao)
    , m_pairingStore(pairingStore)
{
}

std::optional<RelayRequestPlan> FolderRepository::planRequest() const
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return std::nullopt;
    return RelayRequestPlan{ RelayEndpoint{ QUrl(pairing->serverBaseUrl),
                                             RelayAuth{ pairing->deviceId, pairing->deviceSecret } },
                              identityOf(*pairing) };
}

FolderListResult FolderRepository::listWith(HttpClient& httpClient, const RelayEndpoint& endpoint,
                                             const QString& parent)
{
    // Constructed here rather than reused from m_client: FolderClient is a
    // stateless wrapper over an HttpClient reference, and on the async path
    // that HttpClient belongs to the executor thread.
    FolderClient client(httpClient);
    return client.list(endpoint.serverBaseUrl, endpoint.auth, parent);
}

MailFetchOutcome FolderRepository::applyList(const RelayRequestPlan& plan, const QString& parent,
                                              const FolderListResult& result)
{
    // Before the snapshot replace below, which is destructive in both
    // directions: it would publish the previous account's mailbox names into
    // the new account's sidebar AND delete the new account's own rows for
    // that parent.
    if (!m_pairingStore.stillCurrent(plan.identity))
        return { MailRepositoryOutcome::PairingChanged, QString() };

    if (result.error.has_value())
        return { outcomeFromNetworkError(*result.error), result.detail };

    // Replace rather than upsert: a folder deleted on the server is only
    // observable as an absence from this list.
    m_folderDao.replaceParentSnapshot(parent, result.folders, kSourceMode);
    return { MailRepositoryOutcome::Success, QString() };
}

FolderRepository::FolderMutationFetch FolderRepository::createWith(HttpClient& httpClient,
                                                                     const RelayEndpoint& endpoint,
                                                                     const QString& parent, const QString& name)
{
    FolderClient client(httpClient);
    FolderMutationFetch fetched;
    fetched.mutation = client.create(endpoint.serverBaseUrl, endpoint.auth, parent, name);
    if (fetched.mutation.error.has_value())
        return fetched;
    fetched.listedParent = parent;
    fetched.listing = client.list(endpoint.serverBaseUrl, endpoint.auth, parent);
    return fetched;
}

FolderRepository::FolderMutationFetch FolderRepository::renameWith(HttpClient& httpClient,
                                                                     const RelayEndpoint& endpoint,
                                                                     const QString& folder, const QString& name)
{
    FolderClient client(httpClient);
    FolderMutationFetch fetched;
    fetched.mutation = client.rename(endpoint.serverBaseUrl, endpoint.auth, folder, name);
    if (fetched.mutation.error.has_value())
        return fetched;
    // The response's `parent` is the authority on which subtree changed --
    // a rename keeps the folder under its existing parent, but deriving that
    // here would mean re-implementing the server's separator rules.
    fetched.listedParent = fetched.mutation.parent;
    fetched.listing = client.list(endpoint.serverBaseUrl, endpoint.auth, fetched.listedParent);
    return fetched;
}

FolderRepository::FolderMutationFetch FolderRepository::removeWith(HttpClient& httpClient,
                                                                     const RelayEndpoint& endpoint,
                                                                     const QString& folder)
{
    FolderClient client(httpClient);
    FolderMutationFetch fetched;
    fetched.mutation = client.remove(endpoint.serverBaseUrl, endpoint.auth, folder);
    if (fetched.mutation.error.has_value())
        return fetched;
    fetched.listedParent = fetched.mutation.parent;
    fetched.listing = client.list(endpoint.serverBaseUrl, endpoint.auth, fetched.listedParent);
    return fetched;
}

FolderRepository::FolderMutationOutcome FolderRepository::applyMutation(const RelayRequestPlan& plan,
                                                                          const FolderMutationFetch& fetched)
{
    // The mutation itself already happened on the server, under the previous
    // account, and cannot be taken back from here. What must not happen is
    // writing its result into the account paired now.
    if (!m_pairingStore.stillCurrent(plan.identity))
        return { MailRepositoryOutcome::PairingChanged, QString(), QString() };

    if (fetched.mutation.error.has_value())
        return { outcomeFromNetworkError(*fetched.mutation.error), fetched.mutation.detail, QString() };

    // The re-list's own outcome is deliberately discarded, matching the
    // synchronous form: the mutation succeeded on the server, and a failed
    // re-list only means the sidebar is stale until the next refresh. Turning
    // it into a failure would report "could not create folder" for a folder
    // that exists.
    applyList(plan, fetched.listedParent, fetched.listing);
    return { MailRepositoryOutcome::Success, QString(), fetched.mutation.folder };
}

MailFetchOutcome FolderRepository::refresh(const QString& parent)
{
    const std::optional<RelayRequestPlan> plan = planRequest();
    if (!plan.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired") };

    return applyList(*plan, parent, m_client.list(plan->endpoint.serverBaseUrl, plan->endpoint.auth, parent));
}

QVector<MailFolder> FolderRepository::cachedFolders(const QString& parent) const
{
    const QVector<FolderRecord> records = m_folderDao.findByParent(parent);

    QVector<MailFolder> folders;
    folders.reserve(records.size());
    for (const FolderRecord& record : records)
        folders.append(MailFolder{ record.path, record.parent, record.deletable });

    // The backend returns whatever order IMAP's LIST gave it; sort here so
    // the sidebar doesn't reshuffle between refreshes.
    std::sort(folders.begin(), folders.end(), [](const MailFolder& a, const MailFolder& b) {
        return a.path.localeAwareCompare(b.path) < 0;
    });
    return folders;
}

// The three synchronous mutating verbs, kept as compositions of the phases
// above so the async path cannot drift from the behaviour their tests pin.
// Each re-uses m_client (this thread's) rather than the *With() statics.

FolderRepository::FolderMutationOutcome FolderRepository::create(const QString& parent, const QString& name)
{
    const std::optional<RelayRequestPlan> plan = planRequest();
    if (!plan.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired"), QString() };

    FolderMutationFetch fetched;
    fetched.mutation = m_client.create(plan->endpoint.serverBaseUrl, plan->endpoint.auth, parent, name);
    if (!fetched.mutation.error.has_value()) {
        fetched.listedParent = parent;
        fetched.listing = m_client.list(plan->endpoint.serverBaseUrl, plan->endpoint.auth, parent);
    }
    return applyMutation(*plan, fetched);
}

FolderRepository::FolderMutationOutcome FolderRepository::rename(const QString& folder, const QString& name)
{
    const std::optional<RelayRequestPlan> plan = planRequest();
    if (!plan.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired"), QString() };

    FolderMutationFetch fetched;
    fetched.mutation = m_client.rename(plan->endpoint.serverBaseUrl, plan->endpoint.auth, folder, name);
    if (!fetched.mutation.error.has_value()) {
        fetched.listedParent = fetched.mutation.parent;
        fetched.listing = m_client.list(plan->endpoint.serverBaseUrl, plan->endpoint.auth, fetched.listedParent);
    }
    return applyMutation(*plan, fetched);
}

FolderRepository::FolderMutationOutcome FolderRepository::remove(const QString& folder)
{
    const std::optional<RelayRequestPlan> plan = planRequest();
    if (!plan.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired"), QString() };

    FolderMutationFetch fetched;
    fetched.mutation = m_client.remove(plan->endpoint.serverBaseUrl, plan->endpoint.auth, folder);
    if (!fetched.mutation.error.has_value()) {
        fetched.listedParent = fetched.mutation.parent;
        fetched.listing = m_client.list(plan->endpoint.serverBaseUrl, plan->endpoint.auth, fetched.listedParent);
    }
    return applyMutation(*plan, fetched);
}
