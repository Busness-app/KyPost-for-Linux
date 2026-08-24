#include "pgp/DeviceEnrollmentCrypto.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <algorithm>
#include <memory>

namespace {
constexpr auto kInfo = "kypost-device-envelope/v2";
constexpr auto kAlgorithm = "ECDH-P256+HKDF-SHA256+A256GCM";
constexpr auto kCrockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr int kMaxEnvelopeBytes = 1024 * 1024;

QByteArray strictBase64(const QJsonValue& value)
{
    if (!value.isString())
        return {};
    const auto decoded = QByteArray::fromBase64Encoding(value.toString().toLatin1(),
                                                         QByteArray::AbortOnBase64DecodingErrors);
    return decoded.decodingStatus == QByteArray::Base64DecodingStatus::Ok ? decoded.decoded : QByteArray();
}

EVP_PKEY* publicKeyFromPoint(const QByteArray& point)
{
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), &EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_fromdata_init(context.get()) <= 0)
        return nullptr;
    char group[] = "prime256v1";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                           const_cast<char*>(point.constData()), point.size()),
        OSSL_PARAM_construct_end()
    };
    EVP_PKEY* key = nullptr;
    return EVP_PKEY_fromdata(context.get(), &key, EVP_PKEY_PUBLIC_KEY, parameters) > 0 ? key : nullptr;
}

SecureBytes hkdf(const SecureBytes& sharedSecret, const QByteArray& salt)
{
    SecureBytes key(QByteArray(32, '\0'));
    std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)> algorithm(EVP_KDF_fetch(nullptr, "HKDF", nullptr),
                                                               &EVP_KDF_free);
    std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)> context(
        algorithm ? EVP_KDF_CTX_new(algorithm.get()) : nullptr, &EVP_KDF_CTX_free);
    if (!context)
        return {};
    char digest[] = "SHA256";
    QByteArray info(kInfo);
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
            const_cast<char*>(sharedSecret.bytes().constData()), sharedSecret.bytes().size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
            const_cast<char*>(salt.constData()), salt.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info.data(), info.size()),
        OSSL_PARAM_construct_end()
    };
    if (EVP_KDF_derive(context.get(), reinterpret_cast<unsigned char*>(key.bytes().data()),
                       key.bytes().size(), parameters) <= 0)
        return {};
    return key;
}
}

DeviceEnrollmentCrypto::~DeviceEnrollmentCrypto()
{
    clear();
}

void DeviceEnrollmentCrypto::clear()
{
    if (m_key != nullptr)
        EVP_PKEY_free(m_key);
    m_key = nullptr;
    m_publicKey.clear();
}

bool DeviceEnrollmentCrypto::generate()
{
    clear();
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), &EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0)
        return false;
    char group[] = "prime256v1";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_end()
    };
    if (EVP_PKEY_CTX_set_params(context.get(), parameters) <= 0
        || EVP_PKEY_generate(context.get(), &m_key) <= 0)
        return false;
    size_t size = 0;
    if (EVP_PKEY_get_octet_string_param(m_key, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &size) <= 0
        || size != 65) {
        clear();
        return false;
    }
    m_publicKey.resize(static_cast<qsizetype>(size));
    if (EVP_PKEY_get_octet_string_param(m_key, OSSL_PKEY_PARAM_PUB_KEY,
                                         reinterpret_cast<unsigned char*>(m_publicKey.data()),
                                         size, &size) <= 0
        || static_cast<qsizetype>(size) != m_publicKey.size() || m_publicKey.front() != '\x04') {
        clear();
        return false;
    }
    return true;
}

QString DeviceEnrollmentCrypto::verificationCode(const QString& deviceId, qint64 bucket) const
{
    if (!isReady())
        return {};
    return deviceEnrollmentCode(m_publicKey, deviceId, bucket);
}

QString deviceEnrollmentCode(const QByteArray& publicKey, const QString& deviceId, qint64 bucket)
{
    if (publicKey.size() != 65 || publicKey.front() != '\x04')
        return {};
    const QByteArray id = deviceId.toUtf8();
    if (id.size() > 0xffff)
        return {};
    QByteArray preimage = publicKey;
    preimage.append(static_cast<char>((id.size() >> 8) & 0xff));
    preimage.append(static_cast<char>(id.size() & 0xff));
    preimage.append(id);
    for (int shift = 56; shift >= 0; shift -= 8)
        preimage.append(static_cast<char>((static_cast<quint64>(bucket) >> shift) & 0xff));
    unsigned char digest[32];
    unsigned int digestSize = 0;
    if (EVP_Digest(preimage.constData(), preimage.size(), digest, &digestSize, EVP_sha256(), nullptr) != 1
        || digestSize != sizeof(digest))
        return {};
    QString code;
    code.reserve(14);
    for (int character = 0; character < 14; ++character) {
        int value = 0;
        for (int offset = 0; offset < 5; ++offset) {
            const int bit = character * 5 + offset;
            value = (value << 1) | ((digest[bit / 8] >> (7 - bit % 8)) & 1);
        }
        code.append(QLatin1Char(kCrockford[value]));
    }
    return code;
}

