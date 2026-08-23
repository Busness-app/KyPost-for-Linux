#include "domain/ContactSyncRepository.h"

#include "db/ContactDao.h"
#include "db/PendingContactChangeDao.h"
#include "domain/ContactSyncReconciliation.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/ContactSyncClient.h"
#include "net/NetworkError.h"
#include "net/RelayAuth.h"
#include "stores/CursorStore.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QUrl>
#include <QUuid>
#include <type_traits>

namespace {

QString serializeContact(const Contact& contact)
{
    return QString::fromUtf8(QJsonDocument(ContactWire::contactToJson(contact)).toJson(QJsonDocument::Compact));
}

Contact deserializeContact(const QString& json)
{
    return ContactWire::contactFromJson(QJsonDocument::fromJson(json.toUtf8()).object());
}

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

// A partial delta response (e.g. only fn+phones present) must not null out
// fields the server didn't include -- every std::optional<QString> field
// (including the extended-contact-fields and self/dedupe additions) takes
// the response's value if present, else falls back to the existing cached
// value. Every QVector<T> field (emails/phones/addresses/groupIds/ims/
// websites/relations/events/customFields/mergedUIDs) can't distinguish
// "server omitted this field because unchanged" from "server explicitly
// cleared it to empty" -- the wire's omitempty convention produces the same
// empty JSON array either way -- so they take the response's value only
// when non-empty, else fall back to the existing cached value too;
// preserving existing data on any ambiguous empty response is the safer
// failure mode of the two. uid/rev/isSelf/deleted are taken directly from
// the response (never merged) -- plain bools have no "omitted" state to
// disambiguate, and uid/rev are always authoritative from the server.
Contact mergeContact(const Contact& c, const std::optional<Contact>& existing)
{
    // member is a pointer-to-member (e.g. &Contact::fn) so the same
    // "response value if present, else fall back to cached" rule from the
    // comment above runs once per field instead of being copy-pasted.
    auto mergeOpt = [&c, &existing](auto member) {
        return (c.*member) ? (c.*member) : (existing ? (*existing).*member : std::nullopt);
    };
    auto mergeVec = [&c, &existing](auto member) {
        using VecT = std::decay_t<decltype(c.*member)>;
        return !(c.*member).isEmpty() ? (c.*member) : (existing ? (*existing).*member : VecT{});
    };

    Contact merged;
    merged.uid = c.uid;
    merged.rev = c.rev;
    merged.createdAt = mergeOpt(&Contact::createdAt);
    merged.updatedAt = mergeOpt(&Contact::updatedAt);
    merged.fn = mergeOpt(&Contact::fn);
    merged.givenName = mergeOpt(&Contact::givenName);
    merged.familyName = mergeOpt(&Contact::familyName);
    merged.middleName = mergeOpt(&Contact::middleName);
    merged.prefix = mergeOpt(&Contact::prefix);
    merged.suffix = mergeOpt(&Contact::suffix);
    merged.nickname = mergeOpt(&Contact::nickname);
    merged.org = mergeOpt(&Contact::org);
    merged.title = mergeOpt(&Contact::title);
    merged.notes = mergeOpt(&Contact::notes);
    merged.birthday = mergeOpt(&Contact::birthday);
    merged.emails = mergeVec(&Contact::emails);
    merged.phones = mergeVec(&Contact::phones);
    merged.addresses = mergeVec(&Contact::addresses);
    merged.groupIds = mergeVec(&Contact::groupIds);
    merged.photoRef = mergeOpt(&Contact::photoRef);
    merged.pgpKey = mergeOpt(&Contact::pgpKey);
    merged.ims = mergeVec(&Contact::ims);
    merged.websites = mergeVec(&Contact::websites);
    merged.relations = mergeVec(&Contact::relations);
    merged.events = mergeVec(&Contact::events);
    merged.phoneticGivenName = mergeOpt(&Contact::phoneticGivenName);
    merged.phoneticFamilyName = mergeOpt(&Contact::phoneticFamilyName);
    merged.department = mergeOpt(&Contact::department);
    merged.customFields = mergeVec(&Contact::customFields);
    merged.pronouns = mergeOpt(&Contact::pronouns);
    // isSelf/deleted are plain bools with no way to distinguish "server
    // omitted this" from "server explicitly cleared it" -- like deleted
    // below, the response's value is authoritative and taken directly.
    merged.isSelf = c.isSelf;
    merged.mergedUIDs = mergeVec(&Contact::mergedUIDs);
    merged.mergedInto = mergeOpt(&Contact::mergedInto);
    merged.deleted = c.deleted;
    return merged;
}

// One user action, one transaction. Every mutation below is at least two
// DAO calls -- a cached contact and its pending-queue row -- and both DAOs
// write through the same connection, so a transaction is what makes them one
// thing. Half of one of these has a name and a bug report: a contact that
// exists locally and can never sync, or a delete whose tombstone is gone and
// which therefore comes back on the next pull.
template <typename Fn>
bool inTransaction(QSqlDatabase& db, Fn&& body)
{
    if (!db.transaction())
        return false;
    if (!body()) {
        db.rollback();
        return false;
    }
    return db.commit();
}

ContactSyncStatus statusFromNetworkError(NetworkError error)
{
    switch (error) {
    case NetworkError::Unauthorized:
        return ContactSyncStatus::Unauthorized;
    case NetworkError::ServiceUnavailable:
        return ContactSyncStatus::ServiceUnavailable;
    default:
        return ContactSyncStatus::Retry;
    }
}

// Small, deliberately separate switch rather than sharing statusFromNetworkError
// above -- ContactDedupeStatus and ContactSyncStatus are distinct enums, not
// worth templatizing across for two five-line switches.
ContactDedupeStatus dedupeStatusFromNetworkError(NetworkError error)
{
    switch (error) {
    case NetworkError::Unauthorized:
        return ContactDedupeStatus::Unauthorized;
    case NetworkError::ServiceUnavailable:
        return ContactDedupeStatus::ServiceUnavailable;
    default:
        return ContactDedupeStatus::Retry;
    }
}

} // namespace

