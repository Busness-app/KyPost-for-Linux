#include "pgp/EncryptedMessageReader.h"

#include "pgp/OpenPgpEncryptor.h"
#include "pgp/OpenPgpKeyImporter.h"

#include "net/HttpClient.h"
#include "net/PgpPayloadClient.h"
#include "net/RelayAuth.h"

#include "GnupgFixture.h"
#include "../net/FakeRelayServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>

namespace {

// The relay's response with real armor in it. Built through QJsonDocument
// rather than string-pasted, because armor is multi-line and hand-escaping
// it is how a fixture ends up testing the escaping instead of the reader.
QByteArray payloadResponse(const QByteArray& armored)
{
    const QJsonObject body{
        { QStringLiteral("messageId"), 5 },
        { QStringLiteral("mailbox"), QStringLiteral("INBOX") },
        { QStringLiteral("encryptedPayload"), QString::fromUtf8(armored) },
        { QStringLiteral("body"), QString() },
    };
    return httpResponse(200, "OK", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

// The same, plus the keys the relay's address book binds to this message's
// sender. The relay has already narrowed these to that sender; the client must
// not re-derive the binding.
//
// `claimedFingerprint` is what the relay says the key's fingerprint is, and is
// a separate parameter from the key itself precisely so a test can make the
// two disagree -- which is the case the read path now has to refuse. Passing
// an empty one reproduces the pre-2026-08-24 wire, where the field did not
// exist at all.
QByteArray signedPayloadResponse(const QByteArray& armored, const QByteArray& signerPublicKey,
                                  const QString& claimedFingerprint, bool conflict = false,
                                  const QString& resolvedSender = QStringLiteral("sender@example.com"))
{
    QJsonArray keys;
    if (!signerPublicKey.isEmpty()) {
        keys.append(QJsonObject{ { QStringLiteral("publicKey"), QString::fromUtf8(signerPublicKey) },
                                  { QStringLiteral("fingerprint"), claimedFingerprint },
                                  { QStringLiteral("addresses"), QJsonArray{ resolvedSender } },
                                  { QStringLiteral("conflict"), conflict } });
    }
    const QJsonObject body{
        { QStringLiteral("encryptedPayload"), QString::fromUtf8(armored) },
        { QStringLiteral("signerKeys"), keys },
        // The display form is attacker-separable from this and is deliberately
        // never parsed. It is included here so the fixture looks like a real
        // response, and so a test can prove it is ignored.
        { QStringLiteral("sender"), QStringLiteral("sender@example.com <eve@evil.example>") },
        { QStringLiteral("resolvedSender"), resolvedSender },
    };
    return httpResponse(200, "OK", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

} // namespace

class EncryptedMessageReaderTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void anEncryptedMessageComesBackAsPlaintext();
    void oneReadIsOneFetch();
    void mailForAKeyThisMachineLacksIsNotAFailedFetch();
    void garbageArmorIsMalformedRatherThanBlank();
    void aServerCustodyAccountKeepsItsOwnAnswer();
    void nothingToDecryptIsTerminalNotRetryable();
    void theRelayRefusingTheMessageIsTooLarge();
    void aPlaintextOverTheCeilingIsTheSameAnswer();
    void onlyTheRetryableStatusCarriesDetail();

    void aSignatureFromTheSendersOwnKeyIsCreditedToThem();
    void aSignatureFromTheSendersSigningSubkeyIsCreditedToThem();
    void aValidSignatureFromAKeyNobodyBindsToTheSenderIsNotCreditedToThem();
    void aSignatureFromAnAbsentKeyIsUnanswerableNotInvalid();
    void withNoKeyBoundToTheSenderNothingIsCredited();
    void aConflictingKeyIsNotUsedToVerify();
    void anUnsignedMessageIsNotReportedAsAnythingElse();
    void theVerdictIsAboutTheResolvedSenderNotTheDisplayName();

    void aSignerKeyWithNoClaimedFingerprintNeverReachesTheKeyring();
    void aSignerKeyWhoseClaimedFingerprintIsWrongNeverReachesTheKeyring();

private:
    PgpReadResult readAgainst(FakeRelayServer& fake, const OpenPgpDecryptor& decryptor,
                               const QString& gnupgHome) const;
    // A message really signed by `signer` and really encrypted to the reader.
    QByteArray signedAndEncryptedTo(const GnupgFixture& signer, const QString& signerAddress,
                                     const QByteArray& plaintext,
                                     const GnupgFixture* reader = nullptr,
                                     const QString& readerAddress = QStringLiteral("test@example.com")) const;

    GnupgFixture m_fixture;
    // A correspondent with their own key, who signs. Separate keyring, so the
    // reader has to be given their public key to check anything.
    GnupgFixture m_signer;
    // Somebody else entirely, for the forged-signature case.
    GnupgFixture m_stranger;
    // A reader whose keyring no other test writes to. The tests here share
    // m_fixture, and one of them imports the signer's key into it -- so a
    // test that needs "this keyring has never seen that key" cannot use it,
    // and would otherwise pass through a different branch than its name says.
    GnupgFixture m_freshReader;
};

void EncryptedMessageReaderTest::initTestCase()
{
    if (!OpenPgpDecryptor::engineAvailable())
        QSKIP("no usable gpg on this system -- OpenPGP reading is NOT covered");
    if (!m_fixture.build())
        QSKIP("could not build a throwaway GnuPG keyring -- OpenPGP reading is NOT covered");
    if (!m_signer.build(QStringLiteral("Sender <sender@example.com>"))
        || !m_stranger.build(QStringLiteral("Eve <eve@evil.example>"))
        || !m_freshReader.build(QStringLiteral("Fresh <fresh@example.com>"))) {
        QSKIP("could not build correspondent keyrings -- signature checks are NOT covered");
    }
}

// Signed by `signer`'s own key and encrypted to the reader's, which is what a
// real RFC 3156 combined message is. Built in the signer's keyring, so the
// reader genuinely does not have the public key until it is offered one.
QByteArray EncryptedMessageReaderTest::signedAndEncryptedTo(const GnupgFixture& signer,
                                                              const QString& signerAddress,
                                                              const QByteArray& plaintext,
                                                              const GnupgFixture* reader,
                                                              const QString& readerAddress) const
{
    const GnupgFixture& to = reader != nullptr ? *reader : m_fixture;
    // The reader's public key has to be in the signer's keyring to encrypt to.
    const QByteArray readerKey = to.exportPublicKey(readerAddress);
    if (readerKey.isEmpty())
        return {};
    const PgpImportResult imported =
        importPublicKey(readerKey, to.fingerprintOf(readerAddress), signer.path());
    if (imported.status != PgpImportStatus::Imported && imported.status != PgpImportStatus::Unchanged)
        return {};

    const PgpEncryptResult encrypted =
        signAndEncrypt(plaintext, signerAddress, { imported.fingerprint }, signer.path());
    return encrypted.status == PgpEncryptStatus::Encrypted ? encrypted.armoredCiphertext.toUtf8()
                                                            : QByteArray();
}

void EncryptedMessageReaderTest::cleanupTestCase()
{
    GnupgFixture::killAgent(m_fixture.path());
    GnupgFixture::killAgent(m_signer.path());
    GnupgFixture::killAgent(m_stranger.path());
    GnupgFixture::killAgent(m_freshReader.path());
}

PgpReadResult EncryptedMessageReaderTest::readAgainst(FakeRelayServer& fake,
                                                       const OpenPgpDecryptor& decryptor,
                                                       const QString& gnupgHome) const
{
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpPayloadClient payloads(http);
    EncryptedMessageReader reader(payloads, decryptor);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    return reader.read(serverBaseUrl, auth, QStringLiteral("INBOX"), QStringLiteral("5"), gnupgHome);
}

// End to end through the real artifacts: a real gpg keyring, real armor, a
// real socket. Nothing here is a stand-in for the thing it is testing.
void EncryptedMessageReaderTest::anEncryptedMessageComesBackAsPlaintext()
{
    const QByteArray secret = "meet me where the relay cannot read it\n";
    const QByteArray armored = m_fixture.encryptToTestKey(secret);
    QVERIFY(!armored.isEmpty());
    QVERIFY2(!armored.contains("meet me where"), "the fixture did not actually encrypt anything");

    FakeRelayServer fake(payloadResponse(armored));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QCOMPARE(result.plaintext, secret);
}

void EncryptedMessageReaderTest::oneReadIsOneFetch()
{
    const QByteArray armored = m_fixture.encryptToTestKey("once\n");
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(payloadResponse(armored));
    QCOMPARE(readAgainst(fake, OpenPgpDecryptor(), m_fixture.path()).status, PgpReadStatus::Decrypted);
    QCOMPARE(fake.connectionCount(), 1);
}

// The common real case: mail addressed to a key the user keeps on another
// machine. It must survive the trip as NoSecretKey -- "you cannot read this
// here" is actionable, and both "corrupt message" and "network problem" are
// lies that send the user somewhere useless.
void EncryptedMessageReaderTest::mailForAKeyThisMachineLacksIsNotAFailedFetch()
{
    const QByteArray armored = m_fixture.encryptToTestKey("not for this machine\n");
    QVERIFY(!armored.isEmpty());

    QTemporaryDir strangerHome;
    QVERIFY(strangerHome.isValid());

    FakeRelayServer fake(payloadResponse(armored));
    const PgpReadResult result =
        readAgainst(fake, OpenPgpDecryptor(), GnupgFixture::emptyHome(strangerHome));

    QCOMPARE(result.status, PgpReadStatus::NoSecretKey);
    QVERIFY(result.plaintext.isEmpty());

    GnupgFixture::killAgent(strangerHome.path());
}

void EncryptedMessageReaderTest::garbageArmorIsMalformedRatherThanBlank()
{
    FakeRelayServer fake(payloadResponse("-----BEGIN PGP MESSAGE-----\nnot base64 at all\n"));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Malformed);
    QVERIFY2(result.plaintext.isEmpty(), "something was handed up for a message that never decrypted");
}

// 409 is the one outcome with an instruction attached -- finish the key
// migration. Flattening it into FetchFailed would offer Retry for a
// condition that no amount of retrying resolves.
void EncryptedMessageReaderTest::aServerCustodyAccountKeepsItsOwnAnswer()
{
    FakeRelayServer fake(httpResponse(409, "Conflict",
                                       R"({"error":"migrate your key","migrationNeeded":true})"));
    QCOMPARE(readAgainst(fake, OpenPgpDecryptor(), m_fixture.path()).status,
             PgpReadStatus::ServerCustody);
}

void EncryptedMessageReaderTest::nothingToDecryptIsTerminalNotRetryable()
{
    FakeRelayServer fake(
        httpResponse(404, "Not Found", R"({"error":"message carries no OpenPGP payload"})"));
    QCOMPARE(readAgainst(fake, OpenPgpDecryptor(), m_fixture.path()).status,
             PgpReadStatus::NoCiphertext);
}

void EncryptedMessageReaderTest::theRelayRefusingTheMessageIsTooLarge()
{
    FakeRelayServer fake(httpResponse(413, "Request Entity Too Large",
                                       R"({"error":"message exceeds the maximum size"})"));
    QCOMPARE(readAgainst(fake, OpenPgpDecryptor(), m_fixture.path()).status, PgpReadStatus::TooLarge);
}

// The other mechanism, the same answer. A small ciphertext can expand
// without limit, so this one is refused as the plaintext is produced rather
// than by any bound on the wire -- but the reader is in the identical
// position either way, and so is the user.
void EncryptedMessageReaderTest::aPlaintextOverTheCeilingIsTheSameAnswer()
{
    const QByteArray armored = m_fixture.encryptToTestKey(QByteArray(256 * 1024, 'A'));
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(payloadResponse(armored));
    // Well under the plaintext and well under what a compressed 256 KB of one
    // repeated byte occupies on the wire, so this is the DECRYPTED size being
    // refused -- not the response.
    const OpenPgpDecryptor bounded(64 * 1024);
    const PgpReadResult result = readAgainst(fake, bounded, m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::TooLarge);
    QVERIFY2(result.plaintext.isEmpty(), "a partial plaintext was kept after the ceiling was hit");
}

// detail is relay-authored text. It reaches the caller only on the retryable
// status, where it explains an outage; on the terminal ones the status
// already says everything true, and passing the sentence through invites a
// UI that prints attacker-influenced text for a message we could not read.
void EncryptedMessageReaderTest::onlyTheRetryableStatusCarriesDetail()
{
    FakeRelayServer outage(httpResponse(503, "Service Unavailable", "unavailable", "text/plain"));
    const PgpReadResult failed = readAgainst(outage, OpenPgpDecryptor(), m_fixture.path());
    QCOMPARE(failed.status, PgpReadStatus::FetchFailed);
    QVERIFY2(!failed.detail.isEmpty(), "a retryable failure explained nothing");

    FakeRelayServer custody(httpResponse(409, "Conflict", R"({"error":"migrate your key"})"));
    const PgpReadResult terminal = readAgainst(custody, OpenPgpDecryptor(), m_fixture.path());
    QCOMPARE(terminal.status, PgpReadStatus::ServerCustody);
    QVERIFY2(terminal.detail.isEmpty(), "relay prose leaked out on a terminal status");
}

// The one state that may be shown as verified.
void EncryptedMessageReaderTest::aSignatureFromTheSendersOwnKeyIsCreditedToThem()
{
    const QByteArray armored =
        signedAndEncryptedTo(m_signer, QStringLiteral("sender@example.com"), "signed and sealed\n");
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(signedPayloadResponse(
        armored, m_signer.exportPublicKey(QStringLiteral("sender@example.com")),
        m_signer.fingerprintOf(QStringLiteral("sender@example.com"))));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QCOMPARE(result.signature, PgpSignatureVerdict::ValidFromSender);
    QCOMPARE(result.signedBy, QStringLiteral("sender@example.com"));
}

// The same credit, for the key layout most senders with a hardware token
// have: a certify-only [C] primary and a dedicated [S] signing subkey.
//
// gpg reports the SUBKEY's fingerprint for the signature, while the relay
// binds an address to the PRIMARY's -- so comparing those two strings finds no
// match however honest both sides are, and every such sender is accused of
// signing with a key nobody knows. The primary is what the binding is about,
// and gpg is what resolves the one to the other.
void EncryptedMessageReaderTest::aSignatureFromTheSendersSigningSubkeyIsCreditedToThem()
{
    GnupgFixture subkeySigner;
    QVERIFY(subkeySigner.buildWithSigningSubkey(QStringLiteral("Sub <sub@example.com>")));
    GnupgFixture reader;
    QVERIFY(reader.build(QStringLiteral("Subr <subr@example.com>")));

    const QByteArray armored =
        signedAndEncryptedTo(subkeySigner, QStringLiteral("sub@example.com"), "signed by a subkey\n",
                              &reader, QStringLiteral("subr@example.com"));
    QVERIFY(!armored.isEmpty());

    // What the importer reports and what the relay binds: the PRIMARY.
    const QString primary = subkeySigner.fingerprintOf(QStringLiteral("sub@example.com"));
    QVERIFY(!primary.isEmpty());

    FakeRelayServer fake(signedPayloadResponse(
        armored, subkeySigner.exportPublicKey(QStringLiteral("sub@example.com")), primary,
        /*conflict=*/false, QStringLiteral("sub@example.com")));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), reader.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QCOMPARE(result.signature, PgpSignatureVerdict::ValidFromSender);

    GnupgFixture::killAgent(subkeySigner.path());
    GnupgFixture::killAgent(reader.path());
}

// The forgery that matters, and the reason "valid" and "from this sender" are
// two different claims.
//
// Eve is somebody the user ALREADY knows -- her key is in their keyring, as it
// would be after any previous correspondence -- so gpg verifies her signature
// perfectly well. She signs a message claiming to be from sender@example.com.
// The mathematics is impeccable; the identity is not hers to claim.
//
// Note which scenario this is. With Eve's key ABSENT the answer is
// CannotCheck, because nothing can verify it at all; that is honest but it is
// not this. ValidFromUnknownKey only arises once the key is present, which is
// exactly when a client that trusted gpg's verdict alone would show a badge.
void EncryptedMessageReaderTest::aValidSignatureFromAKeyNobodyBindsToTheSenderIsNotCreditedToThem()
{
    const QByteArray armored =
        signedAndEncryptedTo(m_stranger, QStringLiteral("eve@evil.example"), "trust me\n");
    QVERIFY(!armored.isEmpty());

    // Eve is a known correspondent: her key is already in the reader's keyring.
    const QByteArray eveKey = m_stranger.exportPublicKey(QStringLiteral("eve@evil.example"));
    QVERIFY(!eveKey.isEmpty());
    const PgpImportResult knownAlready = importPublicKey(
        eveKey, m_stranger.fingerprintOf(QStringLiteral("eve@evil.example")), m_fixture.path());
    QVERIFY(knownAlready.status == PgpImportStatus::Imported
            || knownAlready.status == PgpImportStatus::Unchanged);

    // The relay offers the key it binds to sender@example.com -- the real one
    // -- while the message was signed by Eve's.
    FakeRelayServer fake(signedPayloadResponse(
        armored, m_signer.exportPublicKey(QStringLiteral("sender@example.com")),
        m_signer.fingerprintOf(QStringLiteral("sender@example.com"))));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QVERIFY2(result.signature != PgpSignatureVerdict::ValidFromSender,
             "a known contact's signature was credited to somebody else");
    QCOMPARE(result.signature, PgpSignatureVerdict::ValidFromUnknownKey);
}

// The same forgery with the signer's key absent. Reported as unanswerable
// rather than as a bad signature -- the mathematics was never run.
void EncryptedMessageReaderTest::aSignatureFromAnAbsentKeyIsUnanswerableNotInvalid()
{
    // Encrypted to the FRESH reader, whose keyring has never seen Eve -- so
    // gpg genuinely cannot check this signature, which is the branch under
    // test. m_fixture would have Eve's key from the test above.
    const QByteArray armored = signedAndEncryptedTo(m_stranger, QStringLiteral("eve@evil.example"),
                                                     "trust me\n", &m_freshReader,
                                                     QStringLiteral("fresh@example.com"));
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(signedPayloadResponse(
        armored, m_signer.exportPublicKey(QStringLiteral("sender@example.com")),
        m_signer.fingerprintOf(QStringLiteral("sender@example.com"))));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_freshReader.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QVERIFY2(result.signature != PgpSignatureVerdict::ValidFromSender,
             "an unverifiable signature was credited to the sender");
    QVERIFY2(result.signature != PgpSignatureVerdict::Invalid,
             "a signature that was never checked was reported as bad");
    QCOMPARE(result.signature, PgpSignatureVerdict::CannotCheck);
}

// The relay offers no key bound to this sender, so there is nothing to credit
// a signature to -- even when gpg itself can verify it perfectly well, which
// it can here, because an earlier test left the signer's key in this keyring.
//
// That last clause is why this test is named for the BINDING rather than for
// the missing key: it exercises the empty-bindings branch, not the
// no-public-key one. The absent-key case has its own test and its own keyring.
void EncryptedMessageReaderTest::withNoKeyBoundToTheSenderNothingIsCredited()
{
    const QByteArray armored =
        signedAndEncryptedTo(m_signer, QStringLiteral("sender@example.com"), "signed\n");
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(signedPayloadResponse(armored, {}, {}));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QVERIFY2(result.signature != PgpSignatureVerdict::Invalid,
             "an unanswerable question was reported as a bad signature");
    QCOMPARE(result.signature, PgpSignatureVerdict::CannotCheck);
}

// The relay saw more than one key claiming this address. Trying them all would
// let whichever one verified decide the answer, which is not a verdict.
void EncryptedMessageReaderTest::aConflictingKeyIsNotUsedToVerify()
{
    const QByteArray armored =
        signedAndEncryptedTo(m_signer, QStringLiteral("sender@example.com"), "signed\n");
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(signedPayloadResponse(
        armored, m_signer.exportPublicKey(QStringLiteral("sender@example.com")),
        m_signer.fingerprintOf(QStringLiteral("sender@example.com")), /*conflict=*/true));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QVERIFY2(result.signature != PgpSignatureVerdict::ValidFromSender,
             "a key the relay flagged as conflicting was used to credit the sender");
    QCOMPARE(result.signature, PgpSignatureVerdict::CannotCheck);
}

void EncryptedMessageReaderTest::anUnsignedMessageIsNotReportedAsAnythingElse()
{
    // encryptToTestKey does not sign.
    const QByteArray armored = m_fixture.encryptToTestKey("no signature here\n");
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(signedPayloadResponse(
        armored, m_signer.exportPublicKey(QStringLiteral("sender@example.com")),
        m_signer.fingerprintOf(QStringLiteral("sender@example.com"))));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QCOMPARE(result.signature, PgpSignatureVerdict::None);
}

// The fixture's `sender` field is `sender@example.com <eve@evil.example>` --
// a display name that reads like somebody else's address. Any verdict must be
// about the resolved mailbox, and this client must not even parse the other.
void EncryptedMessageReaderTest::theVerdictIsAboutTheResolvedSenderNotTheDisplayName()
{
    const QByteArray armored =
        signedAndEncryptedTo(m_signer, QStringLiteral("sender@example.com"), "signed\n");
    QVERIFY(!armored.isEmpty());

    FakeRelayServer fake(signedPayloadResponse(
        armored, m_signer.exportPublicKey(QStringLiteral("sender@example.com")),
        m_signer.fingerprintOf(QStringLiteral("sender@example.com"))));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), m_fixture.path());

    QCOMPARE(result.signedBy, QStringLiteral("sender@example.com"));
    QVERIFY2(!result.signedBy.contains(QStringLiteral("evil.example")),
             "the display form of the From header reached the verdict");
}

