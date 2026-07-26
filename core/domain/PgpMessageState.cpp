#include "domain/PgpMessageState.h"

PgpMessageState pgpMessageStateOf(bool pgpEncrypted, const QString& pgpDecryptError,
                                   const std::optional<QString>& body)
{
    if (!pgpEncrypted)
        return PgpMessageState::None;
    if (!pgpDecryptError.trimmed().isEmpty())
        return PgpMessageState::DecryptFailed;
    if (body.has_value() && !body->trimmed().isEmpty())
        return PgpMessageState::DecryptedByServer;
    return PgpMessageState::ClientProtected;
}

// Takes the *cached* Email deliberately. A raw delta item with
// changeType == "updated" carries an intentionally empty body (the backend
// documents this: the client already has it cached), which is byte-for-byte
// the wire signature of a client-protected message. Classifying that item
// directly would tell a server-mode user their own readable mail is
// unreadable. MailRepository preserves the cached body on such deltas
// precisely so that by the time an Email reaches here, an empty body really
// does mean "there is no body".
PgpMessageState pgpMessageStateOf(const Email& email)
{
    return pgpMessageStateOf(email.pgpEncrypted, email.pgpDecryptError, email.body);
}
