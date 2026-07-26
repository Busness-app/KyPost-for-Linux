#pragma once

#include "models/Email.h"

#include <QString>
#include <optional>

// What this app can actually do with a message's OpenPGP content.
//
// Ported from kypost-android's pgp/PgpMessageState.kt so both clients reach
// the same verdict from the same three relay fields. Kept as a pure function
// in core/ (no QtGui, no KF6, no controller state -- see AGENTS.md section 8
// on core/'s QtCore/QtNetwork/QtSql-only boundary) so the rule is
// unit-testable and so the UI only ever *picks a view*; it never re-derives
// the decision.
//
// Presentation of these states (glyphs, localized prose) lives in
// app/mail/PgpMessagePresentation.h, which is where i18n is available.
enum class PgpMessageState
{
    // No OpenPGP content: render normally.
    None,

    // Encrypted, and the server deliberately did not decrypt it because the
    // account's key is end-to-end (client) protected. There is no body, and
    // this client holds no private key, so the only route to the content is
    // webmail.
    ClientProtected,

    // Encrypted, and the server tried to decrypt and failed. There is a real
    // error to show.
    DecryptFailed,

    // Encrypted, and the server decrypted it for us. Worth surfacing rather
    // than rendering silently: the user should be able to tell that the
    // server read their mail.
    DecryptedByServer,
};

// The ordering matters. A non-blank `pgpDecryptError` is checked before the
// body, because the server populates the error and leaves the body empty --
// reading that as ClientProtected would send the user to webmail for a
// message that will fail there too, for a reason we were already told.
PgpMessageState pgpMessageStateOf(bool pgpEncrypted, const QString& pgpDecryptError,
                                   const std::optional<QString>& body);

// Convenience overload for a fully-populated Email. Takes the *cached* row
// deliberately -- see the .cpp for why a raw delta item must not be passed.
PgpMessageState pgpMessageStateOf(const Email& email);
