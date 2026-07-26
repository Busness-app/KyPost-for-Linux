#pragma once

#include "domain/MailRepository.h" // MailRepositoryOutcome / MailFetchOutcome
#include "models/MailFolder.h"

#include <QString>
#include <QVector>

class FolderClient;
class FolderDao;
class PairingStore;

// Sits between FolderClient and FolderDao, mirroring MailRepository's shape
// (same outcome enum, same pairing-first flow) so the two read alike.
//
// Reuses MailRepositoryOutcome rather than declaring a parallel enum: the
// failure modes are identical (not paired / unauthorized / backend
// unavailable / retry) and MailController maps that enum to user-facing copy
// in exactly one place already.
//
// Scope note: only *listing* is cached. The three mutating verbs write
// straight through to the server and then re-list, because the backend
// decides the resulting path (its own separator and parent-joining rules)
// and refuses some operations outright -- optimistically mutating the local
// cache would mean re-implementing those rules here and getting them subtly
// wrong.
class FolderRepository
{
public:
    FolderRepository(FolderClient& client, FolderDao& folderDao, PairingStore& pairingStore);

    // Fetches `parent`'s children and replaces that parent's cached rows.
    // Empty `parent` lists the top level.
    MailFetchOutcome refresh(const QString& parent);

    // Local cache read, no network call. Sorted by path so the sidebar order
    // is stable across refreshes regardless of server ordering.
    QVector<MailFolder> cachedFolders(const QString& parent) const;

    // Each writes through and refreshes the affected parent on success. The
    // resulting full path is returned in FolderMutationOutcome::folder so a
    // caller can select it.
    struct FolderMutationOutcome
    {
        MailRepositoryOutcome outcome = MailRepositoryOutcome::Retry;
        QString detail;
        QString folder; // resulting path on success
    };

    FolderMutationOutcome create(const QString& parent, const QString& name);
    FolderMutationOutcome rename(const QString& folder, const QString& name);
    FolderMutationOutcome remove(const QString& folder);

private:
    FolderClient& m_client;
    FolderDao& m_folderDao;
    PairingStore& m_pairingStore;
};
