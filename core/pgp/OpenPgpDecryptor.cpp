#include "pgp/OpenPgpDecryptor.h"

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
    case GPG_ERR_NO_DATA:
        return PgpDecryptStatus::NoSecretKey;
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

// gpgme requires this before any other call, and the first call is not
// thread-safe. Doing it once at first use keeps that guarantee without
// making every caller remember it.
void initialiseGpgme()
{
    static const bool initialised = []() {
        gpgme_check_version(nullptr);
        return true;
    }();
    Q_UNUSED(initialised);
}

} // namespace

OpenPgpDecryptor::OpenPgpDecryptor(qint64 maxPlaintextBytes) : m_maxPlaintextBytes(maxPlaintextBytes)
{
}

bool OpenPgpDecryptor::engineAvailable()
{
    initialiseGpgme();
    return gpgme_err_code(gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP)) == GPG_ERR_NO_ERROR;
}

PgpDecryptResult OpenPgpDecryptor::decrypt(const QByteArray& ciphertext, const QString& homeDirectory) const
{
    PgpDecryptResult result;

    if (ciphertext.isEmpty()) {
        result.status = PgpDecryptStatus::Malformed;
        return result;
    }

    initialiseGpgme();

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

    const gpgme_error_t error = gpgme_op_decrypt(context.handle, input.handle, output.handle);

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

    result.status = PgpDecryptStatus::Decrypted;
    result.plaintext = sink.data;
    return result;
}
