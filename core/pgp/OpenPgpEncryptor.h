#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Signs a message with the sender's own key and encrypts it to its
// recipients, through the user's gpg-agent.
//
// Same custody model as OpenPgpDecryptor: no key material is held here and no
// passphrase is ever seen. The signature is produced by gpg-agent, which is
// also why a smartcard or hardware token signs with no code on this side.
//
// Sign and encrypt happen in ONE operation (gpgme_op_encrypt_sign), which is
// what RFC 3156 combined mode is and what the relay's DecryptMIME verifies on
// the way back. Signing separately and encrypting the result would produce a
// different structure that the reading path does not check.
enum class PgpEncryptStatus
{
    Encrypted,

    // No usable SECRET key for the sender address. The message is not sent
    // unsigned instead: an unsigned message from an account that normally
    // signs is a downgrade the recipient cannot see, and the user asked for a
    // signed one.
    NoSigningKey,

    // At least one recipient's key is not in the keyring, or gpg will not
    // encrypt to it. Encryption is abandoned rather than performed for the
    // rest -- see the note on partial recipients below.
    RecipientKeyUnusable,

    // pinentry was dismissed, or the passphrase was wrong. The key is there
    // and the user can try again, which is the opposite of NoSigningKey.
    CancelledOrWrongPassphrase,

    Failed,
    EngineUnavailable,
};

struct PgpEncryptResult
{
    PgpEncryptStatus status = PgpEncryptStatus::EngineUnavailable;
    QString armoredCiphertext; // set only when Encrypted
    // Which recipients gpg refused, when status is RecipientKeyUnusable. Named
    // so the user is told WHO the message could not be encrypted to rather
    // than that "encryption failed".
    QStringList unusableRecipients;
    QString detail; // for a log line; never user-facing wording
};

// PARTIAL RECIPIENTS ARE NEVER ACCEPTED.
//
// gpgme will happily encrypt to the recipients it can and report the rest as
// invalid. Taking that result would send a message the sender believes went to
// five people and that only four can open -- and the one left out is the one
// whose key was being discovered for the first time. Worse on a Bcc split,
// where the failing delivery is the one nobody sees. So any invalid recipient
// fails the whole call.
//
// ALWAYS_TRUST is set, and that is a real choice rather than a convenience.
// Keys arriving from the relay's discovery ladder have no path in the user's
// web of trust, so gpg would refuse every one of them; requiring the user to
// certify each recipient before writing to them would make the feature
// unusable, and every other mail client makes the same call. The authenticity
// decision therefore rests on the relay's ladder plus the fingerprint check in
// OpenPgpKeyImporter -- NOT on gpg's trust model. Do not describe this as
// "trusted keys" anywhere the user can read it.
//
// The plaintext is not bounded here, unlike the decrypt path's. There the
// input is a stranger's compressed ciphertext that can expand without limit;
// here it is a message this user just composed and is already in memory.
//
// signerAddress selects the secret key -- an address or a fingerprint, both of
// which gpg accepts. recipientFingerprints are fingerprints rather than
// addresses on purpose: an address can match more than one key in a keyring,
// and "whichever gpg picked" is not a decision this code may make silently.
//
// homeDirectory is for tests, as everywhere else in core/pgp.
PgpEncryptResult signAndEncrypt(const QByteArray& plaintext, const QString& signerAddress,
                                 const QStringList& recipientFingerprints,
                                 const QString& homeDirectory = QString());
