#include "pgp/PgpSendPlanner.h"

#include "pgp/OpenPgpDecryptor.h"
#include "pgp/OpenPgpKeyImporter.h"

#include "GnupgFixture.h"

#include <QTest>

namespace {

OutgoingMessage sampleMessage()
{
    OutgoingMessage message;
    message.from = QStringLiteral("sender@example.com");
    message.subject = QStringLiteral("Quarterly numbers");
    message.body = QStringLiteral("The real body.");
    message.mode = QStringLiteral("plain");
    message.date = QStringLiteral("Sat, 23 Aug 2026 12:00:00 +0000");
    return message;
}

// The plaintext a delivery yields when its intended reader opens it. Empty
// when they cannot.
QByteArray readAs(const PgpDelivery& delivery, const QString& home)
{
    // The armor sits between the RFC 3156 markers; the decryptor takes it
    // verbatim, exactly as the read path does.
    const qsizetype begin = delivery.message.indexOf("-----BEGIN PGP MESSAGE-----");
    const qsizetype end = delivery.message.indexOf("-----END PGP MESSAGE-----");
    if (begin < 0 || end < 0)
        return {};
    const QByteArray armor =
        delivery.message.mid(begin, end - begin + qstrlen("-----END PGP MESSAGE-----"));
    return OpenPgpDecryptor().decrypt(armor, home).plaintext;
}

} // namespace

class PgpSendPlannerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void toAndCcShareOneDelivery();
    void everyBccRecipientGetsTheirOwnCiphertext();
    void aBccRecipientIsNotNamedInAnybodyElsesDelivery();
    void aBccRecipientSeesTheSameVisibleHeadersAsEveryoneElse();
    void theSentCopyOpensWithTheSendersOwnKey();
    void withNoKeyOfTheirOwnTheSenderIsToldRatherThanGivenACleartextCopy();
    void aRecipientWithNoKeyRefusesTheWholeSend();
    void nothingIsBuiltWhenOneRecipientOfManyIsMissingAKey();
    void addressesMatchCaseInsensitively();
    void noRecipientsIsNotAPlan();

private:
    GnupgFixture m_sender;
    GnupgFixture m_alice;
    GnupgFixture m_bob;
    GnupgFixture m_carol;
    QHash<QString, QString> m_fingerprints;
    QString m_senderFingerprint;

    QString bring(const GnupgFixture& from, const QString& uid);
};

QString PgpSendPlannerTest::bring(const GnupgFixture& from, const QString& uid)
{
    const QByteArray armored = from.exportPublicKey(uid);
    if (armored.isEmpty())
        return {};
    const PgpImportResult result = importPublicKey(armored, from.fingerprintOf(uid), m_sender.path());
    return (result.status == PgpImportStatus::Imported || result.status == PgpImportStatus::Unchanged)
        ? result.fingerprint
        : QString();
}

void PgpSendPlannerTest::initTestCase()
{
    if (!OpenPgpDecryptor::engineAvailable())
        QSKIP("no usable gpg on this system -- send planning is NOT covered");
    if (!m_sender.build(QStringLiteral("Sender <sender@example.com>"))
        || !m_alice.build(QStringLiteral("Alice <alice@example.com>"))
        || !m_bob.build(QStringLiteral("Bob <bob@example.com>"))
        || !m_carol.build(QStringLiteral("Carol <carol@example.com>"))) {
        QSKIP("could not build throwaway GnuPG keyrings -- send planning is NOT covered");
    }

    m_fingerprints[QStringLiteral("alice@example.com")] =
        bring(m_alice, QStringLiteral("alice@example.com"));
    m_fingerprints[QStringLiteral("bob@example.com")] = bring(m_bob, QStringLiteral("bob@example.com"));
    m_fingerprints[QStringLiteral("carol@example.com")] =
        bring(m_carol, QStringLiteral("carol@example.com"));
    m_senderFingerprint = m_sender.fingerprintOf(QStringLiteral("sender@example.com"));

    for (const QString& fingerprint : m_fingerprints)
        QVERIFY(!fingerprint.isEmpty());
    QVERIFY(!m_senderFingerprint.isEmpty());
}

