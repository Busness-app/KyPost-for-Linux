#pragma once

#include "security/SecureBytes.h"

#include <QByteArray>
#include <QString>

typedef struct evp_pkey_st EVP_PKEY;

class DeviceEnrollmentCrypto
{
public:
    DeviceEnrollmentCrypto() = default;
    ~DeviceEnrollmentCrypto();
    DeviceEnrollmentCrypto(const DeviceEnrollmentCrypto&) = delete;
    DeviceEnrollmentCrypto& operator=(const DeviceEnrollmentCrypto&) = delete;

    bool generate();
    bool isReady() const { return m_key != nullptr && m_publicKey.size() == 65; }
    QByteArray publicKeyBase64() const { return m_publicKey.toBase64(); }
    QString verificationCode(const QString& deviceId, qint64 bucket) const;
    SecureBytes openEnvelope(const QByteArray& envelopeJson, const QString& deviceId,
                             const QString& fingerprint) const;
    void clear();

private:
    EVP_PKEY* m_key = nullptr;
    QByteArray m_publicKey;
};

QByteArray deviceEnvelopeAad(const QString& deviceId, const QString& fingerprint);
QString deviceEnrollmentCode(const QByteArray& publicKey, const QString& deviceId, qint64 bucket);
QString formatEnrollmentCode(const QString& code);
