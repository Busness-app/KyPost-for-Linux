#include "mail/PgpMessagePresentation.h"

#include <KLocalizedString>

#include <QUrlQuery>

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

QUrl webmailReadUrl(const QUrl& serverBaseUrl, const QString& mailbox, const QString& messageId)
{
    // https-only and host-bearing. This URL is handed to
    // Qt.openUrlExternally(), so a pairing holding a file://, javascript: or
    // otherwise degraded base URL must never reach a browser. Requiring
    // https (not merely "not file") also stops a downgraded pairing from
    // sending the message id over cleartext.
    if (!serverBaseUrl.isValid() || serverBaseUrl.isRelative())
        return {};
    if (serverBaseUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return {};
    if (serverBaseUrl.host().isEmpty())
        return {};
    if (messageId.isEmpty())
        return {};

    QUrl url = serverBaseUrl;
    url.setPath(QStringLiteral("/read"));
    // Cleared explicitly: a base URL carrying its own query or fragment
    // would otherwise leak into the link, and setQuery() below only replaces
    // the query.
    url.setFragment(QString());

    QUrlQuery query;
    if (!mailbox.isEmpty())
        query.addQueryItem(QStringLiteral("mailbox"), mailbox);
    query.addQueryItem(QStringLiteral("message"), messageId);
    url.setQuery(query);

    return url;
}