void PgpSendPlannerTest::cleanupTestCase()
{
    for (const GnupgFixture* fixture : { &m_sender, &m_alice, &m_bob, &m_carol })
        GnupgFixture::killAgent(fixture->path());
}

void PgpSendPlannerTest::toAndCcShareOneDelivery()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("alice@example.com") };
    message.cc = { QStringLiteral("bob@example.com") };

    const PgpSendPlan plan =
        buildPgpSendPlan(message, {}, m_fingerprints, m_senderFingerprint, m_sender.path());

    QCOMPARE(plan.status, PgpSendPlanStatus::Built);
    QCOMPARE(plan.deliveries.size(), 1);
    QCOMPARE(plan.deliveries.at(0).smtpRecipients,
             QStringList({ QStringLiteral("alice@example.com"), QStringLiteral("bob@example.com") }));
    // Both can actually open it -- the recipient list is not the evidence.
    QVERIFY(readAs(plan.deliveries.at(0), m_alice.path()).contains("The real body."));
    QVERIFY(readAs(plan.deliveries.at(0), m_bob.path()).contains("The real body."));
}

void PgpSendPlannerTest::everyBccRecipientGetsTheirOwnCiphertext()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("alice@example.com") };

    const PgpSendPlan plan = buildPgpSendPlan(
        message, { QStringLiteral("bob@example.com"), QStringLiteral("carol@example.com") },
        m_fingerprints, m_senderFingerprint, m_sender.path());

    QCOMPARE(plan.status, PgpSendPlanStatus::Built);
    QCOMPARE(plan.deliveries.size(), 3); // one for To/Cc, one per Bcc
    QCOMPARE(plan.deliveries.at(1).smtpRecipients, QStringList({ QStringLiteral("bob@example.com") }));
    QCOMPARE(plan.deliveries.at(2).smtpRecipients, QStringList({ QStringLiteral("carol@example.com") }));

    QVERIFY(readAs(plan.deliveries.at(1), m_bob.path()).contains("The real body."));
    QVERIFY(readAs(plan.deliveries.at(2), m_carol.path()).contains("The real body."));
}

// The reason the wire format is a LIST rather than one message. A single
// ciphertext encrypted to everyone names every Bcc recipient in its key IDs,
// which is precisely the disclosure Bcc exists to prevent.
void PgpSendPlannerTest::aBccRecipientIsNotNamedInAnybodyElsesDelivery()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("alice@example.com") };

    const PgpSendPlan plan =
        buildPgpSendPlan(message, { QStringLiteral("bob@example.com") }, m_fingerprints,
                          m_senderFingerprint, m_sender.path());
    QCOMPARE(plan.status, PgpSendPlanStatus::Built);

    // Bob must not be able to open the delivery meant for Alice...
    QVERIFY2(readAs(plan.deliveries.at(0), m_bob.path()).isEmpty(),
             "the blind recipient could open the To/Cc delivery, so his key is in it");
    // ...and Alice must not be able to open Bob's.
    QVERIFY2(readAs(plan.deliveries.at(1), m_alice.path()).isEmpty(),
             "a visible recipient could open the blind delivery");
    // Nor may his address appear anywhere in the visible delivery's bytes.
    QVERIFY2(!plan.deliveries.at(0).message.contains("bob@example.com"),
             "the blind recipient's address is in the delivery everyone else receives");
}

void PgpSendPlannerTest::aBccRecipientSeesTheSameVisibleHeadersAsEveryoneElse()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("alice@example.com") };
    message.cc = { QStringLiteral("carol@example.com") };

    const PgpSendPlan plan =
        buildPgpSendPlan(message, { QStringLiteral("bob@example.com") }, m_fingerprints,
                          m_senderFingerprint, m_sender.path());
    QCOMPARE(plan.status, PgpSendPlanStatus::Built);

    const QByteArray blind = plan.deliveries.at(1).message;
    QVERIFY2(blind.contains("To: alice@example.com"), "the blind recipient cannot see the To line");
    QVERIFY2(blind.contains("Cc: carol@example.com"), "the blind recipient cannot see the Cc line");
    // And still no Bcc header anywhere -- the relay refuses one outright.
    QVERIFY2(!blind.contains("\r\nBcc:"), "a Bcc header reached a delivery");
}