// The read path used to pass an empty expectedFingerprint to
// importPublicKey(), which switches its check off -- so opening any signed
// message wrote whatever public keys the relay offered into the user's
// DURABLE GnuPG keyring, unverified. These two pin the fix from the only
// angle that matters: what is in the keyring afterwards.
//
// A fresh keyring per test, because the assertion is "this key is NOT here"
// and the shared fixtures have had the signer's key imported by tests above.
void EncryptedMessageReaderTest::aSignerKeyWithNoClaimedFingerprintNeverReachesTheKeyring()
{
    GnupgFixture reader;
    QVERIFY(reader.build(QStringLiteral("Nofp <nofp@example.com>")));

    const QByteArray armored =
        signedAndEncryptedTo(m_signer, QStringLiteral("sender@example.com"), "signed\n", &reader,
                              QStringLiteral("nofp@example.com"));
    QVERIFY(!armored.isEmpty());

    const QString signerFingerprint = m_signer.fingerprintOf(QStringLiteral("sender@example.com"));
    QVERIFY(!signerFingerprint.isEmpty());
    QVERIFY2(!GnupgFixture::fingerprintsIn(reader.path()).contains(signerFingerprint),
             "the fixture already held the signer's key, so this proves nothing");

    // The pre-2026-08-24 wire: a key, and no fingerprint to check it against.
    FakeRelayServer fake(signedPayloadResponse(
        armored, m_signer.exportPublicKey(QStringLiteral("sender@example.com")), QString()));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), reader.path());

    // The message still opens. Only the badge is withheld.
    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QCOMPARE(result.signature, PgpSignatureVerdict::CannotCheck);
    QVERIFY2(!GnupgFixture::fingerprintsIn(reader.path()).contains(signerFingerprint),
             "an unchecked relay-supplied key was written to the user's keyring");
}

