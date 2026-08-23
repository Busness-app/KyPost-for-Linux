#pragma once

#include "db/PendingContactChangeDao.h" // PendingContactChangeRecord -- crosses the thread hop in the plan below
#include "domain/ContactSyncReconciliation.h"
#include "models/Contact.h"
#include "net/ContactSyncClient.h" // for ContactDedupeGroup, held by value in ContactDedupeOutcome
#include "domain/DevicePairing.h"
#include "net/RelayAuth.h"         // RelayEndpoint

#include <QSet>
#include <QSqlDatabase>
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

// PairingChanged: the reply was authorised by a pairing this device no longer
// has. Nothing was written -- see PairingIdentity in DevicePairing.h. Not an
// error; the request did exactly what it was told.
//
// CacheWriteFailed: the relay's answer was fine and nothing is wrong with the
// pairing -- the local database or the cursor file refused the write, so the
// response was rolled back rather than half-applied. The pending queue
// survives and the cursor never ends up ahead of the contacts, so the next
// sync asks for the same window again. Same value, same meaning, as
// MailRepositoryOutcome::CacheWriteFailed.
enum class ContactSyncStatus {
    Success,
    NotPaired,
    Unauthorized,
    ServiceUnavailable,
    Retry,
    PairingChanged,
    CacheWriteFailed
};

struct ContactSyncOutcome
{
    ContactSyncStatus status = ContactSyncStatus::Retry;
    ContactSyncSummary summary; // meaningful only when status == Success
    QString detail;             // meaningful when status != Success

    // Temp-uid -> real-uid pairs from this sync()'s reconciliation pass,
    // for callers (e.g. a native-contact link table) that must repoint
    // references away from a temp uid that's now dead in the local cache.
    // Empty on any early-return path (NotPaired, network error, tooOld) --
    // and on a CacheWriteFailed that rolled back. The one exception is a
    // CacheWriteFailed from the cursor write, which happens after the commit:
    // see applySync().
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
    // db must be the same connection contactDao and pendingDao write
    // through: it is what every mutation here opens a transaction on, and a
    // different handle would make those transactions cover nothing while
    // still looking like they do.
    ContactSyncRepository(ContactSyncClient& client, ContactDao& contactDao,
                           PendingContactChangeDao& pendingDao, CursorStore& cursorStore,
                           PairingStore& pairingStore, QSqlDatabase& db);

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
    //
    // Both writes, or neither: an empty return means nothing was saved. The
    // caller must say so rather than showing the contact as saved -- see the
    // .cpp for why half of this is worse than none of it.
    [[nodiscard]] QString queueCreate(Contact contact);

    // contact.uid must already be a real server uid. Updates the cache
    // immediately and enqueues the wire copy as-is (uid+rev present,
    // deleted=false) for the next sync(). False means neither write landed.
    [[nodiscard]] bool queueUpdate(const Contact& contact);

    // Removes the row from the local cache immediately. Enqueues a
    // tombstone ({uid, rev, deleted=true}, every other field default) for
    // the next sync() -- UNLESS uid refers to a contact that was never
    // synced (i.e. there is a still-pending queued create for this same uid
    // and nothing else): in that case, the pending create row is deleted
    // outright instead of enqueuing a delete, since the server never saw it.
    //
    // False means the contact is still there and still queued exactly as it
    // was -- the whole thing rolls back, because a delete that removed the
    // row but lost the tombstone comes back on the next pull.
    [[nodiscard]] bool queueDelete(const QString& uid, qint64 rev);

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
        // Which pairing authorised this sync. applySync() refuses to write
        // anything if the device has been re-paired since -- the contacts
        // table has no subscriber column, and the tooOld branch below
        // DELETES every contact, which on a stale reply would destroy the
        // new account's own contacts rather than the previous account's.
        PairingIdentity identity;
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
    QSqlDatabase& m_db;
};
