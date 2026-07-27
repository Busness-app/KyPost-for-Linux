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
                                               PairingStore& pairingStore)
    : m_client(client)
    , m_contactDao(contactDao)
    , m_pendingDao(pendingDao)
    , m_cursorStore(cursorStore)
    , m_pairingStore(pairingStore)
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
    m_contactDao.insertOrReplace(contact);

    Contact wireCopy = contact;
    wireCopy.uid = QString(); // empty uid marks a create, per ContactSyncClient's contract
    m_pendingDao.enqueue(tempUid, serializeContact(wireCopy), nowUtc());

    return tempUid;
}

void ContactSyncRepository::queueUpdate(const Contact& contact)
{
    m_contactDao.insertOrReplace(contact);
    m_pendingDao.enqueue(contact.uid, serializeContact(contact), nowUtc());
}

void ContactSyncRepository::queueDelete(const QString& uid, qint64 rev)
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
    bool hadUnsyncedCreate = false;
    for (const PendingContactChangeRecord& record : m_pendingDao.findAll()) {
        if (record.contactUid != uid)
            continue;
        if (deserializeContact(record.changeJson).uid.isEmpty())
            hadUnsyncedCreate = true;
        m_pendingDao.deleteById(record.id);
    }

    m_contactDao.deleteById(uid);

    if (hadUnsyncedCreate)
        return;

    Contact tombstone;
    tombstone.uid = uid;
    tombstone.rev = rev;
    tombstone.deleted = true;
    m_pendingDao.enqueue(uid, serializeContact(tombstone), nowUtc());
}

ContactSyncOutcome ContactSyncRepository::sync()
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return { ContactSyncStatus::NotPaired, {}, QStringLiteral("Not paired") };

    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    const QUrl serverUrl(pairing->serverBaseUrl);
    const QString storedCursor = m_cursorStore.contactBaseCursor();
    const qint64 cursor = storedCursor.isEmpty() ? 0 : storedCursor.toLongLong();

    const QVector<PendingContactChangeRecord> pending = m_pendingDao.findAll();

    ContactSyncResult result;
    if (pending.isEmpty()) {
        result = m_client.pull(serverUrl, auth, cursor);
    } else {
        QVector<Contact> changes;
        changes.reserve(pending.size());
        for (const PendingContactChangeRecord& record : pending)
            changes.append(deserializeContact(record.changeJson));
        result = m_client.push(serverUrl, auth, cursor, changes);
    }

    // An unpushed queue must survive to the next sync() call -- pending is
    // deliberately left untouched here.
    if (result.error.has_value())
        return { statusFromNetworkError(*result.error), {}, result.detail };

    if (result.tooOld) {
        // setContactBaseCursor(QString()) directly rather than
        // CursorStore::reset(), which also clears the unrelated mail
        // cursor -- a contacts-only tooOld response has nothing to do with
        // mail sync.
        m_cursorStore.setContactBaseCursor(QString());
        m_contactDao.deleteAll();
        // Same reasoning as the success path below: only the snapshot that
        // was pushed may be dropped.
        for (const PendingContactChangeRecord& record : pending)
            m_pendingDao.deleteById(record.id);
        return { ContactSyncStatus::Success,
                 ContactSyncSummary{ static_cast<int>(pending.size()), 0, 0 },
                 QString() };
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
    for (const ContactReconciliationAssignment& assignment : assignments)
        m_contactDao.deleteById(assignment.localUid);

    int applied = 0;
    for (const Contact& c : result.changed) {
        if (c.uid.isEmpty())
            continue;
        const std::optional<Contact> existing = m_contactDao.findById(c.uid);
        m_contactDao.insertOrReplace(mergeContact(c, existing));
        ++applied;
    }

    for (const Contact& c : result.deletedContacts) {
        m_contactDao.deleteById(c.uid);
        ++applied;
    }

    // Delete only what was actually pushed. deleteAll() truncated the whole
    // queue, including anything enqueued DURING the call: HttpClient blocks on
    // a nested QEventLoop, so QML keeps delivering clicks and a Save or Delete
    // in the contact pane lands mid-sync. The user saw "Synced" while their
    // edit -- a replaced PGP key, say -- was silently discarded and never
    // reached the server.
    for (const PendingContactChangeRecord& record : pending)
        m_pendingDao.deleteById(record.id);
    m_cursorStore.setContactBaseCursor(QString::number(result.cursor));

    return { ContactSyncStatus::Success,
             ContactSyncSummary{ static_cast<int>(pending.size()), applied, result.cursor },
             QString(),
             assignments };
}

ContactDedupeOutcome ContactSyncRepository::dedupe()
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return { ContactDedupeStatus::NotPaired, 0, {}, QStringLiteral("Not paired") };

    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    const QUrl serverUrl(pairing->serverBaseUrl);

    const ContactDedupeResult result = m_client.dedupe(serverUrl, auth);

    if (result.error.has_value())
        return { dedupeStatusFromNetworkError(*result.error), 0, {}, result.detail };

    return { ContactDedupeStatus::Success, result.mergedCount, result.groups, QString() };
}
