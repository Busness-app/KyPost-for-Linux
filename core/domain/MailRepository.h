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

// CacheWriteFailed is local, not transport: the relay answered, but writing
// the rows to SQLite did not land. It is its own value rather than folded
// into Retry because the cursor handling differs (it is deliberately NOT
// advanced, so the same window is fetched again) and because what the user
// can do about it -- free disk space, check the profile -- has nothing to do
// with the network.
//
// PairingChanged means the reply was authorised by a pairing this device no
// longer has -- it arrived after an unpair or after a different account was
// paired -- so nothing was written. Not an error: the request did what it was
// told, the answer is simply no longer ours to keep. Distinct from NotPaired,
// which is "there was nothing to ask with in the first place".
enum class MailRepositoryOutcome {
    Success,
    NotPaired,
    Unauthorized,
    ServiceUnavailable,
    Retry,
    CacheWriteFailed,
    PairingChanged
};

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
    // Always sent, never omitted -- see refreshFolder()'s note. 0 means "I
    // have no cursor, send the whole window and tell me where it ends".
    qint64 since = 0;
    // Which cursor slot applyRefresh() writes the response's cursor back to.
    // Carried on the plan rather than re-read after the request because the
    // pairing can be cleared while the request is in flight, and a cursor
    // filed under an empty subscriber id is a cursor that is never found
    // again.
    QString subscriberId;
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

    // Looks up one cached Email for a caller that has only a messageId and no
    // folder -- opening mail from a desktop notification, whose payload
    // carries no mailbox. Purely a local cache read, no network call.
    //
    // messageId stopped being globally unique in migration 006: the PRIMARY
    // KEY is (folder, message_id), because the relay serves the same id from
    // every mailbox that holds it. So this returns nullopt when the id is
    // cached under more than one folder rather than picking one -- opening
    // the Archive copy of a message the notification announced in INBOX is a
    // wrong-message bug, and nothing here can break the tie. Callers that DO
    // know the folder must use cachedEmail(folder, messageId) instead.
    std::optional<Email> findCachedEmail(const QString& messageId) const;

    // The unambiguous form, for every caller that already knows the mailbox.
    std::optional<Email> cachedEmail(const QString& folder, const QString& messageId) const;

    // Which mailboxes hold this id. Two or more means a caller with only an
    // id -- a notification tap-through -- cannot be answered without asking
    // the user, and this is what it asks with.
    QStringList foldersHolding(const QString& messageId) const;

    // forceFullResync sends `since=0`; every other refresh sends the
    // CursorStore-persisted cursor for this (subscriber, folder), or 0 when
    // there isn't one yet. `since` is ALWAYS sent.
    //
    // This header used to say the opposite -- that a full snapshot required
    // omitting `since` entirely, because "since present at all, even 0, puts
    // the endpoint into delta mode". That is not what the relay does, and
    // believing it cost this client delta sync altogether. From
    // kypost-server's backend/internal/api/server_inbox.go:
    //
    //     cursorSync := strings.TrimSpace(r.URL.Query().Get("since")) != ""
    //     ...
    //     "delta":  since > 0,
    //     "cursor": result.Cursor,
    //
    // Sending `since` selects the CURSOR PROTOCOL; the VALUE decides whether
    // the window comes back partial. `cursor` is returned only on that path.
    // So omitting `since` took the classic path, which returns no cursor --
    // so nothing was ever persisted, so the next refresh had no cursor, so it
    // omitted `since` again. The client never once entered delta mode and
    // re-fetched the full window, bodies included, every 90 seconds.
    // server_inbox.go:290 names this exact conflation as a bug it had already
    // fixed on its own side. kypost-android has always sent since=0
    // (RelayMailSource.kt's sinceValue()); this is a Linux-only regression
    // against a contract both siblings already followed.
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
