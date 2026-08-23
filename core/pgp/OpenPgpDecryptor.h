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

struct PgpDecryptResult
{
    PgpDecryptStatus status = PgpDecryptStatus::EngineUnavailable;
    QByteArray plaintext; // set only when Decrypted
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