QString formatEnrollmentCode(const QString& code)
{
    if (code.size() != 14)
        return code;
    return code.left(4) + QLatin1Char('-') + code.mid(4, 3) + QLatin1Char('-')
        + code.mid(7, 4) + QLatin1Char('-') + code.right(3);
}

QByteArray deviceEnvelopeAad(const QString& deviceId, const QString& fingerprint)
{
    const QByteArray id = deviceId.toUtf8();
    QString normalized = fingerprint.toUpper();
    normalized.removeIf([](QChar c) { return c.isSpace(); });
    const QByteArray fp = normalized.toLatin1();
    if (id.size() > 0xffff || fp.isEmpty() || fp.size() > 0xffff
        || !std::all_of(fp.cbegin(), fp.cend(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        }))
        return {};
    QByteArray aad(kInfo);
    aad.append(static_cast<char>((id.size() >> 8) & 0xff));
    aad.append(static_cast<char>(id.size() & 0xff));
    aad.append(id);
    aad.append(static_cast<char>((fp.size() >> 8) & 0xff));
    aad.append(static_cast<char>(fp.size() & 0xff));
    aad.append(fp);
    return aad;
}

SecureBytes DeviceEnrollmentCrypto::openEnvelope(const QByteArray& envelopeJson, const QString& deviceId,
                                                  const QString& fingerprint) const
{
    if (!isReady() || envelopeJson.isEmpty() || envelopeJson.size() > kMaxEnvelopeBytes)
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(envelopeJson);
    if (!document.isObject())
        return {};
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("v")).toInt() != 2
        || object.value(QStringLiteral("alg")).toString() != QString::fromLatin1(kAlgorithm))
        return {};
    const QByteArray peerPoint = strictBase64(object.value(QStringLiteral("epk")));
    const QByteArray iv = strictBase64(object.value(QStringLiteral("iv")));
    const QByteArray ciphertextAndTag = strictBase64(object.value(QStringLiteral("ct")));
    const QByteArray aad = deviceEnvelopeAad(deviceId, fingerprint);
    if (peerPoint.size() != 65 || peerPoint.front() != '\x04' || iv.size() != 12
        || ciphertextAndTag.size() <= 16 || aad.isEmpty())
        return {};

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> peer(publicKeyFromPoint(peerPoint), &EVP_PKEY_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> derive(
        peer ? EVP_PKEY_CTX_new(m_key, nullptr) : nullptr, &EVP_PKEY_CTX_free);
    if (!derive || EVP_PKEY_derive_init(derive.get()) <= 0 || EVP_PKEY_derive_set_peer(derive.get(), peer.get()) <= 0)
        return {};
    size_t sharedSize = 0;
    if (EVP_PKEY_derive(derive.get(), nullptr, &sharedSize) <= 0 || sharedSize == 0 || sharedSize > 128)
        return {};
    SecureBytes shared(QByteArray(static_cast<qsizetype>(sharedSize), '\0'));
    if (EVP_PKEY_derive(derive.get(), reinterpret_cast<unsigned char*>(shared.bytes().data()), &sharedSize) <= 0)
        return {};
    shared.bytes().resize(static_cast<qsizetype>(sharedSize));
    SecureBytes key = hkdf(shared, m_publicKey);
    shared.clear();
    if (key.isEmpty())
        return {};

    const int cipherSize = ciphertextAndTag.size() - 16;
    SecureBytes plaintext(QByteArray(cipherSize, '\0'));
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(),
                                                                         &EVP_CIPHER_CTX_free);
    int written = 0;
    int finalBytes = 0;
    if (!cipher
        || EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) != 1
        || EVP_DecryptInit_ex(cipher.get(), nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.bytes().constData()),
            reinterpret_cast<const unsigned char*>(iv.constData())) != 1
        || EVP_DecryptUpdate(cipher.get(), nullptr, &written,
            reinterpret_cast<const unsigned char*>(aad.constData()), aad.size()) != 1
        || EVP_DecryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(plaintext.bytes().data()), &written,
            reinterpret_cast<const unsigned char*>(ciphertextAndTag.constData()), cipherSize) != 1
        || EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, 16,
            const_cast<char*>(ciphertextAndTag.constData() + cipherSize)) != 1
        || EVP_DecryptFinal_ex(cipher.get(),
            reinterpret_cast<unsigned char*>(plaintext.bytes().data()) + written, &finalBytes) != 1)
        return {};
    plaintext.bytes().resize(written + finalBytes);
    if (!plaintext.bytes().startsWith("-----BEGIN PGP PRIVATE KEY BLOCK-----"))
        return {};
    return plaintext;
}
