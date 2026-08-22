#pragma once

#include "domain/MailRepository.h" // MailRepositoryOutcome / MailFetchOutcome / MailRelayCredentials
#include "domain/RelayRequestPlan.h"
#include "models/MailFolder.h"
#include "net/FolderClient.h" // FolderListResult / FolderMutationResult -- cross the thread hop by value

#include <QString>
#include <QVector>

class FolderDao;
class HttpClient;
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

    // ---- three-phase form, for callers that must not block ---------------
    //
    // Same split as MailRepository's, with one wrinkle: each mutating verb is
    // TWO round trips, because the backend decides the resulting path and a
    // deleted folder is only observable as an absence from a fresh listing.
    // Both trips go in phase 2 together -- splitting them would put a second
    // thread hop between a mutation and the re-list that makes it visible,
    // for no benefit.

    // Phase 1, on the calling thread: the PairingStore read. Returns nullopt
    // when there is no pairing.
    std::optional<RelayRequestPlan> planRequest() const;

    // What one mutating verb's two requests produced. Crosses the thread hop
    // by value.
    struct FolderMutationFetch
    {
        FolderMutationResult mutation;
        // Which parent `listing` describes -- the server's own answer, not
        // derived here: a rename keeps the folder under its existing parent,
        // and working that out locally would mean re-implementing the
        // server's separator rules.
        QString listedParent;
        FolderListResult listing; // only meaningful when the mutation succeeded
    };

    // Phase 2. Touch nothing but the HttpClient and the plain arguments.
    static FolderListResult listWith(HttpClient& httpClient, const RelayEndpoint& endpoint, const QString& parent);
    static FolderMutationFetch createWith(HttpClient& httpClient, const RelayEndpoint& endpoint,
                                           const QString& parent, const QString& name);
    static FolderMutationFetch renameWith(HttpClient& httpClient, const RelayEndpoint& endpoint,
                                           const QString& folder, const QString& name);
    static FolderMutationFetch removeWith(HttpClient& httpClient, const RelayEndpoint& endpoint,
                                           const QString& folder);

    // Phase 3, back on the calling thread: the FolderDao writes.
    //
    // Both take the plan, and not only for the endpoint: they refuse to write
    // when the device has been re-paired since the request went out. The
    // folders table has no subscriber column, so the previous account's
    // mailbox names -- which are user data, and often revealing ones -- would
    // otherwise be listed in the new account's sidebar. Both report that as
    // MailRepositoryOutcome::PairingChanged.
    MailFetchOutcome applyList(const RelayRequestPlan& plan, const QString& parent,
                                const FolderListResult& result);
    FolderMutationOutcome applyMutation(const RelayRequestPlan& plan, const FolderMutationFetch& fetched);

private:
    FolderClient& m_client;
    FolderDao& m_folderDao;
    PairingStore& m_pairingStore;
};
