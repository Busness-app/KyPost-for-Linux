#include "domain/MailRepository.h"

#include "db/EmailDao.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/RelayAuth.h"
#include "net/RelayMailSource.h"
#include "stores/CursorStore.h"

#include <QMap>
#include <QSet>
#include <QUrl>
#include <algorithm>

namespace {

// Display-only bucket the backend falls back to for messages matching no
// real keyword (bucket(e.Keywords, ...) in the Go source) -- excluded from
// Email::keywords the same way KeywordTabs.kt/KeywordRepository.swift only
// ever deal in real keyword names.
const QString kUncategorizedTab = QStringLiteral("Uncategorized");

MailRepositoryOutcome outcomeFromNetworkError(NetworkError error)
{
    switch (error) {
    case NetworkError::Unauthorized:
        return MailRepositoryOutcome::Unauthorized;
    case NetworkError::ServiceUnavailable:
        return MailRepositoryOutcome::ServiceUnavailable;
    default:
        return MailRepositoryOutcome::Retry;
    }
}

} // namespace

MailRepository::MailRepository(RelayMailSource& source, EmailDao& emailDao, PairingStore& pairingStore,
                                CursorStore& cursorStore)
    : m_source(source)
    , m_emailDao(emailDao)
    , m_pairingStore(pairingStore)
    , m_cursorStore(cursorStore)
{
}

QVector<Email> MailRepository::cachedEmails(const QString& folder) const
{
    return m_emailDao.findByFolder(folder);
}

std::optional<Email> MailRepository::findCachedEmail(const QString& messageId) const
{
    return m_emailDao.findById(messageId);
}

std::optional<MailRefreshPlan> MailRepository::planRefresh(const QString& folder, bool forceFullResync) const
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return std::nullopt;

    MailRefreshPlan plan;
    plan.endpoint = RelayEndpoint{ QUrl(pairing->serverBaseUrl),
                                    RelayAuth{ pairing->deviceId, pairing->deviceSecret } };
    plan.folder = folder;

    // forceFullResync must leave `since` as std::nullopt (omitted from the
    // request) rather than std::optional<qint64>(0) -- per RelayMailSource's
    // wire contract, `since` present at all (even 0) puts the mail endpoint
    // into delta mode, not full-snapshot mode. Only an omitted `since`
    // triggers the full-snapshot response that routes through
    // replaceFolderSnapshot in applyRefresh().
    if (!forceFullResync) {
        const QString storedCursor = m_cursorStore.mailCursor();
        if (!storedCursor.isEmpty())
            plan.since = storedCursor.toLongLong();
    }
    return plan;
}

InboxFetchResult MailRepository::fetchWith(HttpClient& httpClient, const MailRefreshPlan& plan)
{
    // Constructed here rather than reused from m_source: RelayMailSource is a
    // stateless wrapper over an HttpClient reference, and on the async path
    // that HttpClient belongs to the executor thread.
    RelayMailSource source(httpClient);
    return source.fetchInbox(plan.endpoint.serverBaseUrl, plan.endpoint.auth, std::nullopt, plan.folder, plan.since);
}

