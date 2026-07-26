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

MailFetchOutcome FolderRepository::refresh(const QString& parent)
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired") };

    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    const FolderListResult result = m_client.list(QUrl(pairing->serverBaseUrl), auth, parent);
    if (result.error.has_value())
        return { outcomeFromNetworkError(*result.error), result.detail };

    // Replace rather than upsert: a folder deleted on the server is only
    // observable as an absence from this list.
    m_folderDao.replaceParentSnapshot(parent, result.folders, kSourceMode);
    return { MailRepositoryOutcome::Success, QString() };
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

FolderRepository::FolderMutationOutcome FolderRepository::create(const QString& parent, const QString& name)
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired"), QString() };

    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    const FolderMutationResult result = m_client.create(QUrl(pairing->serverBaseUrl), auth, parent, name);
    if (result.error.has_value())
        return { outcomeFromNetworkError(*result.error), result.detail, QString() };

    refresh(parent);
    return { MailRepositoryOutcome::Success, QString(), result.folder };
}

FolderRepository::FolderMutationOutcome FolderRepository::rename(const QString& folder, const QString& name)
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired"), QString() };

    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    const FolderMutationResult result = m_client.rename(QUrl(pairing->serverBaseUrl), auth, folder, name);
    if (result.error.has_value())
        return { outcomeFromNetworkError(*result.error), result.detail, QString() };

    // The response's `parent` is the authority on which subtree changed --
    // a rename keeps the folder under its existing parent, but deriving that
    // here would mean re-implementing the server's separator rules.
    refresh(result.parent);
    return { MailRepositoryOutcome::Success, QString(), result.folder };
}

FolderRepository::FolderMutationOutcome FolderRepository::remove(const QString& folder)
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired"), QString() };

    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    const FolderMutationResult result = m_client.remove(QUrl(pairing->serverBaseUrl), auth, folder);
    if (result.error.has_value())
        return { outcomeFromNetworkError(*result.error), result.detail, QString() };

    refresh(result.parent);
    return { MailRepositoryOutcome::Success, QString(), result.folder };
}
