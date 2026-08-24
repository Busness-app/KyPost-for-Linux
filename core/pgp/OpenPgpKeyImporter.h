#pragma once

#include <QByteArray>
#include <QString>

#include "security/SecureBytes.h"

// Puts a recipient's public key where gpg can encrypt to it: the user's own
// GnuPG keyring.
//
// CUSTODY DECISION, 2026-08-23. Encrypting to a key requires gpg to hold it,
// and the three ways to arrange that are not equivalent:
//
//   * an ephemeral keyring per send -- contained, but gpg then has no record
//     across sends, so a recipient's key changing under them is invisible;
//   * a pinning store of our own -- detects that, but is a second and weaker
//     copy of key-change rules gpg already models;
//   * the user's own keyring -- what every other mail client does. gpg owns
//     the record, key changes surface in the user's own tooling rather than
//     only inside this app, and nothing here has to reimplement trust.
//
// The third was chosen. The cost is that sending mail modifies the user's
// GnuPG keyring, which is why the rules below are what they are.
//
// NON-DESTRUCTIVE, and verified rather than asserted: gpg MERGES an imported
// key into whatever it already holds -- it never replaces or deletes. A newer
// copy of a key the user already has gains its new user IDs and signatures and
// keeps the old ones. OpenPgpKeyImporterTest proves both by importing a
// second key and a merged copy and re-reading the keyring afterwards.
//
// WHAT THIS DOES NOT DO. It cannot tell you the relay handed you the right
// person's key. The fingerprint check below catches a relay whose key and
// whose claim about that key disagree; it cannot catch one that lies
// consistently. Persisting into the keyring is what gives the user a chance to
// notice, and it is a chance, not a guarantee.
enum class PgpImportStatus
{
    // The keyring gained something: a new key, or new material on one it had.
    Imported,

    // gpg already had exactly this. Importing is idempotent, so a resend does
    // not churn the keyring, and this is a success.
    Unchanged,

    // Not imported, and the user's keyring was NOT touched. Either the bytes
    // are not a usable public key, or the fingerprint gpg computed is not the
    // one the relay claimed.
    Rejected,

    EngineUnavailable,
};

struct PgpImportResult
{
    PgpImportStatus status = PgpImportStatus::EngineUnavailable;
    // The fingerprint gpg itself computed, never the one that was claimed.
    QString fingerprint;
    // Why it was rejected, for a log line. Never user-facing wording: core/
    // owns error values, not prose (AGENTS.md section 6c).
    QString detail;
};

// Imports one ASCII-armored public key.
//
// expectedFingerprint is checked BEFORE the user's keyring is touched at all.
// The check happens in a throwaway GnuPG home, so a key whose fingerprint does
// not match what the relay claimed never enters the real keyring even
// momentarily -- the alternative, importing and then deleting, would mean
// this code deletes from a keyring it does not own, and a half-finished run
// would leave the bad key behind. Passing an empty expectedFingerprint skips
// the comparison but still reports what gpg computed.
//
// homeDirectory is for tests, as everywhere else in core/pgp; production
// passes nothing and gets the user's own keyring.
PgpImportResult importPublicKey(const QByteArray& armoredPublicKey, const QString& expectedFingerprint,
                                 const QString& homeDirectory = QString());

PgpImportResult importPrivateKey(const SecureBytes& armoredPrivateKey, const QString& expectedFingerprint,
                                  const QString& homeDirectory = QString());
