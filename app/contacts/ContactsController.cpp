#include "contacts/ContactsController.h"

#include "contacts/ContactFieldMapping.h"
#include "domain/ContactPhotoRepository.h"
#include "domain/ContactSyncRepository.h"
#include "domain/GroupsRepository.h"
#include "models/Contact.h"
#include "models/Group.h"
#include "util/ReentrancyGuard.h"

#include <KLocalizedString>

#include <QUrl>
#include <QVariantList>
#include <algorithm>

namespace {

// Blank string -> std::nullopt, matching Contact's std::optional<QString>
// field convention (org/notes) rather than storing an empty-but-present
// string.
std::optional<QString> toOptional(const QString& value)
{
    return value.isEmpty() ? std::nullopt : std::make_optional(value);
}

ContactEmailEntry emailEntryFromMap(const QVariantMap& map)
{
    ContactEmailEntry entry;
    entry.label = toOptional(map.value(QStringLiteral("label")).toString());
    entry.value = map.value(QStringLiteral("value")).toString();
    return entry;
}

ContactPhoneEntry phoneEntryFromMap(const QVariantMap& map)
{
    ContactPhoneEntry entry;
    entry.label = toOptional(map.value(QStringLiteral("label")).toString());
    entry.value = map.value(QStringLiteral("value")).toString();
    return entry;
}

ContactAddressEntry addressEntryFromMap(const QVariantMap& map)
{
    ContactAddressEntry entry;
    entry.label = toOptional(map.value(QStringLiteral("label")).toString());
    entry.street = toOptional(map.value(QStringLiteral("street")).toString());
    entry.city = toOptional(map.value(QStringLiteral("city")).toString());
    entry.region = toOptional(map.value(QStringLiteral("region")).toString());
    entry.postalCode = toOptional(map.value(QStringLiteral("postalCode")).toString());
    entry.country = toOptional(map.value(QStringLiteral("country")).toString());
    return entry;
}

ContactImEntry imEntryFromMap(const QVariantMap& map)
{
    ContactImEntry entry;
    entry.service = toOptional(map.value(QStringLiteral("service")).toString());
    entry.label = toOptional(map.value(QStringLiteral("label")).toString());
    entry.value = map.value(QStringLiteral("value")).toString();
    return entry;
}

ContactUrlEntry urlEntryFromMap(const QVariantMap& map)
{
    ContactUrlEntry entry;
    entry.label = toOptional(map.value(QStringLiteral("label")).toString());
    entry.value = map.value(QStringLiteral("value")).toString();
    return entry;
}

ContactRelationEntry relationEntryFromMap(const QVariantMap& map)
{
    ContactRelationEntry entry;
    entry.label = toOptional(map.value(QStringLiteral("label")).toString());
    entry.name = map.value(QStringLiteral("name")).toString();
    return entry;
}

ContactEventEntry eventEntryFromMap(const QVariantMap& map)
{
    ContactEventEntry entry;
    entry.label = toOptional(map.value(QStringLiteral("label")).toString());
    entry.date = map.value(QStringLiteral("date")).toString();
    return entry;
}

ContactCustomFieldEntry customFieldEntryFromMap(const QVariantMap& map)
{
    ContactCustomFieldEntry entry;
    entry.label = map.value(QStringLiteral("label")).toString();
    entry.value = map.value(QStringLiteral("value")).toString();
    return entry;
}

// entriesToVariantList comes from ContactFieldMapping.h (shared with
// PgpQrController). entriesFromVariantList has no other caller, so it stays
// local -- mirrors ContactDao.cpp's entriesToJson/entriesFromJson template
// pattern, just targeting QVariant instead of QJsonValue.
template <typename T, typename FromMapFn>
QVector<T> entriesFromVariantList(const QVariantList& list, FromMapFn fromMap)
{
    QVector<T> entries;
    entries.reserve(list.size());
    for (const QVariant& value : list)
        entries.append(fromMap(value.toMap()));
    return entries;
}

// groupIds is a plain QVector<QString>, not a struct-entry list -- own
// conversion pair rather than going through entriesToVariantList/FromVariantList.
QVariantList stringListToVariantList(const QVector<QString>& values)
{
    QVariantList list;
    list.reserve(values.size());
    for (const QString& value : values)
        list.append(value);
    return list;
}

QVector<QString> stringListFromVariantList(const QVariantList& list)
{
    QVector<QString> values;
    values.reserve(list.size());
    for (const QVariant& value : list)
        values.append(value.toString());
    return values;
}

std::optional<Contact> findByUid(const QVector<Contact>& contacts, const QString& uid)
{
    const auto it = std::find_if(contacts.begin(), contacts.end(),
                                  [&uid](const Contact& c) { return c.uid == uid; });
    if (it == contacts.end())
        return std::nullopt;
    return *it;
}

} // namespace

