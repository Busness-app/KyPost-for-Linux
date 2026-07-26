#include "security/CredentialCipher.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QRandomGenerator>

#include <openssl/evp.h>

#include <memory>

namespace {

using EvpCtx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

QByteArray randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    // QRandomGenerator::system() is the CSPRNG (getrandom/urandom), unlike
    // the default global generator -- the distinction matters here.
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(out.data()),
                                          reinterpret_cast<quint32*>(out.data() + out.size()));
    return out;
}

QByteArray deriveKey(const QString& pin, const QByteArray& salt)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, pin.toUtf8(), salt,
                                               CredentialCipher::kPbkdf2Iterations,
                                               CredentialCipher::kKeyBytes);
}

} // namespace

namespace CredentialCipher {

std::optional<QString> seal(const QString& pin, const QByteArray& plaintext)
{
    // generate() fills whole quint32 words, so ask for multiples of 4 and
    // trim -- kSaltBytes/kIvBytes are both already word-aligned, but keep
    // the guard so changing them can't silently under-fill.
    static_assert(kSaltBytes % 4 == 0 && kIvBytes % 4 == 0, "randomBytes fills whole 32-bit words");

    const QByteArray salt = randomBytes(kSaltBytes);
    const QByteArray iv = randomBytes(kIvBytes);
    const QByteArray key = deriveKey(pin, salt);
    if (key.size() != kKeyBytes)
        return std::nullopt;

    EvpCtx ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx)
        return std::nullopt;

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return std::nullopt;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kIvBytes, nullptr) != 1)
        return std::nullopt;
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                            reinterpret_cast<const unsigned char*>(key.constData()),
                            reinterpret_cast<const unsigned char*>(iv.constData())) != 1)
        return std::nullopt;

    QByteArray ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int len = 0;
    if (EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(ciphertext.data()), &len,
                           reinterpret_cast<const unsigned char*>(plaintext.constData()),
                           static_cast<int>(plaintext.size()))
        != 1) {
        return std::nullopt;
    }
    int cipherLen = len;

    if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(ciphertext.data()) + len, &len) != 1)
        return std::nullopt;
    cipherLen += len;
    ciphertext.resize(cipherLen);

    QByteArray tag(kTagBytes, Qt::Uninitialized);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kTagBytes, tag.data()) != 1)
        return std::nullopt;

    return QString::fromLatin1((salt + iv + ciphertext + tag).toBase64());
}

std::optional<QByteArray> open(const QString& pin, const QString& sealed)
{
    const QByteArray blob = QByteArray::fromBase64(sealed.toLatin1());
    // Everything but the ciphertext is fixed-size, so anything shorter than
    // the envelope itself cannot be a valid blob.
    if (blob.size() < kSaltBytes + kIvBytes + kTagBytes)
        return std::nullopt;

    const QByteArray salt = blob.left(kSaltBytes);
    const QByteArray iv = blob.mid(kSaltBytes, kIvBytes);
    const QByteArray tag = blob.right(kTagBytes);
    const QByteArray ciphertext =
        blob.mid(kSaltBytes + kIvBytes, blob.size() - kSaltBytes - kIvBytes - kTagBytes);

    const QByteArray key = deriveKey(pin, salt);
    if (key.size() != kKeyBytes)
        return std::nullopt;

    EvpCtx ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx)
        return std::nullopt;

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return std::nullopt;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kIvBytes, nullptr) != 1)
        return std::nullopt;
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                            reinterpret_cast<const unsigned char*>(key.constData()),
                            reinterpret_cast<const unsigned char*>(iv.constData())) != 1) {
        return std::nullopt;
    }

    QByteArray plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int len = 0;
    if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &len,
                           reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                           static_cast<int>(ciphertext.size()))
        != 1) {
        return std::nullopt;
    }
    int plainLen = len;

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTagBytes,
                             const_cast<char*>(tag.constData()))
        != 1) {
        return std::nullopt;
    }

    // The only place a wrong PIN is detected: GCM verifies the tag here and
    // returns <= 0 on mismatch. Nothing before this point can tell the
    // difference, which is exactly the property we want.
    if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(plaintext.data()) + len, &len) <= 0)
        return std::nullopt;
    plainLen += len;

    plaintext.resize(plainLen);
    return plaintext;
}

} // namespace CredentialCipher
