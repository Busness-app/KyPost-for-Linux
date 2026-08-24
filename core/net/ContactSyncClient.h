#pragma once

#include "models/Contact.h"
#include "net/NetworkError.h"

#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QVector>
#include <optional>

class HttpClient;
struct RelayAuth;

// Every core/models/Contact.h field maps 1:1 onto a same-named JSON key
// (confirmed against the Go Contact/ContactValue/ContactAddress structs) --
// exported here (rather than kept file-local to ContactSyncClient.cpp) so
// core/db/PendingContactChangeDao's callers (ContactSyncRepository) can
// serialize a queued local change with the exact same mapping this client
// uses on the wire, instead of a second, divergence-prone copy.
namespace ContactWire {

QJsonObject contactToJson(const Contact& contact);
Contact contactFromJson(const QJsonObject& obj);

} // namespace ContactWire

// Response shape shared by both GET (pull) and POST (push)
// {serverBaseUrl}/api/contacts/sync, verified against the Go backend's
// handleContactsSync and contacts.Store.ChangedSince (see Task 16 brief).
// deletedContacts holds full Contact objects, not bare uids — the backend's
// "deleted" array carries the same Contact JSON shape as "changed", just
// with every other field zeroed/tombstoned server-side and Deleted: true
// (which is not itself a Contact field; deletedContacts vs. changed is the
// deleted flag). When tooOld is true, the server omits "changed"/"deleted"
// entirely, so both vectors are simply empty in that case — this client
// only surfaces tooOld; the reset-and-wipe behavior it implies is Phase 4
// (ContactSyncRepository), not implemented here.
struct ContactSyncResult
{
    std::optional<NetworkError> error;
    QString detail; // human-readable detail on error; empty otherwise
    qint64 cursor = 0;
    bool tooOld = false;
    QVector<Contact> changed;
    QVector<Contact> deletedContacts;
};

// One merged group from a dedupe pass -- survivor is the uid that absorbed
// the others, absorbed is the list of now-tombstoned loser uids. Matches the
// Go backend's DedupeMerge{Survivor, Absorbed} (internal/contacts/store.go).
struct ContactDedupeGroup
{
    QString survivor;
    QVector<QString> absorbed;

    bool operator==(const ContactDedupeGroup&) const = default;
};

// Response from POST {serverBaseUrl}/api/contacts/dedupe -- the Go backend's
// DedupeReport{MergedCount, Groups}. mergedCount is the total number of
// tombstoned losers across every group; groups is empty (not an error) when
// nothing merged. This call does not itself return the resulting Contact
// changes -- callers must pull()/push() separately to fetch the tombstones
// and survivor updates, same as any other server-side mutation.
struct ContactDedupeResult
{
    std::optional<NetworkError> error;
    QString detail; // human-readable detail on error; empty otherwise
    int mergedCount = 0;
    QVector<ContactDedupeGroup> groups;
};

// Syncs contacts with the Relay backend via GET/POST {serverBaseUrl}/api/
// contacts/sync, verified against internal/api/contacts_handlers.go's
// handleContactsSync and internal/contacts/contacts.go's Contact/
// ContactValue/ContactAddress structs (see Task 16 brief) — sub/hash are
// query params on both verbs (unlike NativeRegistrationClient, which takes
// no query-param auth), and
// every core/models/Contact.h field maps 1:1 onto a same-named JSON key.
class ContactSyncClient
{
public:
    explicit ContactSyncClient(HttpClient& httpClient);

    // since = 0 requests a full initial sync.
    ContactSyncResult pull(const QUrl& serverBaseUrl, const RelayAuth& auth, qint64 since) const;

    // An empty Contact::uid within changes marks a create (server assigns
    // the real uid) -- falls out naturally since uid is a plain QString, no
    // special sentinel needed.
    ContactSyncResult push(const QUrl& serverBaseUrl, const RelayAuth& auth, qint64 baseCursor,
                            const QVector<Contact>& changes) const;

    // POST {serverBaseUrl}/api/contacts/dedupe, empty body, sub/hash as query
    // params (same withMailAuth shape pull()/push() already use). A strict
    // subset of parseSyncResponse's job -- no changed/deleted arrays to walk.
    ContactDedupeResult dedupe(const QUrl& serverBaseUrl, const RelayAuth& auth) const;

private:
    HttpClient& m_httpClient;
};