ContactSyncRepository::ContactSyncRepository(ContactSyncClient& client, ContactDao& contactDao,
                                               PendingContactChangeDao& pendingDao, CursorStore& cursorStore,
                                               PairingStore& pairingStore, QSqlDatabase& db)
    : m_client(client)
    , m_contactDao(contactDao)
    , m_pendingDao(pendingDao)
    , m_cursorStore(cursorStore)
    , m_pairingStore(pairingStore)
    , m_db(db)
{
}

QVector<Contact> ContactSyncRepository::contacts() const
{
    return m_contactDao.findAll();
}

std::optional<Contact> ContactSyncRepository::findByUid(const QString& uid) const
{
    return m_contactDao.findById(uid);
}

QSet<QString> ContactSyncRepository::pendingUids() const
{
    QSet<QString> uids;
    for (const PendingContactChangeRecord& record : m_pendingDao.findAll())
        uids.insert(record.contactUid);
    return uids;
}

bool ContactSyncRepository::isPending(const QString& uid) const
{
    return pendingUids().contains(uid);
}

QString ContactSyncRepository::queueCreate(Contact contact)
{
    const QString tempUid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    contact.uid = tempUid;
    contact.deleted = false;

    Contact wireCopy = contact;
    wireCopy.uid = QString(); // empty uid marks a create, per ContactSyncClient's contract

    // The cached row without the queue row is a contact the user can see,
    // edit and rely on that will never reach the server or any other device.
    const bool queued = inTransaction(m_db, [&] {
        return m_contactDao.insertOrReplace(contact)
            && m_pendingDao.enqueue(tempUid, serializeContact(wireCopy), nowUtc()) > 0;
    });

    return queued ? tempUid : QString();
}

bool ContactSyncRepository::queueUpdate(const Contact& contact)
{
    // Same pair, same reason as queueCreate() -- with one extra way to be
    // wrong: the cache write can land while the queue write does not, and
    // then the local copy shows the user's edit while the server keeps the
    // old one, forever, with no pending badge to say so.
    return inTransaction(m_db, [&] {
        return m_contactDao.insertOrReplace(contact)
            && m_pendingDao.enqueue(contact.uid, serializeContact(contact), nowUtc()) > 0;
    });
}

bool ContactSyncRepository::queueDelete(const QString& uid, qint64 rev)
{
    // If the only pending entry for this uid is a still-unflushed create,
    // the server never saw this contact -- cancel the create outright
    // rather than pushing a tombstone for something that doesn't exist
    // server-side.
    //
    // EVERY queued change for this uid is dropped, not just the create. An
    // edit saved after the create carries the temporary uid in its wire
    // payload, so it survived this loop and was pushed on the next sync as a
    // change naming a uid the server had never seen -- which the relay treats
    // as a create under that uid. The contact the user deleted was recreated
    // account-wide and echoed straight back into the local database.
    //
    // All of it in one transaction. Dropping the queued changes and then
    // failing to enqueue the tombstone is the resurrected-contact bug this
    // method's comment describes, arrived at from the other direction: the
    // row is gone locally, nothing tells the server, and the next pull hands
    // it straight back.
    return inTransaction(m_db, [&] {
        bool hadUnsyncedCreate = false;
        for (const PendingContactChangeRecord& record : m_pendingDao.findAll()) {
            if (record.contactUid != uid)
                continue;
            if (deserializeContact(record.changeJson).uid.isEmpty())
                hadUnsyncedCreate = true;
            if (!m_pendingDao.deleteById(record.id))
                return false;
        }

        if (!m_contactDao.deleteById(uid))
            return false;

        if (hadUnsyncedCreate)
            return true;

        Contact tombstone;
        tombstone.uid = uid;
        tombstone.rev = rev;
        tombstone.deleted = true;
        return m_pendingDao.enqueue(uid, serializeContact(tombstone), nowUtc()) > 0;
    });
}