ContactsController::ContactsController(ContactSyncRepository& repository, GroupsRepository& groupsRepository,
                                         ContactPhotoRepository& photoRepository, QObject* parent)
    : QObject(parent)
    , m_repository(repository)
    , m_groupsRepository(groupsRepository)
    , m_photoRepository(photoRepository)
    , m_model(new ContactListModel(this))
{
    // Deliberately does NOT call load() here -- matches MailController's
    // existing convention (its model starts empty until QML calls
    // selectFolder()) rather than introducing a second, inconsistent
    // eager-populate-on-construction pattern. QML is expected to call
    // load() itself (e.g. from Component.onCompleted), same as it already
    // does for MailApp.
}

QObject* ContactsController::contactModel() const
{
    return m_model;
}

bool ContactsController::isBusy() const
{
    return m_isBusy;
}

QString ContactsController::lastError() const
{
    return m_lastError;
}

QString ContactsController::statusMessage() const
{
    return m_statusMessage;
}

void ContactsController::setBusy(bool busy)
{
    if (m_isBusy == busy)
        return;
    m_isBusy = busy;
    emit isBusyChanged();
}

void ContactsController::setLastError(const QString& error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

void ContactsController::setStatusMessage(const QString& message)
{
    if (m_statusMessage == message)
        return;
    m_statusMessage = message;
    emit statusMessageChanged();
}

void ContactsController::load()
{
    // Read-only display sort: the self-contact (if any) always renders
    // first, per Contact::isSelf's doc comment -- this client never sets
    // the flag itself, only reads whatever the server already sent over
    // sync. std::stable_partition keeps every other contact's relative
    // order unchanged.
    QVector<Contact> contacts = m_repository.contacts();
    std::stable_partition(contacts.begin(), contacts.end(), [](const Contact& c) { return c.isSelf; });
    m_model->setContacts(contacts, m_repository.pendingUids());
}

bool ContactsController::isSynced(const QString& uid)
{
    if (!m_repository.findByUid(uid).has_value())
        return false;
    return !m_repository.isPending(uid);
}

// Guarded public entry point; dedupe() below reaches the body through
// syncInternal() instead, because it legitimately chains into a sync and the
// shared guard would otherwise turn that chained call into a no-op. Same
// split, for the same reason, as MailController's *Internal methods.
void ContactsController::sync()
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;
    syncInternal();
}

void ContactsController::syncInternal()
{
    setBusy(true);
    const ContactSyncOutcome outcome = m_repository.sync();
    setBusy(false);

    // Mirrors Android's ContactSyncOutcome toast mapping in
    // ContactsListActivity: one short user-facing string per
    // ContactSyncStatus value.
    switch (outcome.status) {
    case ContactSyncStatus::Success:
        setLastError(QString());
        setStatusMessage(i18n("Synced -- %1 pushed, %2 applied", outcome.summary.pushed, outcome.summary.applied));
        // Task 2: refresh the groups name-cache once per successful contact
        // sync cycle -- not on NotPaired/Unauthorized/ServiceUnavailable/
        // Retry, matching the brief's "after a successful contact sync
        // pass" wording. refresh() itself degrades gracefully (no-op) on
        // any fetch error, so this never turns a successful contact sync
        // into a reported failure.
        m_groupsRepository.refresh();
        load();
        break;
    case ContactSyncStatus::NotPaired:
        setStatusMessage(QString());
        setLastError(i18n("Not paired"));
        break;
    case ContactSyncStatus::Unauthorized:
        setStatusMessage(QString());
        setLastError(i18n("Unauthorized -- please re-pair this device"));
        break;
    case ContactSyncStatus::ServiceUnavailable:
        setStatusMessage(QString());
        setLastError(outcome.detail.isEmpty() ? i18n("Service unavailable") : outcome.detail);
        break;
    case ContactSyncStatus::Retry:
        setStatusMessage(QString());
        setLastError(outcome.detail.isEmpty() ? i18n("Sync failed, try again") : outcome.detail);
        break;
    }
}

