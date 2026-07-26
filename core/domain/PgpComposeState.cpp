#include "domain/PgpComposeState.h"

PgpComposeState pgpComposeStateOf(std::optional<bool> hasIdentity, std::optional<QString> protection)
{
    PgpComposeState state;
    if (!protection.has_value())
        return state;

    const QString mode = *protection;
    if (mode == QStringLiteral("client")) {
        state.handoffToWebmail = true;
        return state;
    }
    if (mode != QStringLiteral("server") && !mode.isEmpty())
        return state;

    // "server" or "" (no identity): encryption targets the recipients' keys
    // either way, so it is always available. Signing needs this account's own
    // key, which only "server" plus a confirmed identity provides.
    state.canEncrypt = true;
    state.canSign = mode == QStringLiteral("server") && hasIdentity.value_or(false);
    return state;
}
