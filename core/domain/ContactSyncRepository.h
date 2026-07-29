#pragma once

#include "db/PendingContactChangeDao.h" // PendingContactChangeRecord -- crosses the thread hop in the plan below
#include "domain/ContactSyncReconciliation.h"
#include "models/Contact.h"
#include "net/ContactSyncClient.h" // for ContactDedupeGroup, held by value in ContactDedupeOutcome
#include "net/RelayAuth.h"         // RelayEndpoint

#include <QSet>
#include <QString>
#include <QVector>
#include <optional>

class ContactDao;
class HttpClient;
class CursorStore;
class PairingStore;

struct ContactSyncSummary
{
    int pushed = 0;
    int applied = 0;
    qint64 newCursor = 0;

    bool operator==(const ContactSyncSummary&) const = default;
};

enum class ContactSyncStatus { Success, NotPaired, Unauthorized, ServiceUnavailable, Retry };

struct ContactSyncOutcome
{
    ContactSyncStatus status = ContactSyncStatus::Retry;
    ContactSyncSummary summary; // meaningful only when status == Success
    QString detail;             // meaningful when status != Success

    // Temp-uid -> real-uid pairs from this sync()'s reconciliation pass,
    // for callers (e.g. a native-contact link table) that must repoint
    // references away from a temp uid that's now dead in the local cache.
    // Empty on any early-return path (NotPaired, network error, tooOld).
    QVector<ContactReconciliationAssignment> uidReassignments;
};

enum class ContactDedupeStatus { Success, NotPaired, Unauthorized, ServiceUnavailable, Retry };

struct ContactDedupeOutcome
{
    ContactDedupeStatus status = ContactDedupeStatus::Retry;
    int mergedCount = 0;                  // meaningful only when status == Success
    QVector<ContactDedupeGroup> groups;    // meaningful only when status == Success
    QString detail;                        // meaningful when status != Success
};

// Sits between ContactSyncClient and ContactDao/PendingContactChangeDao,
// matching the Domain/Repositories layer in kypost-for-Mac
// (ContactSyncRepository.swift) -- see ContactSyncReconciliation.h for the
// uid-assignment half of sync(). queueCreate/queueUpdate/queueDelete apply
// to the local cache immediately and enqueue the corresponding wire change
// for the next sync() call; sync() itself either pushes the queue (if
// non-empty) or pulls (if empty), then merges the response back into the
// cache -- see sync()'s .cpp comments for the partial-delta merge rule.
class ContactSyncRepository
{
public:
    ContactSyncRepository(ContactSyncClient& client, ContactDao& contactDao,
                           PendingContactChangeDao& pendingDao, CursorStore& cursorStore,
                           PairingStore& pairingStore);

    QVector<Contact> contacts() const; // contactDao.findAll()

    std::optional<Contact> findByUid(const QString& uid) const; // contactDao.findById()

    // uids with at least one row in pendingDao -- i.e. queued for the next
    // sync() and not yet round-tripped through a successful push/pull. This
    // is the real synced/pending ground truth (replaces the old
    // rev!=0-on-Contact heuristic ContactListModel/ContactDetail.qml used to
    // duplicate independently -- see queueDelete()'s own scan below for the
    // pattern this reuses).
    QSet<QString> pendingUids() const;

    // Convenience single-uid form of pendingUids(); see its doc comment.
    bool isPending(const QString& uid) const;

    // Assigns a temp local uid (QUuid::createUuid().toString(QUuid::
    // WithoutBraces)), caches it under that uid immediately, and enqueues a
    // create (wire copy has uid=="", per ContactSyncClient's documented
    // "empty uid marks a create" contract) for the next sync().
    QString queueCreate(Contact contact);

    // contact.uid must already be a real server uid. Updates the cache
    // immediately and enqueues the wire copy as-is (uid+rev present,
    // deleted=false) for the next sync().
    void queueUpdate(const Contact& contact);

    // Removes the row from the local cache immediately. Enqueues a
    // tombstone ({uid, rev, deleted=true}, every other field default) for
    // the next sync() -- UNLESS uid refers to a contact that was never
    // synced (i.e. there is a still-pending queued create for this same uid
    // and nothing else): in that case, the pending create row is deleted
    // outright instead of enqueuing a delete, since the server never saw it.
    void queueDelete(const QString& uid, qint64 rev);

    ContactSyncOutcome sync();

    // ---- three-phase form, for callers that must not block ---------------
    //
    // The largest split in the migration: this chain touches PairingStore,
    // CursorStore, ContactDao and PendingContactChangeDao, all confined to
    // the calling thread. Only the one push-or-pull request may move.
    //
    // The pending queue is READ in phase 1 and carried across, deliberately.
    // Phase 3 then deletes exactly those records by id -- not the whole queue
    // -- so a contact edited while the request was out survives to the next
    // sync. That rule already existed (a nested event loop let clicks land
    // mid-sync); moving the request off-thread does not weaken it, it makes
    // it the normal case rather than the racy one.

    // Everything the request needs, read before it and carried by value.
    struct ContactSyncPlan
    {
        RelayEndpoint endpoint;
        qint64 cursor = 0;
        QVector<PendingContactChangeRecord> pending;
    };

    // Phase 1, on the calling thread. nullopt when there is no pairing.
    std::optional<ContactSyncPlan> planSync() const;
    // Phase 2. Push when the queue is non-empty, pull otherwise.
    static ContactSyncResult syncWith(HttpClient& httpClient, const ContactSyncPlan& plan);
    // Phase 3, back on the calling thread: reconciliation and every DAO and
    // cursor write.
    ContactSyncOutcome applySync(const ContactSyncPlan& plan, const ContactSyncResult& result);

    // dedupe() needs no phase 3 -- it writes nothing locally; the caller
    // chains into a sync to pull the consequences.
    static ContactDedupeResult dedupeWith(HttpClient& httpClient, const RelayEndpoint& endpoint);
    static ContactDedupeOutcome dedupeOutcomeOf(const ContactDedupeResult& result);

    // Single-purpose, like sync() -- one HTTP action, does not call sync()
    // itself. The local cache (ContactDao/PendingContactChangeDao) is
    // untouched by this call; callers that want the resulting tombstones/
    // survivor updates reflected locally must call sync() separately.
    ContactDedupeOutcome dedupe();

private:
    ContactSyncClient& m_client;
    ContactDao& m_contactDao;
    PendingContactChangeDao& m_pendingDao;
    CursorStore& m_cursorStore;
    PairingStore& m_pairingStore;
};