void ContactsController::dedupe()
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;

    setBusy(true);
    const ContactDedupeOutcome outcome = m_repository.dedupe();
    setBusy(false);

    switch (outcome.status) {
    case ContactDedupeStatus::Success:
        if (outcome.mergedCount > 0) {
            // sync() pulls the resulting tombstones/survivor update, reloads
            // the model, and sets its own lastError/statusMessage. Only
            // prefix its message with the merge count when it also
            // succeeded -- if the follow-up sync() itself fails, leave its
            // failure message/lastError as-is rather than mask it behind a
            // misleadingly cheerful "Merged N duplicate(s)" prefix.
            syncInternal();
            if (lastError().isEmpty())
                setStatusMessage(i18n("Merged %1 duplicate(s) -- %2", outcome.mergedCount, statusMessage()));
        } else {
            setLastError(QString());
            setStatusMessage(i18n("No duplicates found"));
            load();
        }
        break;
    case ContactDedupeStatus::NotPaired:
        setStatusMessage(QString());
        setLastError(i18n("Not paired"));
        break;
    case ContactDedupeStatus::Unauthorized:
        setStatusMessage(QString());
        setLastError(i18n("Unauthorized -- please re-pair this device"));
        break;
    case ContactDedupeStatus::ServiceUnavailable:
        setStatusMessage(QString());
        setLastError(outcome.detail.isEmpty() ? i18n("Service unavailable") : outcome.detail);
        break;
    case ContactDedupeStatus::Retry:
        setStatusMessage(QString());
        setLastError(outcome.detail.isEmpty() ? i18n("Dedupe failed, try again") : outcome.detail);
        break;
    }
}

QVariantMap ContactsController::contactAt(const QString& uid)
{
    const std::optional<Contact> found = findByUid(m_repository.contacts(), uid);
    if (!found)
        return {};
    const Contact& c = *found;

    QVariantMap map;
    map[QStringLiteral("uid")] = c.uid;
    map[QStringLiteral("rev")] = c.rev;
    map[QStringLiteral("createdAt")] = c.createdAt.value_or(QString());
    map[QStringLiteral("updatedAt")] = c.updatedAt.value_or(QString());
    map[QStringLiteral("fn")] = c.fn.value_or(QString());
    map[QStringLiteral("givenName")] = c.givenName.value_or(QString());
    map[QStringLiteral("familyName")] = c.familyName.value_or(QString());
    map[QStringLiteral("middleName")] = c.middleName.value_or(QString());
    map[QStringLiteral("prefix")] = c.prefix.value_or(QString());
    map[QStringLiteral("suffix")] = c.suffix.value_or(QString());
    map[QStringLiteral("nickname")] = c.nickname.value_or(QString());
    map[QStringLiteral("org")] = c.org.value_or(QString());
    map[QStringLiteral("title")] = c.title.value_or(QString());
    map[QStringLiteral("notes")] = c.notes.value_or(QString());
    map[QStringLiteral("birthday")] = c.birthday.value_or(QString());

    QVariantList emails;
    emails.reserve(c.emails.size());
    for (const ContactEmailEntry& entry : c.emails)
        emails.append(emailEntryToMap(entry));
    map[QStringLiteral("emails")] = emails;

    QVariantList phones;
    phones.reserve(c.phones.size());
    for (const ContactPhoneEntry& entry : c.phones)
        phones.append(phoneEntryToMap(entry));
    map[QStringLiteral("phones")] = phones;

    QVariantList addresses;
    addresses.reserve(c.addresses.size());
    for (const ContactAddressEntry& entry : c.addresses)
        addresses.append(addressEntryToMap(entry));
    map[QStringLiteral("addresses")] = addresses;

    map[QStringLiteral("groupIds")] = stringListToVariantList(c.groupIds);
    map[QStringLiteral("photoRef")] = c.photoRef.value_or(QString());
    map[QStringLiteral("pgpKey")] = c.pgpKey.value_or(QString());
    map[QStringLiteral("ims")] = entriesToVariantList(c.ims, imEntryToMap);
    map[QStringLiteral("websites")] = entriesToVariantList(c.websites, urlEntryToMap);
    map[QStringLiteral("relations")] = entriesToVariantList(c.relations, relationEntryToMap);
    map[QStringLiteral("events")] = entriesToVariantList(c.events, eventEntryToMap);
    map[QStringLiteral("phoneticGivenName")] = c.phoneticGivenName.value_or(QString());
    map[QStringLiteral("phoneticFamilyName")] = c.phoneticFamilyName.value_or(QString());
    map[QStringLiteral("department")] = c.department.value_or(QString());
    map[QStringLiteral("customFields")] = entriesToVariantList(c.customFields, customFieldEntryToMap);
    map[QStringLiteral("pronouns")] = c.pronouns.value_or(QString());
    map[QStringLiteral("isSelf")] = c.isSelf;

    map[QStringLiteral("deleted")] = c.deleted;
    return map;
}

