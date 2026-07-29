#include "security/CredentialCipher.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QSet>
#include <QTest>

#include <openssl/evp.h>

namespace {

// Builds a blob in the PRE-Argon2id layout: salt(16) || iv(12) ||
// ciphertext || tag(16), keyed by PBKDF2-HMAC-SHA256 at 150k iterations,
// with no version header.
//
// Hand-rolled here rather than kept behind a legacy code path in production,
// because the only thing that still needs to WRITE this format is a test.
// Real installs already have such blobs sitting in their keychain, and the
// upgrade must not lock them out of their own pairing -- which is a claim
// only a real old-format blob can check.
QString sealLegacyPbkdf2(const QString& pin, const QByteArray& plaintext, const QByteArray& salt,
                          const QByteArray& iv)
{
    const QByteArray key = QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, pin.toUtf8(), salt, CredentialCipher::kPbkdf2Iterations,
        CredentialCipher::kKeyBytes);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CredentialCipher::kIvBytes, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.constData()),
                        reinterpret_cast<const unsigned char*>(iv.constData()));

    QByteArray ciphertext(plaintext.size() + 32, Qt::Uninitialized);
    int len = 0;
    EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()), &len,
                       reinterpret_cast<const unsigned char*>(plaintext.constData()),
                       static_cast<int>(plaintext.size()));
    int total = len;
    EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + len, &len);
    total += len;
    ciphertext.resize(total);

    QByteArray tag(CredentialCipher::kTagBytes, Qt::Uninitialized);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CredentialCipher::kTagBytes, tag.data());
    EVP_CIPHER_CTX_free(ctx);

    return QString::fromLatin1((salt + iv + ciphertext + tag).toBase64());
}

} // namespace

class CredentialCipherTest : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsUnderTheSamePin();
    void wrongPinYieldsNothing();
    void tamperedCiphertextFailsTheTag();
    void sealIsNonDeterministic();
    void rejectsTruncatedAndGarbageBlobs();
    void emptyPlaintextRoundTrips();

    // KDF migration.
    void newBlobsCarryTheArgon2idMarker();
    void legacyPbkdf2BlobsStillOpen();
    void legacyBlobsAreResealedInTheirOwnFormat();
    void aWrongPinOnALegacyBlobIsStillJustAFailure();
};

void CredentialCipherTest::roundTripsUnderTheSamePin()
{
    const QByteArray secret = "device-secret-abc123";
    const std::optional<QString> sealed = CredentialCipher::seal(QStringLiteral("123456"), secret);
    QVERIFY(sealed.has_value());
    // The secret must not be recoverable by reading the stored string.
    QVERIFY(!sealed->toUtf8().contains(secret));

    const std::optional<QByteArray> opened = CredentialCipher::open(QStringLiteral("123456"), *sealed);
    QVERIFY(opened.has_value());
    QCOMPARE(*opened, secret);
}

void CredentialCipherTest::wrongPinYieldsNothing()
{
    const std::optional<QString> sealed =
        CredentialCipher::seal(QStringLiteral("123456"), QByteArray("secret"));
    QVERIFY(sealed.has_value());

    QVERIFY(!CredentialCipher::open(QStringLiteral("123457"), *sealed).has_value());
    QVERIFY(!CredentialCipher::open(QString(), *sealed).has_value());
    QVERIFY(!CredentialCipher::open(QStringLiteral("1234567"), *sealed).has_value());
}