void PgpSendPlannerTest::theSentCopyOpensWithTheSendersOwnKey()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("alice@example.com") };

    const PgpSendPlan plan =
        buildPgpSendPlan(message, {}, m_fingerprints, m_senderFingerprint, m_sender.path());

    QCOMPARE(plan.status, PgpSendPlanStatus::Built);
    QVERIFY(!plan.sentCopy.isEmpty());
    QVERIFY(!plan.sentCopyUnavailable);

    PgpDelivery asDelivery;
    asDelivery.message = plan.sentCopy;
    QVERIFY2(readAs(asDelivery, m_sender.path()).contains("The real body."),
             "the sender cannot read their own Sent copy");
    // And the real subject is not on the outside of it either.
    QVERIFY2(!plan.sentCopy.contains("Quarterly numbers"),
             "the Sent copy leaks the real subject to the IMAP host");
}

// The copy is APPENDed to the account's IMAP host, which is somebody else's
// machine holding no key. A readable copy there would put the body and the
// real subject of every encrypted message in the clear.
void PgpSendPlannerTest::withNoKeyOfTheirOwnTheSenderIsToldRatherThanGivenACleartextCopy()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("alice@example.com") };

    const PgpSendPlan plan =
        buildPgpSendPlan(message, {}, m_fingerprints, QString(), m_sender.path());

    // The message still goes out.
    QCOMPARE(plan.status, PgpSendPlanStatus::Built);
    QCOMPARE(plan.deliveries.size(), 1);
    // But there is no copy, and the caller is told so rather than left to
    // notice an empty field.
    QVERIFY(plan.sentCopy.isEmpty());
    QVERIFY2(plan.sentCopyUnavailable, "losing the Sent copy was silent");
}

void PgpSendPlannerTest::aRecipientWithNoKeyRefusesTheWholeSend()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("stranger@example.com") };

    const PgpSendPlan plan =
        buildPgpSendPlan(message, {}, m_fingerprints, m_senderFingerprint, m_sender.path());

    QCOMPARE(plan.status, PgpSendPlanStatus::RecipientWithoutKey);
    QVERIFY(plan.deliveries.isEmpty());
    QVERIFY2(plan.recipientsWithoutKeys.contains(QStringLiteral("stranger@example.com")),
             "the user is not told which recipient has no key");
}

// Nothing may be built for the reachable recipients while one is unreachable:
// a plan with four of five deliveries is one somebody will be tempted to send.
void PgpSendPlannerTest::nothingIsBuiltWhenOneRecipientOfManyIsMissingAKey()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("alice@example.com"), QStringLiteral("bob@example.com") };

    const PgpSendPlan plan =
        buildPgpSendPlan(message, { QStringLiteral("stranger@example.com") }, m_fingerprints,
                          m_senderFingerprint, m_sender.path());

    QCOMPARE(plan.status, PgpSendPlanStatus::RecipientWithoutKey);
    QVERIFY2(plan.deliveries.isEmpty(), "deliveries were built for a send that cannot go out");
    QVERIFY(plan.sentCopy.isEmpty());
}

// A user typing "Alice@Example.com" and a relay answering "alice@example.com"
// are the same recipient. A lookup miss here would read to the user as "no key
// on file" and refuse a send that is perfectly possible.
void PgpSendPlannerTest::addressesMatchCaseInsensitively()
{
    OutgoingMessage message = sampleMessage();
    message.to = { QStringLiteral("Alice@Example.COM") };

    const PgpSendPlan plan =
        buildPgpSendPlan(message, {}, m_fingerprints, m_senderFingerprint, m_sender.path());

    QCOMPARE(plan.status, PgpSendPlanStatus::Built);
    QVERIFY(readAs(plan.deliveries.at(0), m_alice.path()).contains("The real body."));
}

void PgpSendPlannerTest::noRecipientsIsNotAPlan()
{
    const PgpSendPlan plan =
        buildPgpSendPlan(sampleMessage(), {}, m_fingerprints, m_senderFingerprint, m_sender.path());

    QVERIFY(plan.status != PgpSendPlanStatus::Built);
    QVERIFY(plan.deliveries.isEmpty());
}

QTEST_GUILESS_MAIN(PgpSendPlannerTest)
#include "PgpSendPlannerTest.moc"
