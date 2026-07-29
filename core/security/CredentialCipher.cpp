#include "security/CredentialCipher.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QRandomGenerator>

#include <QLoggingCategory>

#include <argon2.h>
#include <openssl/evp.h>

#include <memory>

namespace {

using EvpCtx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

// Count is a template parameter so the "whole 32-bit words" precondition is
// checked HERE, at the definition that depends on it, and cannot be called
// wrong at all.
//
// It used to be a runtime `int` with the static_assert living in
// sealWithDerivedKey() -- a different function, which happened to guard the
// only two call sites that existed at the time. generate(begin, end) fills
// whole quint32s, so the next caller to pass a non-multiple of 4 would have
// handed it an end pointer in the middle of a word. An invariant belongs
// where it is relied on, not next to whoever currently satisfies it.
template<int Count>
QByteArray randomBytes()
{
    static_assert(Count > 0 && Count % 4 == 0, "generate() fills whole 32-bit words");
    QByteArray out(Count, Qt::Uninitialized);
    // QRandomGenerator::system() is the CSPRNG (getrandom/urandom), unlike
    // the default global generator -- the distinction matters here.
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(out.data()),
                                          reinterpret_cast<quint32*>(out.data() + out.size()));
    return out;
}

// The KDF for every blob written from now on.
//
// Memory-hard, which is the entire point -- see kMagicArgon2id in the
// header. A failure here returns empty, and every caller checks the size
// before using it, so a libargon2 that refuses (out of memory, most
// plausibly) fails the operation rather than proceeding with a short or
// zeroed key.
QByteArray deriveKeyArgon2id(const QString& pin, const QByteArray& salt)
{
    const QByteArray pinBytes = pin.toUtf8();
    QByteArray out(CredentialCipher::kKeyBytes, Qt::Uninitialized);

    const int rc = argon2id_hash_raw(
        CredentialCipher::kArgon2Iterations, CredentialCipher::kArgon2MemoryKiB,
        CredentialCipher::kArgon2Parallelism, pinBytes.constData(),
        static_cast<size_t>(pinBytes.size()), salt.constData(), static_cast<size_t>(salt.size()),
        out.data(), static_cast<size_t>(out.size()));

    if (rc != ARGON2_OK) {
        qWarning("CredentialCipher: argon2id key derivation failed (%s)", argon2_error_message(rc));
        return QByteArray();
    }
    return out;
}

// The KDF that produced blobs written before the format carried a version
// marker. Used to OPEN those, never to seal.
QByteArray deriveKeyPbkdf2Legacy(const QString& pin, const QByteArray& salt)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, pin.toUtf8(), salt,
                                               CredentialCipher::kPbkdf2Iterations,
                                               CredentialCipher::kKeyBytes);
}

} // namespace

namespace CredentialCipher {

namespace {

// The shared body of seal()/sealWithKey(): everything after the key is in
// hand. A fresh IV is generated here, per call, for both callers -- see
// sealWithKey()'s header comment for why that is not optional.
std::optional<QString> sealWithDerivedKey(const QByteArray& key, const QByteArray& salt,
                                           const QByteArray& plaintext, bool legacyPbkdf2)
{
    if (key.size() != kKeyBytes || salt.size() != kSaltBytes)
        return std::nullopt;

    const QByteArray iv = randomBytes<kIvBytes>();

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

    // The header goes on only for Argon2id blobs. A key derived by the
    // legacy KDF must be re-emitted in the legacy layout, or the blob would
    // announce a KDF that cannot reproduce the key inside it and nothing
    // would ever open it again.
    const QByteArray header =
        legacyPbkdf2 ? QByteArray() : QByteArray(kMagicArgon2id, kMagicBytes);
    return QString::fromLatin1((header + salt + iv + ciphertext + tag).toBase64());
}

} // namespace

std::optional<QString> seal(const QString& pin, const QByteArray& plaintext)
{
    const QByteArray salt = randomBytes<kSaltBytes>();
    const QByteArray key = deriveKeyArgon2id(pin, salt);
    if (key.isEmpty())
        return std::nullopt;
    return sealWithDerivedKey(key, salt, plaintext, /*legacyPbkdf2=*/false);
}

std::optional<QString> sealWithKey(const SessionKey& sessionKey, const QByteArray& plaintext)
{
    if (!sessionKey.isValid())
        return std::nullopt;
    return sealWithDerivedKey(sessionKey.key, sessionKey.salt, plaintext, sessionKey.legacyPbkdf2);
}

std::optional<QByteArray> open(const QString& pin, const QString& sealed)
{
    const std::optional<std::pair<QByteArray, SessionKey>> opened = openWithKey(pin, sealed);
    if (!opened.has_value())
        return std::nullopt;
    return opened->first;
}

std::optional<std::pair<QByteArray, SessionKey>> openWithKey(const QString& pin, const QString& sealed)
{
    const QByteArray raw = QByteArray::fromBase64(sealed.toLatin1());

    // Which KDF wrote this? Blobs predating the version marker begin
    // directly with the salt, so "starts with the magic" is the whole test.
    //
    // The magic is 4 bytes of a 16-byte random salt's worth of keyspace
    // away from colliding, but a collision would not be dangerous anyway:
    // it would pick the wrong KDF, derive the wrong key, and fail the GCM
    // tag check -- the same outcome as a wrong PIN, which is exactly what
    // openWithKey() is required to be indistinguishable from.
    const bool legacyPbkdf2 = !raw.startsWith(QByteArray(kMagicArgon2id, kMagicBytes));
    const QByteArray blob = legacyPbkdf2 ? raw : raw.mid(kMagicBytes);

    // Everything but the ciphertext is fixed-size, so anything shorter than
    // the envelope itself cannot be a valid blob.
    if (blob.size() < kSaltBytes + kIvBytes + kTagBytes)
        return std::nullopt;

    const QByteArray salt = blob.left(kSaltBytes);
    const QByteArray iv = blob.mid(kSaltBytes, kIvBytes);
    const QByteArray tag = blob.right(kTagBytes);
    const QByteArray ciphertext =
        blob.mid(kSaltBytes + kIvBytes, blob.size() - kSaltBytes - kIvBytes - kTagBytes);

    const QByteArray key = legacyPbkdf2 ? deriveKeyPbkdf2Legacy(pin, salt) : deriveKeyArgon2id(pin, salt);
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
    return std::make_pair(plaintext, SessionKey{ key, salt, legacyPbkdf2 });
}

} // namespace CredentialCipher