MailFetchOutcome MailRepository::applyRefresh(const MailRefreshPlan& plan, const InboxFetchResult& result)
{
    const QString& folder = plan.folder;
    if (result.error.has_value())
        return { outcomeFromNetworkError(*result.error), result.detail };

    // The backend buckets one message into every keyword-tab it matches, so
    // the same messageId can appear as a separate copy in more than one
    // byTab array. Group by messageId, take any one copy as the base row,
    // collect the (non-Uncategorized) tab names it appeared under into
    // Email::keywords, and stamp Email::folder with the mailbox that was
    // requested rather than the tab name RelayMailSource set it to --
    // RelayMailSource intentionally leaves this mapping to the domain layer
    // (see its own header comments).
    QMap<QString, Email> emailsById;
    QMap<QString, QSet<QString>> keywordsById;
    QStringList order;
    // Message ids the delta flagged as "updated" (flags/label changed, body
    // deliberately omitted). Consumed by the delta branch below to avoid
    // overwriting a cached body with the empty one the server sent on
    // purpose.
    QSet<QString> updatedIds;

    for (auto it = result.byTab.constBegin(); it != result.byTab.constEnd(); ++it) {
        const QString& tab = it.key();
        for (const InboxEmailItem& item : it.value()) {
            const QString& id = item.email.messageId;
            if (!emailsById.contains(id)) {
                emailsById.insert(id, item.email);
                order.append(id);
            }
            if (item.changeType.has_value() && *item.changeType == QStringLiteral("updated"))
                updatedIds.insert(id);
            if (tab != kUncategorizedTab)
                keywordsById[id].insert(tab);
        }
    }

    QVector<Email> emails;
    emails.reserve(order.size());
    for (const QString& id : order) {
        Email email = emailsById.value(id);
        email.folder = folder;
        QStringList keywords = keywordsById.value(id).values();
        std::sort(keywords.begin(), keywords.end());
        email.keywords = keywords;
        emails.append(email);
    }

    if (!result.isDelta) {
        m_emailDao.replaceFolderSnapshot(folder, emails);
    } else {
        // insertOrReplace is upsert-by-messageId, but it must not be handed
        // the row verbatim on an "updated" delta. The backend documents
        // (internal/api/server.go, inbox entry ChangeType) that "updated"
        // means "flags/label changed, Body intentionally empty -- the client
        // already has it cached". Upserting that row as-is wipes the cached
        // body, so opening a message after any flag change showed a blank
        // one.
        //
        // That was already a bug; PGP state makes it a dangerous one. A
        // message with pgpEncrypted set, no body and no decrypt error is the
        // exact wire signature of a client-protected message, so a wiped
        // body would make pgpMessageStateOf() report ClientProtected and
        // tell a server-mode user their own readable mail is unreadable --
        // the precise failure mailcache/store.go's warmBody() comment warns
        // clients about.
        //
        // Preserving is gated on all three of: the delta says "updated", the
        // incoming row has no body, and a cached body actually exists. A
        // genuinely client-protected message has no cached body to restore,
        // so it still classifies correctly.
        for (const Email& email : emails) {
            Email toStore = email;
            const bool incomingHasBody = toStore.body.has_value() && !toStore.body->isEmpty();
            if (!incomingHasBody && updatedIds.contains(toStore.messageId)) {
                const std::optional<Email> cached = m_emailDao.findById(toStore.messageId);
                if (cached.has_value() && cached->body.has_value() && !cached->body->isEmpty()) {
                    toStore.body = cached->body;
                    if (toStore.preview.isEmpty())
                        toStore.preview = cached->preview;
                }
            }
            m_emailDao.insertOrReplace(toStore);
        }
        for (const QString& id : result.removed)
            m_emailDao.deleteById(id);
        m_cursorStore.setMailCursor(QString::number(result.cursor));
    }

    return { MailRepositoryOutcome::Success, QString() };
}

// The synchronous form, kept as the composition of the three phases above.
//
// Not a duplicate implementation: every caller that can afford to block goes
// through here, so the async path cannot drift away from the delta-merge and
// keyword-mapping behaviour this class's tests pin.
MailFetchOutcome MailRepository::refreshFolder(const QString& folder, bool forceFullResync)
{
    const std::optional<MailRefreshPlan> plan = planRefresh(folder, forceFullResync);
    if (!plan.has_value())
        return { MailRepositoryOutcome::NotPaired, QStringLiteral("Not paired") };

    const InboxFetchResult result =
        m_source.fetchInbox(plan->endpoint.serverBaseUrl, plan->endpoint.auth, std::nullopt, plan->folder,
                             plan->since);
    return applyRefresh(*plan, result);
}