void CredentialCipherTest::tamperedCiphertextFailsTheTag()
{
    const std::optional<QString> sealed =
        CredentialCipher::seal(QStringLiteral("123456"), QByteArray("secret-value"));
    QVERIFY(sealed.has_value());

    QByteArray blob = QByteArray::fromBase64(sealed->toLatin1());

    // Flip a bit in the ciphertext region (past salt+iv, before the tag).
    QByteArray flippedCipher = blob;
    const int cipherOffset = CredentialCipher::kSaltBytes + CredentialCipher::kIvBytes;
    flippedCipher[cipherOffset] = flippedCipher[cipherOffset] ^ 0x01;
    QVERIFY(!CredentialCipher::open(QStringLiteral("123456"),
                                     QString::fromLatin1(flippedCipher.toBase64()))
                 .has_value());

    // Flip a bit in the tag itself.
    QByteArray flippedTag = blob;
    flippedTag[flippedTag.size() - 1] = flippedTag[flippedTag.size() - 1] ^ 0x01;
    QVERIFY(!CredentialCipher::open(QStringLiteral("123456"),
                                     QString::fromLatin1(flippedTag.toBase64()))
                 .has_value());

    // Flip a bit in the salt: the derived key changes, so this must fail too
    // rather than decrypt to garbage.
    QByteArray flippedSalt = blob;
    flippedSalt[0] = flippedSalt[0] ^ 0x01;
    QVERIFY(!CredentialCipher::open(QStringLiteral("123456"),
                                     QString::fromLatin1(flippedSalt.toBase64()))
                 .has_value());
}

void CredentialCipherTest::sealIsNonDeterministic()
{
    // A fresh salt and IV every time: sealing the same secret twice must not
    // produce the same blob, or an observer could tell that the stored
    // credential had not changed.
    QSet<QString> blobs;
    for (int i = 0; i < 5; ++i) {
        const std::optional<QString> sealed =
            CredentialCipher::seal(QStringLiteral("123456"), QByteArray("same-secret"));
        QVERIFY(sealed.has_value());
        blobs.insert(*sealed);
    }
    QCOMPARE(blobs.size(), 5);
}

void CredentialCipherTest::rejectsTruncatedAndGarbageBlobs()
{
    QVERIFY(!CredentialCipher::open(QStringLiteral("123456"), QString()).has_value());
    QVERIFY(!CredentialCipher::open(QStringLiteral("123456"), QStringLiteral("not-base64!!")).has_value());
    // Shorter than salt+iv+tag can possibly be.
    QVERIFY(!CredentialCipher::open(QStringLiteral("123456"),
                                     QString::fromLatin1(QByteArray(8, 'x').toBase64()))
                 .has_value());
}

void CredentialCipherTest::emptyPlaintextRoundTrips()
{
    // Legal, and must not be confused with failure.
    const std::optional<QString> sealed = CredentialCipher::seal(QStringLiteral("123456"), QByteArray());
    QVERIFY(sealed.has_value());
    const std::optional<QByteArray> opened = CredentialCipher::open(QStringLiteral("123456"), *sealed);
    QVERIFY(opened.has_value());
    QVERIFY(opened->isEmpty());
}

// Everything sealed from now on is Argon2id, and says so.
//
// The PIN is six digits by policy, so this blob's whole keyspace is 10^6.
// Under PBKDF2-150k that is ~1.5e11 HMACs with a tiny working set -- one
// consumer GPU, hours. Argon2id at 64 MiB per guess caps parallelism at
// however many spare gigabytes the attacker's card has, which is the
// property PBKDF2 never had and that raising its iteration count cannot buy.
void CredentialCipherTest::newBlobsCarryTheArgon2idMarker()
{
    const std::optional<QString> sealed =
        CredentialCipher::seal(QStringLiteral("419273"), QByteArray("device-secret"));
    QVERIFY(sealed.has_value());

    const QByteArray raw = QByteArray::fromBase64(sealed->toLatin1());
    QVERIFY(raw.startsWith(QByteArray(CredentialCipher::kMagicArgon2id, CredentialCipher::kMagicBytes)));

    const std::optional<QByteArray> opened = CredentialCipher::open(QStringLiteral("419273"), *sealed);
    QVERIFY(opened.has_value());
    QCOMPARE(*opened, QByteArray("device-secret"));
}