// Shared field-population body of createContact/updateContact: reads every
// non-fn/non-identity key out of `fields` into `contact`. Every key here,
// including emails/phones/addresses, is a whole-value/whole-list replace --
// omitting a key clears it, same rule as ims/websites/relations/events/
// customFields below.
void ContactsController::applyFieldsToContact(Contact& contact, const QVariantMap& fields) const
{
    contact.org = toOptional(fields.value(QStringLiteral("org")).toString());
    contact.notes = toOptional(fields.value(QStringLiteral("notes")).toString());
    contact.emails = entriesFromVariantList<ContactEmailEntry>(
        fields.value(QStringLiteral("emails")).toList(), emailEntryFromMap);
    contact.phones = entriesFromVariantList<ContactPhoneEntry>(
        fields.value(QStringLiteral("phones")).toList(), phoneEntryFromMap);
    contact.addresses = entriesFromVariantList<ContactAddressEntry>(
        fields.value(QStringLiteral("addresses")).toList(), addressEntryFromMap);

    contact.groupIds = stringListFromVariantList(fields.value(QStringLiteral("groupIds")).toList());
    contact.photoRef = toOptional(fields.value(QStringLiteral("photoRef")).toString());
    contact.pgpKey = toOptional(fields.value(QStringLiteral("pgpKey")).toString());
    contact.ims = entriesFromVariantList<ContactImEntry>(fields.value(QStringLiteral("ims")).toList(), imEntryFromMap);
    contact.websites = entriesFromVariantList<ContactUrlEntry>(
        fields.value(QStringLiteral("websites")).toList(), urlEntryFromMap);
    contact.relations = entriesFromVariantList<ContactRelationEntry>(
        fields.value(QStringLiteral("relations")).toList(), relationEntryFromMap);
    contact.events =
        entriesFromVariantList<ContactEventEntry>(fields.value(QStringLiteral("events")).toList(), eventEntryFromMap);
    contact.phoneticGivenName = toOptional(fields.value(QStringLiteral("phoneticGivenName")).toString());
    contact.phoneticFamilyName = toOptional(fields.value(QStringLiteral("phoneticFamilyName")).toString());
    contact.department = toOptional(fields.value(QStringLiteral("department")).toString());
    contact.customFields = entriesFromVariantList<ContactCustomFieldEntry>(
        fields.value(QStringLiteral("customFields")).toList(), customFieldEntryFromMap);
    contact.pronouns = toOptional(fields.value(QStringLiteral("pronouns")).toString());
}

QString ContactsController::createContact(const QVariantMap& fields)
{
    // Guarded like every other invokable that touches the sync DAOs. These
    // three used to be unguarded, so a click landing inside sync()'s nested
    // QEventLoop enqueued a pending change that the in-flight sync then
    // discarded -- the user saw "Synced" while their edit was silently lost.
    // ContactSyncRepository no longer truncates the queue, but leaving these
    // re-entrant would still let a mutation interleave with a sync's
    // read-modify-write.
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return QString();

    const QString fn = fields.value(QStringLiteral("fn")).toString().trimmed();
    if (fn.isEmpty()) {
        setLastError(i18n("Name is required"));
        return QString();
    }

    Contact contact;
    contact.fn = fn;
    applyFieldsToContact(contact, fields);

    const QString newUid = m_repository.queueCreate(contact);
    setLastError(QString());
    load();
    return newUid;
}