void EncryptedMessageReaderTest::aSignerKeyWhoseClaimedFingerprintIsWrongNeverReachesTheKeyring()
{
    GnupgFixture reader;
    QVERIFY(reader.build(QStringLiteral("Badfp <badfp@example.com>")));

    const QByteArray armored =
        signedAndEncryptedTo(m_signer, QStringLiteral("sender@example.com"), "signed\n", &reader,
                              QStringLiteral("badfp@example.com"));
    QVERIFY(!armored.isEmpty());

    const QString signerFingerprint = m_signer.fingerprintOf(QStringLiteral("sender@example.com"));
    const QString strangerFingerprint = m_stranger.fingerprintOf(QStringLiteral("eve@evil.example"));
    QVERIFY(!signerFingerprint.isEmpty() && !strangerFingerprint.isEmpty());
    QVERIFY(signerFingerprint != strangerFingerprint);

    // A relay whose key and whose claim about that key disagree -- the case
    // the send path has always refused and the read path did not.
    FakeRelayServer fake(signedPayloadResponse(
        armored, m_signer.exportPublicKey(QStringLiteral("sender@example.com")), strangerFingerprint));
    const PgpReadResult result = readAgainst(fake, OpenPgpDecryptor(), reader.path());

    QCOMPARE(result.status, PgpReadStatus::Decrypted);
    QCOMPARE(result.signature, PgpSignatureVerdict::CannotCheck);
    QVERIFY2(!GnupgFixture::fingerprintsIn(reader.path()).contains(signerFingerprint),
             "a key the relay mis-fingerprinted was written to the user's keyring anyway");
}

QTEST_GUILESS_MAIN(EncryptedMessageReaderTest)
#include "EncryptedMessageReaderTest.moc"