// The upgrade must not lock existing users out of their own pairing.
//
// A blob written by any shipped build has no version header and is PBKDF2.
// The device secret inside it cannot be re-derived from anything else -- the
// relay minted it once and will not repeat it -- so failing to open it means
// the pairing is gone and the user must re-pair from scratch.
void CredentialCipherTest::legacyPbkdf2BlobsStillOpen()
{
    const QByteArray secret = "device-secret-from-an-older-build";
    const QByteArray salt(CredentialCipher::kSaltBytes, 'S');
    const QByteArray iv(CredentialCipher::kIvBytes, 'I');
    const QString legacy = sealLegacyPbkdf2(QStringLiteral("419273"), secret, salt, iv);

    // Precondition: this really is the old layout, with no marker.
    const QByteArray raw = QByteArray::fromBase64(legacy.toLatin1());
    QVERIFY(!raw.startsWith(QByteArray(CredentialCipher::kMagicArgon2id, CredentialCipher::kMagicBytes)));

    const std::optional<QByteArray> opened = CredentialCipher::open(QStringLiteral("419273"), legacy);
    QVERIFY(opened.has_value());
    QCOMPARE(*opened, secret);
}

// A session key that came from a legacy blob must re-seal in the legacy
// layout.
//
// openWithKey() hands back the derived key so a rotated device secret can be
// re-wrapped without re-prompting for the PIN. If that key were written back
// under an Argon2id header, the blob would advertise a KDF that cannot
// reproduce the key inside it, and NOTHING would open it again -- the PIN
// included. That is an unrecoverable pairing, produced by an upgrade, on the
// perfectly ordinary path where the relay rotates the secret.
void CredentialCipherTest::legacyBlobsAreResealedInTheirOwnFormat()
{
    const QByteArray salt(CredentialCipher::kSaltBytes, 'S');
    const QByteArray iv(CredentialCipher::kIvBytes, 'I');
    const QString legacy = sealLegacyPbkdf2(QStringLiteral("419273"), QByteArray("original"), salt, iv);

    const auto opened = CredentialCipher::openWithKey(QStringLiteral("419273"), legacy);
    QVERIFY(opened.has_value());
    QVERIFY(opened->second.legacyPbkdf2);

    const std::optional<QString> resealed =
        CredentialCipher::sealWithKey(opened->second, QByteArray("rotated"));
    QVERIFY(resealed.has_value());

    // Still the old layout...
    const QByteArray raw = QByteArray::fromBase64(resealed->toLatin1());
    QVERIFY(!raw.startsWith(QByteArray(CredentialCipher::kMagicArgon2id, CredentialCipher::kMagicBytes)));
    // ...and the PIN still opens it, carrying the new value.
    const std::optional<QByteArray> reopened = CredentialCipher::open(QStringLiteral("419273"), *resealed);
    QVERIFY(reopened.has_value());
    QCOMPARE(*reopened, QByteArray("rotated"));
}

// Format detection must not become an oracle. A wrong PIN on a legacy blob
// has to look exactly like a wrong PIN on a new one, and like a tampered
// blob: std::nullopt, no distinguishing signal.
void CredentialCipherTest::aWrongPinOnALegacyBlobIsStillJustAFailure()
{
    const QByteArray salt(CredentialCipher::kSaltBytes, 'S');
    const QByteArray iv(CredentialCipher::kIvBytes, 'I');
    const QString legacy = sealLegacyPbkdf2(QStringLiteral("419273"), QByteArray("secret"), salt, iv);

    QVERIFY(!CredentialCipher::open(QStringLiteral("860514"), legacy).has_value());

    // And a new-format blob that has had its marker stripped is treated as
    // legacy, derives the wrong key, and fails the tag -- rather than being
    // silently opened by the wrong KDF path.
    const std::optional<QString> modern =
        CredentialCipher::seal(QStringLiteral("419273"), QByteArray("secret"));
    QVERIFY(modern.has_value());
    const QByteArray stripped = QByteArray::fromBase64(modern->toLatin1()).mid(CredentialCipher::kMagicBytes);
    QVERIFY(!CredentialCipher::open(QStringLiteral("419273"), QString::fromLatin1(stripped.toBase64()))
                 .has_value());
}

QTEST_APPLESS_MAIN(CredentialCipherTest)
#include "CredentialCipherTest.moc"
