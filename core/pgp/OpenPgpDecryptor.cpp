#include "pgp/OpenPgpDecryptor.h"

#include "pgp/GpgmeInit.h"

#include <gpgme.h>

#include <cstring>

namespace {

// gpgme's C API, not the gpgmepp C++ wrapper.
//
// Not a style preference. KDE neon's libgpgmepp-dev requires gpgme >= 2.0.0
// while Ubuntu noble carries gpgme 1.x, and CI layers both archives -- so
// the wrapper's CMake config found its own headers and then failed on its
// own dependency ("Could NOT find Gpgme ... Required is at least version
// 2.0.0"). The C API here is a decade older than anything this file needs,
// is what the wrapper is a wrapper for, and is present wherever gpgme is.
// One fewer dependency, and no version tangle to re-litigate.

// Collects the plaintext gpg produces and refuses to grow past a ceiling.
//
// A SINK rather than "decrypt, then check the size": OpenPGP messages carry
// compressed data, so a small ciphertext can expand without limit. Checking
// afterwards means the allocation the bound exists to prevent has already
// happened. Refusing the write makes gpgme stop and fail, which is what
// TooLarge is mapped from.
struct BoundedSink
{
    qint64 limit = 0;
    bool exceeded = false;
    QByteArray data;
};

ssize_t sinkWrite(void* handle, const void* buffer, size_t size)
{
    auto* sink = static_cast<BoundedSink*>(handle);
    if (sink->exceeded)
        return -1;
    if (static_cast<qint64>(sink->data.size()) + static_cast<qint64>(size) > sink->limit) {
        sink->exceeded = true;
        // Nothing partial is kept. Half a decrypted message is not a message,
        // and handing one up would be indistinguishable from a truncated mail
        // the sender actually wrote.
        sink->data.clear();
        return -1;
    }
    sink->data.append(static_cast<const char*>(buffer), static_cast<int>(size));
    return static_cast<ssize_t>(size);
}

PgpDecryptStatus statusFromError(gpgme_error_t error)
{
    // Mapped to what the user can DO about it, which is the whole reason
    // this is not a bool (AGENTS.md §4a). The error CODE is consulted, never
    // gpgme's human-readable string -- that is localised and not a contract.
    switch (gpgme_err_code(error)) {
    case GPG_ERR_NO_SECKEY:
        return PgpDecryptStatus::NoSecretKey;
    // NO_DATA is "there is no OpenPGP message in here", which is Malformed.
    // It was grouped with NO_SECKEY above until 2026-08-23, so a corrupt or
    // non-PGP payload told the user their key was on another machine and
    // sent them looking for it. The two are genuinely distinguishable --
    // mail encrypted to a key this keyring lacks really does come back as
    // NO_SECKEY -- so there was never anything to gain by conflating them.
    case GPG_ERR_NO_DATA:
        return PgpDecryptStatus::Malformed;
    case GPG_ERR_CANCELED:
    case GPG_ERR_FULLY_CANCELED:
    case GPG_ERR_BAD_PASSPHRASE:
        return PgpDecryptStatus::CancelledOrWrongPassphrase;
    default:
        return PgpDecryptStatus::Malformed;
    }
}

// Owning wrappers for the two C handles, so every early return releases
// them without a `goto cleanup` ladder. gpgme's API is C; this file does not
// have to read like it.
struct ContextHandle
{
    gpgme_ctx_t handle = nullptr;
    ~ContextHandle()
    {
        if (handle != nullptr)
            gpgme_release(handle);
    }
};

struct DataHandle
{
    gpgme_data_t handle = nullptr;
    ~DataHandle()
    {
        if (handle != nullptr)
            gpgme_data_release(handle);
    }
};

struct KeyHandle
{
    gpgme_key_t handle = nullptr;
    ~KeyHandle()
    {
        if (handle != nullptr)
            gpgme_key_unref(handle);
    }
};

// The primary key a signature's signing key belongs to, per gpg's own key
// data. A key with a dedicated signing subkey -- a hardware token's, and what
// Sequoia and Proton generate -- signs with the SUBKEY, so the fingerprint in
// the signature is never the one an address is bound to.
//
// Empty on any failure: no such key in this keyring, or gpg cannot say. That
// is the answer "unknown", and the caller must not read it as a match.
QString primaryFingerprintOf(gpgme_ctx_t context, const char* signingFingerprint)
{
    if (signingFingerprint == nullptr)
        return {};
    KeyHandle key;
    if (gpgme_err_code(gpgme_get_key(context, signingFingerprint, &key.handle, /*secret=*/0))
            != GPG_ERR_NO_ERROR
        || key.handle == nullptr || key.handle->fpr == nullptr) {
        return {};
    }
    return QString::fromLatin1(key.handle->fpr);
}

} // namespace

