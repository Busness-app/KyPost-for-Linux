#include "mail/PgpMessagePresentation.h"

#include <KLocalizedString>

#include <QUrlQuery>

#include <optional>

QString pgpRowMarker(PgpMessageState state)
{
    switch (state) {
    case PgpMessageState::ClientProtected:
        return QStringLiteral("\U0001F512"); // lock
    case PgpMessageState::DecryptFailed:
        return QStringLiteral("⚠"); // warning sign
    case PgpMessageState::None:
    case PgpMessageState::DecryptedByServer:
        break;
    }
    return {};
}

QString pgpRowMarkerAccessibleName(PgpMessageState state)
{
    switch (state) {
    case PgpMessageState::ClientProtected:
        return i18n("Encrypted, cannot be read in this app");
    case PgpMessageState::DecryptFailed:
        return i18n("Encrypted, could not be decrypted");
    case PgpMessageState::None:
    case PgpMessageState::DecryptedByServer:
        break;
    }
    return {};
}

QString pgpBannerTitle(PgpMessageState state)
{
    switch (state) {
    case PgpMessageState::ClientProtected:
        return i18n("This message is end-to-end encrypted");
    case PgpMessageState::DecryptFailed:
        return i18n("This message could not be decrypted");
    case PgpMessageState::DecryptedByServer:
        return i18n("Decrypted by the server");
    case PgpMessageState::None:
        break;
    }
    return {};
}

QString pgpBannerBody(PgpMessageState state, const QString& decryptError)
{
    switch (state) {
    case PgpMessageState::ClientProtected:
        // Deliberately names *why* rather than apologising: the account is
        // configured so the server cannot read it either, which is the
        // feature working, not a failure.
        return i18n("Your account's PGP key is held only by you, so neither the server nor this "
                    "app can decrypt this message. Open it in webmail, where your key is "
                    "unlocked, to read it.");
    case PgpMessageState::DecryptFailed:
        return decryptError.trimmed().isEmpty()
            ? i18n("The server tried to decrypt this message and failed.")
            : i18n("The server tried to decrypt this message and failed: %1", decryptError.trimmed());
    case PgpMessageState::DecryptedByServer:
        return i18n("This message arrived encrypted. The server decrypted it to show it here, "
                    "which means the server could read its contents.");
    case PgpMessageState::None:
        break;
    }
    return {};
}

namespace {

// The one place the webmail base URL is vetted, shared by webmailReadUrl()
// and webmailMailboxUrl(). https-only and host-bearing: these URLs are handed
// to an external browser, so a pairing holding a file://, javascript: or
// otherwise degraded base must never reach one, and requiring https (not
// merely "not file") stops a downgraded pairing from putting a mailbox name
// or message id on the wire in the clear.
//
// Returns a URL with /read as the path and any inherited query/fragment
// cleared, ready for the caller to set its own query.
std::optional<QUrl> webmailReadBase(const QUrl& serverBaseUrl)
{
    if (!serverBaseUrl.isValid() || serverBaseUrl.isRelative())
        return std::nullopt;
    if (serverBaseUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return std::nullopt;
    if (serverBaseUrl.host().isEmpty())
        return std::nullopt;

    QUrl url = serverBaseUrl;
    url.setPath(QStringLiteral("/read"));
    // Cleared explicitly: a base URL carrying its own query or fragment would
    // otherwise leak into the link, and setQuery() below only replaces the query.
    url.setFragment(QString());
    url.setQuery(QUrlQuery());
    return url;
}

} // namespace

QUrl webmailReadUrl(const QUrl& serverBaseUrl, const QString& mailbox, const QString& messageId)
{
    const std::optional<QUrl> base = webmailReadBase(serverBaseUrl);
    if (!base.has_value())
        return {};
    if (messageId.isEmpty())
        return {};

    QUrl url = *base;
    QUrlQuery query;
    if (!mailbox.isEmpty())
        query.addQueryItem(QStringLiteral("mailbox"), mailbox);
    query.addQueryItem(QStringLiteral("message"), messageId);
    url.setQuery(query);
    return url;
}

// Targets a mailbox rather than one message: POST /api/mail/draft answers with
// a bare {ok:true} and no UID, so a saved draft has nothing to link to.
QUrl webmailMailboxUrl(const QUrl& serverBaseUrl, const QString& mailbox)
{
    const std::optional<QUrl> base = webmailReadBase(serverBaseUrl);
    if (!base.has_value())
        return {};
    if (mailbox.isEmpty())
        return {};

    QUrl url = *base;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("mailbox"), mailbox);
    url.setQuery(query);
    return url;
}