std::optional<ContactSyncRepository::ContactSyncPlan> ContactSyncRepository::planSync() const
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return std::nullopt;

    const QString storedCursor = m_cursorStore.contactBaseCursor();

    ContactSyncPlan plan;
    plan.endpoint = RelayEndpoint{ QUrl(pairing->serverBaseUrl),
                                    RelayAuth{ pairing->deviceId, pairing->deviceSecret } };
    plan.cursor = storedCursor.isEmpty() ? 0 : storedCursor.toLongLong();
    plan.pending = m_pendingDao.findAll();
    plan.identity = identityOf(*pairing);
    return plan;
}

ContactSyncResult ContactSyncRepository::syncWith(HttpClient& httpClient, const ContactSyncPlan& plan)
{
    // Constructed here rather than reused from m_client: ContactSyncClient is
    // a stateless wrapper over an HttpClient reference, and on the async path
    // that HttpClient belongs to the executor thread.
    ContactSyncClient client(httpClient);
    if (plan.pending.isEmpty())
        return client.pull(plan.endpoint.serverBaseUrl, plan.endpoint.auth, plan.cursor);

    QVector<Contact> changes;
    changes.reserve(plan.pending.size());
    for (const PendingContactChangeRecord& record : plan.pending)
        changes.append(deserializeContact(record.changeJson));
    return client.push(plan.endpoint.serverBaseUrl, plan.endpoint.auth, plan.cursor, changes);
}

