#include "pgp/EncryptedMessageReader.h"

#include "net/RelayAuth.h"
#include "pgp/OpenPgpKeyImporter.h"

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

// The binding, and the whole point of the exercise.
//
// A signature is credited to the sender only when the key that made it is one
// the relay's address book already binds to the address it resolved. gpg's own
// trust model cannot supply this: keys arriving from discovery have no path in
// the user's web of trust, so nothing would ever verify. What gpg supplies is
// the mathematics; the identity comes from the binding.
PgpSignatureVerdict verdictFor(const PgpSignature& signature, const QStringList& boundFingerprints)
{
    if (!signature.present)
        return PgpSignatureVerdict::None;
    if (signature.keyUnavailable)
        return PgpSignatureVerdict::CannotCheck;
    if (!signature.mathematicallyValid)
        return PgpSignatureVerdict::Invalid;
    if (boundFingerprints.isEmpty())
        return PgpSignatureVerdict::CannotCheck;

    // The PRIMARY key of whatever made the signature, not the signing key
    // itself. A sender whose key has a dedicated signing subkey signs with the
    // subkey while a binding names the primary, so comparing the signature's
    // own fingerprint -- which this did until 2026-08-26 -- never matched for
    // them, and every hardware-token user was told their mail came from a key
    // nobody knew. gpg resolves the subkey to its primary; when it cannot, the
    // resolution is empty and matches nothing, which is the safe direction.
    //
    // Compared case-insensitively because gpg renders a fingerprint in upper
    // case and nothing guarantees the other side did. A case mismatch here
    // would silently downgrade every valid signature to "unknown key", which
    // reads as an accusation.
    if (signature.primaryFingerprint.isEmpty())
        return PgpSignatureVerdict::ValidFromUnknownKey;
    for (const QString& fingerprint : boundFingerprints) {
        if (fingerprint.compare(signature.primaryFingerprint, Qt::CaseInsensitive) == 0) {
            return PgpSignatureVerdict::ValidFromSender;
        }
    }
    return PgpSignatureVerdict::ValidFromUnknownKey;
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

    // The keys a signature may be credited to, into the keyring so gpg can
    // check against them (AGENTS.md 4b -- the same custody decision as the
    // send path). Only keys the relay ALREADY bound to this message's sender
    // are imported, so an unsolicited MESSAGE cannot add arbitrary keys to
    // somebody's keyring: it can at most add the one its own address is
    // already associated with.
    //
    // EVERY IMPORT IS FINGERPRINT-CHECKED, exactly as the send path's is.
    // Until 2026-08-24 this passed an empty expectedFingerprint, which
    // switches that check off inside importPublicKey() -- so the read path
    // wrote relay-chosen public keys into the user's durable GnuPG keyring
    // with nothing verified about them, while the send path a few files away
    // refused to do the same thing without a match. The asymmetry was the
    // bug; a comment calling the result "clutter" was wishful, because the
    // keyring is security state the user owns and this is an unsolicited
    // message causing a permanent write to it.
    //
    // What the check buys, stated honestly: it catches a relay whose key and
    // whose claim about that key disagree. It does NOT stop a relay that lies
    // consistently -- see OpenPgpKeyImporter.h's WHAT THIS DOES NOT DO -- and
    // no check on this side of the wire can, because the relay is the party
    // asserting the binding in the first place.
    //
    // A key with no claimed fingerprint is skipped entirely rather than
    // imported unchecked. It costs a badge (the verdict falls to CannotCheck,
    // which is what "the question was not answerable" is for) and it keeps
    // the keyring clean. Fail-closed is the right direction for a downgrade
    // nobody can be misled by.
    //
    // A conflicting key is skipped rather than tried. The relay saw more than
    // one claiming this address and cannot say which is right; importing them
    // all would let whichever one verified decide the answer.
    QStringList boundFingerprints;
    for (const PgpSignerKey& key : payload.signerKeys) {
        if (key.conflict || key.fingerprint.isEmpty())
            continue;
        const PgpImportResult imported =
            importPublicKey(key.publicKey.toUtf8(), key.fingerprint, gnupgHome);
        if (imported.status == PgpImportStatus::Imported
            || imported.status == PgpImportStatus::Unchanged) {
            boundFingerprints.append(imported.fingerprint);
        }
    }

    const PgpDecryptResult decrypted = m_decryptor.decrypt(payload.encryptedPayload.toUtf8(), gnupgHome);
    out.status = statusFromDecrypt(decrypted.status);
    if (out.status != PgpReadStatus::Decrypted)
        return out;

    out.plaintext = decrypted.plaintext;
    out.signedBy = payload.resolvedSender;
    out.signature = verdictFor(decrypted.signature, boundFingerprints);
    return out;
}
