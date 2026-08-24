#include "pgp/DeviceEnrollmentCrypto.h"

#include <QTest>

#include <algorithm>

class DeviceEnrollmentCryptoTest : public QObject
{
    Q_OBJECT

private slots:
    void matchesTheBrowserVerificationVector();
    void formatsTheCodeForReading();
    void generatesAValidEphemeralPublicPoint();
    void bindsTheEnvelopeToDeviceAndFingerprint();
    void rejectsMalformedEnvelopes();
};

void DeviceEnrollmentCryptoTest::matchesTheBrowserVerificationVector()
{
    QByteArray publicKey(65, '\x02');
    publicKey[0] = '\x04';
    std::fill(publicKey.begin() + 1, publicKey.begin() + 33, '\x01');
    QCOMPARE(deviceEnrollmentCode(publicKey, QStringLiteral("test-device"), 14000000),
             QStringLiteral("5R9K6FWA18A8YP"));
}

void DeviceEnrollmentCryptoTest::formatsTheCodeForReading()
{
    QCOMPARE(formatEnrollmentCode(QStringLiteral("5R9K6FWA18A8YP")),
             QStringLiteral("5R9K-6FW-A18A-8YP"));
}

void DeviceEnrollmentCryptoTest::generatesAValidEphemeralPublicPoint()
{
    DeviceEnrollmentCrypto crypto;
    QVERIFY(crypto.generate());
    const QByteArray point = QByteArray::fromBase64(crypto.publicKeyBase64());
    QCOMPARE(point.size(), 65);
    QCOMPARE(point.front(), '\x04');
    QVERIFY(!crypto.verificationCode(QStringLiteral("device-123"), 123456).isEmpty());
    crypto.clear();
    QVERIFY(!crypto.isReady());
}

void DeviceEnrollmentCryptoTest::bindsTheEnvelopeToDeviceAndFingerprint()
{
    const QByteArray aad = deviceEnvelopeAad(
        QStringLiteral("device-123"), QStringLiteral("aa bb cc dd"));
    QVERIFY(aad.startsWith("kypost-device-envelope/v2"));
    QVERIFY(aad.endsWith("AABBCCDD"));
    QVERIFY(deviceEnvelopeAad(QStringLiteral("device-123"), QStringLiteral("not hex")).isEmpty());
}

void DeviceEnrollmentCryptoTest::rejectsMalformedEnvelopes()
{
    DeviceEnrollmentCrypto crypto;
    QVERIFY(crypto.generate());
    QVERIFY(crypto.openEnvelope("{}", QStringLiteral("device-123"),
                                QStringLiteral("AABBCCDD")).isEmpty());
}

QTEST_GUILESS_MAIN(DeviceEnrollmentCryptoTest)
#include "DeviceEnrollmentCryptoTest.moc"