OpenPgpDecryptor::OpenPgpDecryptor(qint64 maxPlaintextBytes) : m_maxPlaintextBytes(maxPlaintextBytes)
{
}

bool OpenPgpDecryptor::engineAvailable()
{
    ensureGpgmeInitialised();
    return gpgme_err_code(gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP)) == GPG_ERR_NO_ERROR;
}

PgpDecryptResult OpenPgpDecryptor::decrypt(const QByteArray& ciphertext, const QString& homeDirectory) const
{
    PgpDecryptResult result;

    if (ciphertext.isEmpty()) {
        result.status = PgpDecryptStatus::Malformed;
        return result;
    }

    ensureGpgmeInitialised();

    ContextHandle context;
    if (gpgme_err_code(gpgme_new(&context.handle)) != GPG_ERR_NO_ERROR || context.handle == nullptr) {
        result.status = PgpDecryptStatus::EngineUnavailable;
        return result;
    }

    const QByteArray home = homeDirectory.toUtf8();
    if (!home.isEmpty()) {
        // Points this context at a specific keyring instead of the user's --
        // which is what makes any of this testable without touching (or
        // depending on) the developer's own keys.
        if (gpgme_err_code(gpgme_ctx_set_engine_info(context.handle, GPGME_PROTOCOL_OpenPGP, nullptr,
                                                       home.constData()))
            != GPG_ERR_NO_ERROR) {
            result.status = PgpDecryptStatus::EngineUnavailable;
            return result;
        }
    }

    DataHandle input;
    if (gpgme_err_code(gpgme_data_new_from_mem(&input.handle, ciphertext.constData(),
                                                 static_cast<size_t>(ciphertext.size()), /*copy=*/0))
        != GPG_ERR_NO_ERROR) {
        result.status = PgpDecryptStatus::EngineUnavailable;
        return result;
    }

    BoundedSink sink;
    sink.limit = m_maxPlaintextBytes;

    static gpgme_data_cbs sinkCallbacks = {
        nullptr,   // read: this is an output sink and gpgme never reads it
        sinkWrite, //
        nullptr,   // seek
        nullptr,   // release
    };

    DataHandle output;
    if (gpgme_err_code(gpgme_data_new_from_cbs(&output.handle, &sinkCallbacks, &sink))
        != GPG_ERR_NO_ERROR) {
        result.status = PgpDecryptStatus::EngineUnavailable;
        return result;
    }

    // decrypt_VERIFY, not decrypt. An RFC 3156 combined message carries its
    // signature inside the ciphertext, so this is the only point at which it
    // can be checked -- there is no second pass available later.
    const gpgme_error_t error =
        gpgme_op_decrypt_verify(context.handle, input.handle, output.handle);

    // Checked BEFORE the error code. Hitting the ceiling makes gpgme report a
    // write failure, which would otherwise map to Malformed and tell the user
    // their mail was corrupt when it was merely enormous.
    if (sink.exceeded) {
        result.status = PgpDecryptStatus::TooLarge;
        return result;
    }

    if (gpgme_err_code(error) != GPG_ERR_NO_ERROR) {
        result.status = statusFromError(error);
        return result;
    }

    if (const gpgme_verify_result_t verified = gpgme_op_verify_result(context.handle);
        verified != nullptr && verified->signatures != nullptr) {
        // The first signature only. A message signed by several keys is not
        // something this client can present honestly -- "which of these do you
        // mean" is a question the UI has no way to ask -- and taking the most
        // favourable one would be the wrong answer by construction.
        const gpgme_signature_t signature = verified->signatures;
        result.signature.present = true;
        result.signature.fingerprint =
            signature->fpr != nullptr ? QString::fromLatin1(signature->fpr) : QString();
        result.signature.primaryFingerprint = primaryFingerprintOf(context.handle, signature->fpr);
        switch (gpgme_err_code(signature->status)) {
        case GPG_ERR_NO_ERROR:
            result.signature.mathematicallyValid = true;
            break;
        case GPG_ERR_NO_PUBKEY:
            result.signature.keyUnavailable = true;
            break;
        default:
            // Bad, expired, revoked: not valid, and not "cannot check".
            break;
        }
    }

    result.status = PgpDecryptStatus::Decrypted;
    result.plaintext = sink.data;
    return result;
}
