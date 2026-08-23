#pragma once

#include "net/PgpPayloadClient.h"
#include "pgp/OpenPgpDecryptor.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

struct RelayAuth;

// Every way reading one client-protected message can end, as one flat set.
//
// Flat on purpose. The fetch can fail four ways and the decrypt five, and a
// caller handed both halves separately has to re-derive "what do I tell the
// user" from a pair -- which is how "we could not read this message" ends up
// meaning nine different things on screen, or one. Each value below is a
// distinct position for the reader to be in, and each one has a different
// answer to "what can I do about it".
enum class PgpReadStatus
{
    // Plaintext in hand.
    Decrypted,

    // Nothing here to decrypt: no OpenPGP payload at all, or a signed-but-
    // not-encrypted message this client has no verifier for. Terminal.
    NoCiphertext,

    // The account still holds its key server-side and never migrated to
    // client custody. The server refuses ciphertext to such an account and
    // no longer decrypts for it either, so the way out is finishing the
    // migration -- not retrying, and not a different device.
    ServerCustody,

    // Too large to read here, whoever refused: the relay would not hold the
    // message, this client would not receive it, or the plaintext exceeded
    // the decryptor's ceiling as it was produced. One status because the
    // reader's position is the same in all three and none is retryable.
    TooLarge,

    // Transport, auth, or a response that would not decode. The ONLY
    // retryable status here -- everything else above and below is terminal,
    // and a UI that offers Retry on the others is offering a button that
    // cannot work.
    FetchFailed,

    // The ciphertext arrived and this machine's keyring cannot open it.
    // Usually means the mail is addressed to a key the user holds elsewhere.
    NoSecretKey,

    // pinentry was dismissed, or the passphrase was wrong. Deliberately not
    // folded into NoSecretKey: the key IS here and the user can try again,
    // which is the opposite of what NoSecretKey means.
    CancelledOrWrongPassphrase,

    // The bytes are not an OpenPGP message, or are a corrupt one.
    Malformed,

    // No usable gpg on this system. Checked BEFORE the fetch, so this
    // answers without a pointless round trip and without pulling somebody's
    // ciphertext into memory for a decryption that was never going to run.
    EngineUnavailable,
};

struct PgpReadResult
{
    PgpReadStatus status = PgpReadStatus::FetchFailed;

    // Set only when Decrypted.
    //
    // THIS MUST NOT BE PERSISTED. The sender chose end-to-end encryption;
    // the database key lives in the platform secret store and is not behind
    // the user's OpenPGP passphrase, so writing the plaintext into the cache
    // would silently downgrade the message to the protection level of every
    // other mail in the app. That is not this class's decision to enforce --
    // it hands the bytes over and they are the caller's from then on -- so
    // it is stated here as a constraint on callers rather than claimed as a
    // property of the type.
    QByteArray plaintext;

    // Human-readable detail on FetchFailed; empty otherwise. Never a
    // user-facing sentence -- core/ owns error values, not wording
    // (AGENTS.md section 6c).
    QString detail;
};

// Reads one client-protected message: fetches its ciphertext from the relay,
// then decrypts it with the user's own gpg-agent.
//
// THREADING. Every call blocks twice, and the second one is unbounded: the
// HTTP fetch blocks on HttpClient's event loop, and the decrypt blocks for
// as long as the user takes to answer pinentry -- which for a hardware token
// means until they physically touch it, and which they may never do. This
// must run on NetworkExecutor's worker thread. Calling it on the GUI thread
// freezes the application on a dialog the application is not drawing.
class EncryptedMessageReader
{
public:
    EncryptedMessageReader(const PgpPayloadClient& payloads, const OpenPgpDecryptor& decryptor);

    // gnupgHome is for tests, and is passed straight through to
    // OpenPgpDecryptor -- production passes nothing and gets the user's own
    // keyring.
    PgpReadResult read(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& mailbox,
                        const QString& messageId, const QString& gnupgHome = QString()) const;

private:
    const PgpPayloadClient& m_payloads;
    const OpenPgpDecryptor& m_decryptor;
};
