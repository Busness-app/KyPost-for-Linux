#pragma once

#include <QString>
#include <QStringList>
#include <optional>

// `atUtc` is kept as the raw wire ISO-8601 UTC string (matching the
// backend's `/api/inbox` `atUTC` field) rather than a Qt date/time type, to
// avoid parsing/timezone loss at the model layer.
struct Email
{
    QString messageId;
    QString folder;
    QString sender;
    QString sentTo;
    QString cc;
    QString bcc;
    QString subject;
    QString preview;
    std::optional<QString> body;
    QString label;
    QStringList keywords;
    QString status;
    QString atUtc;
    bool hasAttachments = false;
    QString sourceMode;

    // OpenPGP state as reported by /api/inbox (`pgpEncrypted` /
    // `pgpDecryptError`, both `omitempty` on the wire, so absent means
    // false/empty). Deliberately NOT interpreted here -- the four-way
    // decision they feed lives in core/domain/PgpMessageState.h, because the
    // rule depends on `body` as well and must be identical everywhere.
    //
    // This client holds no private key and never decrypts anything; these
    // fields exist so the UI can explain an unreadable message rather than
    // render a blank one.
    bool pgpEncrypted = false;
    QString pgpDecryptError;

    bool operator==(const Email&) const = default;
};
