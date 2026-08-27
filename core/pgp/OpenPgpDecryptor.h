#pragma once

#include <QByteArray>
#include <QString>

// Decrypts an OpenPGP message using the user's own GnuPG installation.
//
// CUSTODY MODEL, decided 2026-08-22 (AGENTS.md §4a): this class never holds
// key material and never sees a passphrase. Both stay inside gpg-agent,
// where the user already keeps them -- which is also how hardware tokens
// and smartcards work here without a line of code, and why most of §4a's
// rules about storing keys do not apply to this implementation. What we
// hold is the decrypted message, which is cached mail like any other.
//
// The `homeDirectory` argument exists for tests: pointing a context at a
// throwaway GNUPGHOME is what makes this testable without touching (or
// depending on) the developer's real keyring. Production passes nothing and
// gets the user's own.
enum class PgpDecryptStatus
{
    Decrypted,
    NoSecretKey,       // nothing in this keyring can open it
    CancelledOrWrongPassphrase, // pinentry was dismissed, or the passphrase was wrong
    Malformed,         // not an OpenPGP message, or a corrupt one
    TooLarge,          // the plaintext exceeded the bound; nothing is returned
    EngineUnavailable, // no usable gpg on this system
};

// What gpg made of the signature inside an encrypted message, if any.
//
// Deliberately NOT a verdict. This says what the mathematics says; whether the
// signer is the person the message claims to be from is a binding question,
// and it is answered where the sender is known -- see EncryptedMessageReader.
// Reporting "valid" here and letting a caller read it as "from the sender" is
// exactly the mistake that binding exists to prevent.
struct PgpSignature
{
    // A signature was present at all. Absent is not suspicious on its own --
    // plenty of encrypted mail is unsigned -- but it is not "verified" either.
    bool present = false;

    // The signature checks out against the key that made it. Says nothing
    // about WHOSE key that is.
    //
    // Read from gpgme's per-signature status rather than from its `summary`
    // VALID bit, for the same reason the send path sets ALWAYS_TRUST: a key
    // that arrived from key discovery has no path in the user's web of trust,
    // so the VALID bit is never set for it and every real signature would
    // report as unverified.
    bool mathematicallyValid = false;

    // No public key to check with, which is not the same as a bad signature
    // and must never be shown as one.
    bool keyUnavailable = false;

    // The key that made it, as gpg computed it -- never as anything claimed.
    // This is the SIGNING key, which on any key with a dedicated signing
    // subkey is the subkey and not the identity anything is bound to.
    QString fingerprint;

    // The primary key that signing key belongs to, resolved by gpg from its
    // own keyring. This is what an address binding names, so it is what a
    // binding check compares. Empty when gpg could not resolve it -- which is
    // not a match and must never be treated as one.
    QString primaryFingerprint;
};

struct PgpDecryptResult
{
    PgpDecryptStatus status = PgpDecryptStatus::EngineUnavailable;
    QByteArray plaintext; // set only when Decrypted
    PgpSignature signature;
};

class OpenPgpDecryptor
{
public:
    // A PGP payload is attacker-controlled input -- it arrives from whoever
    // sent the mail -- and compressed OpenPGP data can expand by orders of
    // magnitude, which no bound on the WIRE size constrains (AGENTS.md §4a).
    // So the ceiling is applied to the plaintext as it is produced, and
    // decryption is abandoned the moment it is passed rather than after a
    // few hundred megabytes have been allocated.
    static constexpr qint64 kMaxPlaintextBytes = 32LL * 1024 * 1024;

    explicit OpenPgpDecryptor(qint64 maxPlaintextBytes = kMaxPlaintextBytes);

    PgpDecryptResult decrypt(const QByteArray& ciphertext, const QString& homeDirectory = QString()) const;

    // False when there is no usable gpg on this system. Callers use it to
    // explain the situation once rather than reporting every message as
    // undecryptable.
    static bool engineAvailable();

private:
    qint64 m_maxPlaintextBytes;
};
