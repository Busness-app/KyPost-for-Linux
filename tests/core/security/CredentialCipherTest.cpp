#include "security/CredentialCipher.h"

#include <QSet>
#include <QTest>

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

QTEST_APPLESS_MAIN(CredentialCipherTest)
#include "CredentialCipherTest.moc"
