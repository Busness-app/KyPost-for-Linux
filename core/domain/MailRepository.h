#pragma once

#include "models/Email.h"
#include "net/RelayAuth.h"
#include "net/RelayMailSource.h" // InboxFetchResult -- crosses the thread hop by value

#include <QString>
#include <QUrl>
#include <QVector>
#include <optional>

class EmailDao;
class HttpClient;
class PairingStore;
class CursorStore;

enum class MailRepositoryOutcome { Success, NotPaired, Unauthorized, ServiceUnavailable, Retry };

struct MailFetchOutcome
{
    MailRepositoryOutcome outcome = MailRepositoryOutcome::Retry;
    QString detail; // meaningful when outcome != Success
};

// Everything a refresh needs from the thread-confined stores, read before the
// request and carried across the thread hop by value. Plain data on purpose:
// it must not hold a reference to anything owned by the calling thread.
struct MailRefreshPlan
{
    RelayEndpoint endpoint;
    QString folder;
    // Absent means "full snapshot" -- see refreshFolder()'s note on why an
    // omitted `since` is not the same as `since=0` on this endpoint.
    std::optional<qint64> since;
};

// Sits between RelayMailSource and EmailDao, matching the Domain/
// Repositories layer in kypost-for-Mac (its MailRepository does a dumb
// full-replace with no delta-merge; Android's MailRepository.kt/
// reconcileFetchResult is the closer reference for the delta-merge logic
// here). Also owns the two wire-mapping fixes RelayMailSource deliberately
// left to this layer (see refreshFolder()'s .cpp comments): populating
// Email::keywords from the per-response byTab buckets, and correcting
// Email::folder to the requested mailbox rather than the tab name
// RelayMailSource stamps on each item.
class MailRepository
{
public:
    MailRepository(RelayMailSource& source, EmailDao& emailDao, PairingStore& pairingStore,
                    CursorStore& cursorStore);

    QVector<Email> cachedEmails(const QString& folder) const;

    // Task 35: looks up a single cached Email by its globally-unique
    // messageId (message_id is EmailDao's SQLite PRIMARY KEY), independent
    // of which folder it happens to be cached under -- unlike
    // cachedEmails(folder) above, a caller navigating straight to a
    // messageId (e.g. EmailDetail.qml, reached via MailController::
    // findByMessageId) doesn't need to already know/guess the folder just
    // to look one email up. Purely a local cache read, no network call.
    std::optional<Email> findCachedEmail(const QString& messageId) const;

    // forceFullResync omits `since` from the request entirely (std::nullopt)
    // for a user-initiated manual refresh, which is what triggers a true
    // full-snapshot response from the backend -- per RelayMailSource's wire
    // contract, `since` present at all (even literally 0) puts the mail
    // endpoint into delta mode instead. (This is the opposite convention
    // from ContactSyncClient, where since=0 really does mean "full sync" --
    // a deliberate, documented wire-contract difference between the two
    // endpoints, not a bug on either side.) Otherwise this method sends the
    // CursorStore-persisted mail cursor (also omitted when empty --
    // first-ever fetch for this folder is a full snapshot the same way).
    MailFetchOutcome refreshFolder(const QString& folder, bool forceFullResync = false);

    // ---- three-phase form, for callers that must not block ---------------
    //
    // Same shape as DeviceRegistrationService's split and for the same
    // reason: only the HTTP request may leave the calling thread. Here the
    // confined parts are PairingStore (caches, mutated by the credential
    // gate), CursorStore (a QSettings file) and EmailDao (a QSqlDatabase
    // connection, usable only from the thread that opened it).
    //
    // Note what this does NOT require: moving the database. The reconcile and
    // every DAO write stay on the calling thread, so nothing about the
    // composition root has to change. docs/THREADING.md previously
    // recommended the opposite; see there for why that was wrong.

    // Phase 1, on the calling thread. Reads the pairing and the stored
    // cursor. Returns nullopt when there is no pairing -- the caller reports
    // MailRepositoryOutcome::NotPaired, exactly as refreshFolder() does.
    std::optional<MailRefreshPlan> planRefresh(const QString& folder, bool forceFullResync) const;

    // Phase 2. The only part that may run on another thread: it touches
    // nothing but the HttpClient it is handed and the plain plan. Static for
    // that reason -- there is no `this` to accidentally reach through.
    static InboxFetchResult fetchWith(HttpClient& httpClient, const MailRefreshPlan& plan);

    // Phase 3, back on the calling thread. Does the keyword/folder wire
    // mapping, the delta merge, and the EmailDao + CursorStore writes.
    MailFetchOutcome applyRefresh(const MailRefreshPlan& plan, const InboxFetchResult& result);

private:
    RelayMailSource& m_source;
    EmailDao& m_emailDao;
    PairingStore& m_pairingStore;
    CursorStore& m_cursorStore;
};
