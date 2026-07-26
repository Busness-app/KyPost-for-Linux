#include "domain/PgpComposeState.h"

#include <QTest>

class PgpComposeStateTest : public QObject
{
    Q_OBJECT

private slots:
    void serverCustodyWithIdentityOffersBoth();
    void encryptDoesNotRequireAnIdentityButSignDoes();
    void clientCustodyHandsOffInsteadOfOfferingToggles();
    void unreachableBootstrapHidesEverything();
    void unrecognizedProtectionDegradesRatherThanGuessing();
};

void PgpComposeStateTest::serverCustodyWithIdentityOffersBoth()
{
    const PgpComposeState state = pgpComposeStateOf(true, QStringLiteral("server"));

    QCOMPARE(state.canEncrypt, true);
    QCOMPARE(state.canSign, true);
    QCOMPARE(state.handoffToWebmail, false);
}

// Encrypting uses the RECIPIENTS' public keys, so it works with no sender
// identity at all; only signing needs the account's own key. Verified against
// kypost-server server.go:1214-1223, where a missing server-readable key
// 400s only when req.Sign is set. Gating both on hasIdentity would deny
// encryption to an account that never made a key.
void PgpComposeStateTest::encryptDoesNotRequireAnIdentityButSignDoes()
{
    const PgpComposeState noIdentity = pgpComposeStateOf(false, QStringLiteral(""));

    QCOMPARE(noIdentity.canEncrypt, true);
    QCOMPARE(noIdentity.canSign, false);
    QCOMPARE(noIdentity.handoffToWebmail, false);

    // Contradictory but defensive: protection "server" implies a key exists,
    // so hasIdentity false should not happen. Signing still must not be
    // offered on the strength of the protection value alone.
    const PgpComposeState serverButNoKey = pgpComposeStateOf(false, QStringLiteral("server"));

    QCOMPARE(serverButNoKey.canEncrypt, true);
    QCOMPARE(serverButNoKey.canSign, false);
}

void PgpComposeStateTest::clientCustodyHandsOffInsteadOfOfferingToggles()
{
    const PgpComposeState state = pgpComposeStateOf(true, QStringLiteral("client"));

    QCOMPARE(state.canEncrypt, false);
    QCOMPARE(state.canSign, false);
    QCOMPARE(state.handoffToWebmail, true);
}

// Guessing "server" offers a toggle that 409s; guessing "client" sends people
// to webmail for nothing. Neither is acceptable, so an unreachable bootstrap
// shows no PGP controls and plain send keeps working.
void PgpComposeStateTest::unreachableBootstrapHidesEverything()
{
    const PgpComposeState state = pgpComposeStateOf(std::nullopt, std::nullopt);

    QCOMPARE(state.canEncrypt, false);
    QCOMPARE(state.canSign, false);
    QCOMPARE(state.handoffToWebmail, false);
}

void PgpComposeStateTest::unrecognizedProtectionDegradesRatherThanGuessing()
{
    const PgpComposeState state = pgpComposeStateOf(true, QStringLiteral("some-future-mode"));

    QCOMPARE(state.canEncrypt, false);
    QCOMPARE(state.canSign, false);
    QCOMPARE(state.handoffToWebmail, false);
}

QTEST_APPLESS_MAIN(PgpComposeStateTest)
#include "PgpComposeStateTest.moc"
