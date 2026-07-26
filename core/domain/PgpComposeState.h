#pragma once

#include <QString>
#include <optional>

// Which PGP controls a compose screen may offer, derived from
// GET /api/pgp/bootstrap. Pure: no network, no Qt beyond QString, so it
// lives in core/domain beside PgpMessageState.
struct PgpComposeState
{
    bool canEncrypt = false;
    bool canSign = false;
    // Replace the toggles with a "continue in webmail" affordance: the
    // account's private key exists only in the user's browser, so no request
    // from this client can sign or encrypt.
    bool handoffToWebmail = false;
};

// Both arguments are optional because a failed bootstrap must be
// distinguishable from a successful "no identity" -- std::nullopt means
// "could not check", which hides every control rather than guessing a
// custody mode.
//
// `protection` values are the server's: "server", "client", "" (no
// identity). Anything else is treated as not-server and degrades to no
// controls.
PgpComposeState pgpComposeStateOf(std::optional<bool> hasIdentity, std::optional<QString> protection);
