#include "pgp/EncryptedMessageReader.h"

#include "net/RelayAuth.h"

namespace {

// Maps the ways a fetch can FAIL. read() handles Fetched before calling
// this, so that case is unreachable -- and is mapped to FetchFailed rather
// than Decrypted precisely because it is. Returning Decrypted here would
// hand back a success with no plaintext, which renders as a blank message
// the user has no reason to distrust. If a later edit ever does reach this
// line, it should fail closed and loudly, not quietly claim to have read
// mail it never opened.
PgpReadStatus statusFromFetch(PgpPayloadStatus status)
{
    switch (status) {
    case PgpPayloadStatus::Fetched:
        return PgpReadStatus::FetchFailed;
    case PgpPayloadStatus::NoCiphertext:
        return PgpReadStatus::NoCiphertext;
    case PgpPayloadStatus::ServerCustody:
        return PgpReadStatus::ServerCustody;
    case PgpPayloadStatus::TooLarge:
        return PgpReadStatus::TooLarge;
    case PgpPayloadStatus::Failed:
        return PgpReadStatus::FetchFailed;
    }
    return PgpReadStatus::FetchFailed;
}

PgpReadStatus statusFromDecrypt(PgpDecryptStatus status)
{
    switch (status) {
    case PgpDecryptStatus::Decrypted:
        return PgpReadStatus::Decrypted;
    case PgpDecryptStatus::NoSecretKey:
        return PgpReadStatus::NoSecretKey;
    case PgpDecryptStatus::CancelledOrWrongPassphrase:
        return PgpReadStatus::CancelledOrWrongPassphrase;
    case PgpDecryptStatus::Malformed:
        return PgpReadStatus::Malformed;
    case PgpDecryptStatus::TooLarge:
        // Joins the relay's two size refusals. The plaintext expanded past
        // the ceiling as it was produced, which is a different mechanism and
        // the same fact: this message is too big to read here.
        return PgpReadStatus::TooLarge;
    case PgpDecryptStatus::EngineUnavailable:
        return PgpReadStatus::EngineUnavailable;
    }
    return PgpReadStatus::Malformed;
}

} // namespace

EncryptedMessageReader::EncryptedMessageReader(const PgpPayloadClient& payloads,
                                                 const OpenPgpDecryptor& decryptor)
    : m_payloads(payloads), m_decryptor(decryptor)
{
}

PgpReadResult EncryptedMessageReader::read(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                            const QString& mailbox, const QString& messageId,
                                            const QString& gnupgHome) const
{
    PgpReadResult out;

    // First, and before the network. A machine with no gpg cannot read this
    // message however the fetch goes, so asking the relay for the ciphertext
    // would be a round trip whose only effect is to pull someone's encrypted
    // mail into a process that has no way to open it.
    if (!OpenPgpDecryptor::engineAvailable()) {
        out.status = PgpReadStatus::EngineUnavailable;
        return out;
    }

    const PgpPayloadResult payload = m_payloads.fetch(serverBaseUrl, auth, mailbox, messageId);
    if (payload.status != PgpPayloadStatus::Fetched) {
        out.status = statusFromFetch(payload.status);
        // Only on the retryable status, matching what the header promises.
        // The terminal ones carry a relay-authored sentence that says
        // nothing the status has not already said, and letting it through
        // invites a UI that prints it -- which would put attacker-influenced
        // text on screen for a message we could not read.
        if (out.status == PgpReadStatus::FetchFailed)
            out.detail = payload.detail;
        return out;
    }

    const PgpDecryptResult decrypted = m_decryptor.decrypt(payload.encryptedPayload.toUtf8(), gnupgHome);
    out.status = statusFromDecrypt(decrypted.status);
    if (out.status == PgpReadStatus::Decrypted)
        out.plaintext = decrypted.plaintext;
    return out;
}