ContactSyncOutcome ContactSyncRepository::applySync(const ContactSyncPlan& plan, const ContactSyncResult& result)
{
    const QVector<PendingContactChangeRecord>& pending = plan.pending;

    // First, before either branch below writes anything. The tooOld branch in
    // particular deletes every contact and clears the cursor: run for a reply
    // that belongs to a pairing this device no longer has, it would wipe the
    // NEW account's contacts on the previous account's say-so.
    //
    // The pending queue is deliberately left alone here too. Those are local
    // edits that have not been accepted anywhere yet; dropping them because
    // the account changed would silently discard the user's own work.
    if (!m_pairingStore.stillCurrent(plan.identity))
        return { ContactSyncStatus::PairingChanged, {}, QString(), {} };

    // An unpushed queue must survive to the next sync() call -- pending is
    // deliberately left untouched here.
    if (result.error.has_value())
        return { statusFromNetworkError(*result.error), {}, result.detail, {} };

    if (result.tooOld) {
        // setContactBaseCursor(QString()) directly rather than
        // CursorStore::reset(), which also clears the unrelated mail
        // cursor -- a contacts-only tooOld response has nothing to do with
        // mail sync.
        //
        // Cursor first here; database first on the success path below. Both
        // orders serve one invariant: whatever fails, the cursor that
        // survives must never be AHEAD of the contacts that survive. Ahead
        // means the next pull asks for a delta against rows this device does
        // not have, and the relay never mentions the missing ones again --
        // silent, permanent, and indistinguishable from an empty address
        // book. Behind costs one redundant pull that re-applies rows which
        // are already there.
        //
        // So: clear the cursor, THEN wipe. A failed wipe after a cleared
        // cursor just means the next sync pulls the whole book again and
        // upserts over what is still here.
        if (!m_cursorStore.setContactBaseCursor(QString()))
            return { ContactSyncStatus::CacheWriteFailed, {}, QString(), {} };

        const bool wiped = inTransaction(m_db, [&] {
            if (!m_contactDao.deleteAll())
                return false;
            // Same reasoning as the success path below: only the snapshot
            // that was pushed may be dropped.
            for (const PendingContactChangeRecord& record : pending) {
                if (!m_pendingDao.deleteById(record.id))
                    return false;
            }
            return true;
        });
        if (!wiped)
            return { ContactSyncStatus::CacheWriteFailed, {}, QString(), {} };

        return { ContactSyncStatus::Success,
                 ContactSyncSummary{ static_cast<int>(pending.size()), 0, 0 },
                 QString(),
                 {} };
    }

    QVector<Contact> pendingCreates;
    for (const PendingContactChangeRecord& record : pending) {
        const Contact wire = deserializeContact(record.changeJson);
        if (!wire.uid.isEmpty())
            continue;
        Contact create;
        create.uid = record.contactUid;
        create.fn = wire.fn;
        create.emails = wire.emails;
        pendingCreates.append(create);
    }

    const QVector<ContactReconciliationAssignment> assignments =
        ContactSyncReconciliation::reconcile(pendingCreates, result.changed);

    // The response and the queue it answers are applied as ONE database
    // transaction, for the same reason the cursor exists at all: every row
    // in it is described exactly once, and a half-applied response is a
    // state no cursor value can describe. Dropping the pushed queue while
    // the changed rows failed to land is the specific version of that which
    // loses the user's own edits -- the relay considers them delivered, this
    // device no longer has them queued, and nothing will ever ask again.
    int applied = 0;
    const bool persisted = inTransaction(m_db, [&] {
        for (const ContactReconciliationAssignment& assignment : assignments) {
            if (!m_contactDao.deleteById(assignment.localUid))
                return false;
        }

        for (const Contact& c : result.changed) {
            if (c.uid.isEmpty())
                continue;
            const std::optional<Contact> existing = m_contactDao.findById(c.uid);
            if (!m_contactDao.insertOrReplace(mergeContact(c, existing)))
                return false;
            ++applied;
        }

        for (const Contact& c : result.deletedContacts) {
            if (!m_contactDao.deleteById(c.uid))
                return false;
            ++applied;
        }

        // Delete only what was actually pushed -- by record id, never
        // deleteAll(). A Save or Delete in the contact pane can land while
        // the request is out, and truncating the whole queue discarded it:
        // the user saw "Synced" while their edit -- a replaced PGP key, say
        // -- never reached the server.
        //
        // That used to happen because HttpClient blocked on a nested
        // QEventLoop and QML kept delivering clicks. The request runs on the
        // executor thread now, so the GUI thread is simply free for the whole
        // round trip and a mid-sync edit is the ORDINARY case rather than a
        // race. Same rule, more load-bearing than before.
        for (const PendingContactChangeRecord& record : pending) {
            if (!m_pendingDao.deleteById(record.id))
                return false;
        }
        return true;
    });

    if (!persisted)
        return { ContactSyncStatus::CacheWriteFailed, {}, QString(), {} };

    // Database first, cursor second -- the opposite order to the tooOld
    // branch above, and the same invariant (see there). A committed batch
    // with a cursor that did not persist costs one redundant pull whose rows
    // upsert over themselves; a persisted cursor over a batch that did not
    // commit costs the batch, permanently.
    //
    // The reassignments ride along even on this failure path, unlike every
    // other one: the transaction above committed, so those temp uids really
    // are dead in the local cache, and the pending creates that produced them
    // are gone -- no later sync can derive this mapping again.
    if (!m_cursorStore.setContactBaseCursor(QString::number(result.cursor)))
        return { ContactSyncStatus::CacheWriteFailed, {}, QString(), assignments };

    return { ContactSyncStatus::Success,
             ContactSyncSummary{ static_cast<int>(pending.size()), applied, result.cursor },
             QString(),
             assignments };
}

ContactDedupeResult ContactSyncRepository::dedupeWith(HttpClient& httpClient, const RelayEndpoint& endpoint)
{
    ContactSyncClient client(httpClient);
    return client.dedupe(endpoint.serverBaseUrl, endpoint.auth);
}

ContactDedupeOutcome ContactSyncRepository::dedupeOutcomeOf(const ContactDedupeResult& result)
{
    if (result.error.has_value())
        return { dedupeStatusFromNetworkError(*result.error), 0, {}, result.detail };
    return { ContactDedupeStatus::Success, result.mergedCount, result.groups, QString() };
}

// The synchronous forms, kept as compositions of the phases above so the
// async paths cannot drift from the behaviour their tests pin.

ContactSyncOutcome ContactSyncRepository::sync()
{
    const std::optional<ContactSyncPlan> plan = planSync();
    if (!plan.has_value())
        return { ContactSyncStatus::NotPaired, {}, QStringLiteral("Not paired"), {} };

    ContactSyncResult result;
    if (plan->pending.isEmpty()) {
        result = m_client.pull(plan->endpoint.serverBaseUrl, plan->endpoint.auth, plan->cursor);
    } else {
        QVector<Contact> changes;
        changes.reserve(plan->pending.size());
        for (const PendingContactChangeRecord& record : plan->pending)
            changes.append(deserializeContact(record.changeJson));
        result = m_client.push(plan->endpoint.serverBaseUrl, plan->endpoint.auth, plan->cursor, changes);
    }
    return applySync(*plan, result);
}

ContactDedupeOutcome ContactSyncRepository::dedupe()
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return { ContactDedupeStatus::NotPaired, 0, {}, QStringLiteral("Not paired") };

    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    const QUrl serverUrl(pairing->serverBaseUrl);
    return dedupeOutcomeOf(m_client.dedupe(serverUrl, auth));
}