bool ContactsController::updateContact(const QString& uid, const QVariantMap& fields)
{
    // Guarded like every other invokable that touches the sync DAOs. These
    // three used to be unguarded, so a click landing inside sync()'s nested
    // QEventLoop enqueued a pending change that the in-flight sync then
    // discarded -- the user saw "Synced" while their edit was silently lost.
    // ContactSyncRepository no longer truncates the queue, but leaving these
    // re-entrant would still let a mutation interleave with a sync's
    // read-modify-write.
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    const QString fn = fields.value(QStringLiteral("fn")).toString().trimmed();
    if (fn.isEmpty()) {
        setLastError(i18n("Name is required"));
        return false;
    }

    const std::optional<Contact> found = findByUid(m_repository.contacts(), uid);
    if (!found) {
        setLastError(i18n("Contact not found"));
        return false;
    }

    Contact contact = *found;
    contact.fn = fn;
    applyFieldsToContact(contact, fields);

    m_repository.queueUpdate(contact);
    setLastError(QString());
    load();
    return true;
}

bool ContactsController::deleteContact(const QString& uid, qint64 rev)
{
    // Guarded like every other invokable that touches the sync DAOs. These
    // three used to be unguarded, so a click landing inside sync()'s nested
    // QEventLoop enqueued a pending change that the in-flight sync then
    // discarded -- the user saw "Synced" while their edit was silently lost.
    // ContactSyncRepository no longer truncates the queue, but leaving these
    // re-entrant would still let a mutation interleave with a sync's
    // read-modify-write.
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    m_repository.queueDelete(uid, rev);
    setLastError(QString());
    load();
    return true;
}

QVariantList ContactsController::allGroups()
{
    QVariantList list;
    const QVector<Group> groups = m_groupsRepository.groups();
    list.reserve(groups.size());
    for (const Group& group : groups) {
        QVariantMap map;
        map[QStringLiteral("id")] = group.id;
        map[QStringLiteral("name")] = group.name;
        list.append(map);
    }
    return list;
}

namespace {

struct ContactSearchCandidate
{
    QString uid;
    QString name;
    QString email;
    QString department;
    bool isPrefixMatch = false;
};

} // namespace

QVariantList ContactsController::searchContacts(const QString& query, int limit)
{
    const QString needle = query.trimmed().toCaseFolded();

    QVector<ContactSearchCandidate> candidates;
    for (const Contact& contact : m_repository.contacts()) {
        const QString name = contact.fn.value_or(QString());
        const QString foldedName = name.toCaseFolded();
        for (const ContactEmailEntry& email : contact.emails) {
            const QString foldedEmail = email.value.toCaseFolded();
            if (!needle.isEmpty() && !foldedName.contains(needle) && !foldedEmail.contains(needle))
                continue;
            ContactSearchCandidate candidate;
            candidate.uid = contact.uid;
            candidate.name = name;
            candidate.email = email.value;
            candidate.department = contact.department.value_or(QString());
            candidate.isPrefixMatch =
                needle.isEmpty() || foldedName.startsWith(needle) || foldedEmail.startsWith(needle);
            candidates.append(candidate);
        }
    }

    // Prefix/exact matches first; std::stable_sort keeps contacts() order
    // as the tiebreaker within each rank.
    std::stable_sort(candidates.begin(), candidates.end(),
                      [](const ContactSearchCandidate& a, const ContactSearchCandidate& b) {
                          return a.isPrefixMatch && !b.isPrefixMatch;
                      });

    if (limit > 0 && candidates.size() > limit)
        candidates.resize(limit);

    QVariantList results;
    results.reserve(candidates.size());
    for (const ContactSearchCandidate& candidate : candidates) {
        QVariantMap map;
        map[QStringLiteral("uid")] = candidate.uid;
        map[QStringLiteral("name")] = candidate.name;
        map[QStringLiteral("email")] = candidate.email;
        map[QStringLiteral("department")] = candidate.department;
        results.append(map);
    }
    return results;
}

QString ContactsController::photoPathFor(const QString& uid)
{
    const std::optional<Contact> found = findByUid(m_repository.contacts(), uid);
    if (!found || !found->photoRef.has_value() || found->photoRef->isEmpty())
        return QString();

    const QString path = m_photoRepository.photoPathFor(uid, *found->photoRef);
    if (path.isEmpty())
        return QString();
    return QUrl::fromLocalFile(path).toString();
}
