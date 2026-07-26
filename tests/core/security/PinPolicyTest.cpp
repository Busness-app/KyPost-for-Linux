#include "security/PinPolicy.h"

#include <QTest>

// The PIN is not just compared against a stored hash: with the credential
// gate on it is the key-encryption key for the relay device secret
// (core/security/CredentialCipher.h). An attacker with a copy of the keychain
// entry attacks it offline, where LockoutPolicy's backoff and
// wipe-after-10-failures do not exist. Before this policy the only check
// anywhere was isEmpty(), so a one-character PIN was accepted.
class PinPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void rejectsTooShort();
    void rejectsTooLong();
    void rejectsAllSameCharacter();
    void rejectsSequentialRuns();
    void acceptsOrdinaryPins();
    void isAcceptableMatchesValidate();
};

void PinPolicyTest::rejectsTooShort()
{
    QCOMPARE(PinPolicy::validate(QString()), PinPolicy::Rejection::TooShort);
    QCOMPARE(PinPolicy::validate(QStringLiteral("1")), PinPolicy::Rejection::TooShort);
    QCOMPARE(PinPolicy::validate(QStringLiteral("41927")), PinPolicy::Rejection::TooShort);
    // Exactly at the minimum is fine.
    QCOMPARE(PinPolicy::validate(QStringLiteral("419273")), PinPolicy::Rejection::Ok);
}

void PinPolicyTest::rejectsTooLong()
{
    const QString overLong(PinPolicy::kMaximumLength + 1, QLatin1Char('7'));
    // Length is checked before the all-same-character rule, so this reports
    // TooLong rather than AllSameCharacter.
    QCOMPARE(PinPolicy::validate(overLong), PinPolicy::Rejection::TooLong);
}

void PinPolicyTest::rejectsAllSameCharacter()
{
    QCOMPARE(PinPolicy::validate(QStringLiteral("111111")), PinPolicy::Rejection::AllSameCharacter);
    QCOMPARE(PinPolicy::validate(QStringLiteral("000000")), PinPolicy::Rejection::AllSameCharacter);
    QCOMPARE(PinPolicy::validate(QStringLiteral("99999999")), PinPolicy::Rejection::AllSameCharacter);
}

void PinPolicyTest::rejectsSequentialRuns()
{
    QCOMPARE(PinPolicy::validate(QStringLiteral("123456")), PinPolicy::Rejection::Sequential);
    QCOMPARE(PinPolicy::validate(QStringLiteral("654321")), PinPolicy::Rejection::Sequential);
    // Not just the classic ones: the rule is "a run", computed over the
    // whole string, so these are caught too.
    QCOMPARE(PinPolicy::validate(QStringLiteral("345678")), PinPolicy::Rejection::Sequential);
    QCOMPARE(PinPolicy::validate(QStringLiteral("98765432")), PinPolicy::Rejection::Sequential);

    // A run that breaks anywhere is not a run.
    QCOMPARE(PinPolicy::validate(QStringLiteral("123457")), PinPolicy::Rejection::Ok);
}

void PinPolicyTest::acceptsOrdinaryPins()
{
    QCOMPARE(PinPolicy::validate(QStringLiteral("419273")), PinPolicy::Rejection::Ok);
    QCOMPARE(PinPolicy::validate(QStringLiteral("860514")), PinPolicy::Rejection::Ok);
    QCOMPARE(PinPolicy::validate(QStringLiteral("1029384756")), PinPolicy::Rejection::Ok);
    // Not digits-only by contract -- the field hints at digits, but a user
    // pasting a longer passphrase must not be refused for that alone.
    QCOMPARE(PinPolicy::validate(QStringLiteral("correct horse battery")), PinPolicy::Rejection::Ok);
}

void PinPolicyTest::isAcceptableMatchesValidate()
{
    QVERIFY(PinPolicy::isAcceptable(QStringLiteral("419273")));
    QVERIFY(!PinPolicy::isAcceptable(QStringLiteral("1")));
    QVERIFY(!PinPolicy::isAcceptable(QStringLiteral("123456")));
}

QTEST_APPLESS_MAIN(PinPolicyTest)
#include "PinPolicyTest.moc"
