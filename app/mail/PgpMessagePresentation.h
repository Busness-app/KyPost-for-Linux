#pragma once

#include "domain/PgpMessageState.h"

#include <QString>
#include <QUrl>

// User-facing presentation of core/domain/PgpMessageState's four states.
//
// Split out of core/ because everything here is localized prose, and core/ is
// held to a QtCore/QtNetwork/QtSql-only boundary with no KF6::I18n (AGENTS.md
// section 8). The decision itself stays in core; only its wording lives here.

// Glyph for an inbox row, or an empty string for no marker.
//
// Only the two states that yield no readable content are marked.
// DecryptedByServer is deliberately unmarked: those rows open and read
// normally, so a marker would appear on most rows in a server-mode mailbox
// while carrying nothing the user can act on -- and the detail view already
// discloses that the server decrypted it.
QString pgpRowMarker(PgpMessageState state);

// Spelled-out equivalent of pgpRowMarker() for screen readers, or an empty
// string where there is no marker. Separate from the glyph because a lock
// emoji is announced inconsistently (or not at all) across AT stacks.
QString pgpRowMarkerAccessibleName(PgpMessageState state);

// Heading and body copy for the detail-view banner, or empty strings for
// states that need no banner (None).
QString pgpBannerTitle(PgpMessageState state);
QString pgpBannerBody(PgpMessageState state, const QString& decryptError);

// Builds the webmail URL for reading a message the client cannot decrypt.
//
// Returns an empty QUrl unless `serverBaseUrl` is a valid https URL, so a
// malformed or downgraded pairing can never produce a link the UI will open
// externally. Mirrors the containment thinking in
// app/pgp/PgpQrTargetValidator.
QUrl webmailReadUrl(const QUrl& serverBaseUrl, const QString& mailbox, const QString& messageId);
