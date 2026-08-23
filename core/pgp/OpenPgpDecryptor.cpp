#include "pgp/OpenPgpDecryptor.h"

#include <gpgme++/context.h>
#include <gpgme++/data.h>
#include <gpgme++/decryptionresult.h>
#include <gpgme++/engineinfo.h>
#include <gpgme++/global.h>
#include <gpgme++/interfaces/dataprovider.h>

#include <memory>

namespace {

// Collects the plaintext gpg produces and refuses to grow past a ceiling.
//
// A sink rather than "decrypt, then check the size": OpenPGP messages carry
// compressed data, so a small ciphertext can expand without limit. Checking
// afterwards means the allocation the bound exists to prevent has already
// happened. Refusing the write makes gpgme stop and report a failure, which
// is what TooLarge is mapped from.
class BoundedSink : public GpgME::DataProvider
{
public:
    explicit BoundedSink(qint64 limit) : m_limit(limit) {}

    bool isSupported(Operation op) const override
    {
        // Write only. This is an output sink; gpgme never reads from it and
        // never seeks, and claiming otherwise would invite it to try.
        return op == Operation::Write;
    }

    ssize_t read(void*, size_t) override { return -1; }

    ssize_t write(const void* buffer, size_t bufSize) override
    {
        if (m_exceeded)
            return -1;
        if (static_cast<qint64>(m_data.size()) + static_cast<qint64>(bufSize) > m_limit) {
            m_exceeded = true;
            // Nothing partial is kept. Half a decrypted message is not a
            // message, and handing one up would be indistinguishable from a
            // truncated mail the sender actually wrote.
            m_data.clear();
            return -1;
        }
        m_data.append(static_cast<const char*>(buffer), static_cast<int>(bufSize));
        return static_cast<ssize_t>(bufSize);
    }

    off_t seek(off_t, int) override { return -1; }
    void release() override { m_data.clear(); }

    bool exceeded() const { return m_exceeded; }
    const QByteArray& data() const { return m_data; }

private:
    qint64 m_limit;
    bool m_exceeded = false;
    QByteArray m_data;
};

PgpDecryptStatus statusFromError(const GpgME::Error& error)
{
    // Mapped to what the user can DO about it, which is the whole reason
    // this is not a bool (AGENTS.md §4a). gpgme's codes are consulted rather
    // than its human-readable string, which is localised and not a contract.
    switch (error.code()) {
    case GPG_ERR_NO_SECKEY:
    case GPG_ERR_NO_DATA:
        return PgpDecryptStatus::NoSecretKey;
    case GPG_ERR_CANCELED:
    case GPG_ERR_FULLY_CANCELED:
    case GPG_ERR_BAD_PASSPHRASE:
        return PgpDecryptStatus::CancelledOrWrongPassphrase;
    case GPG_ERR_INV_VALUE:
    case GPG_ERR_BAD_DATA:
        return PgpDecryptStatus::Malformed;
    default:
        return PgpDecryptStatus::Malformed;
    }
}

} // namespace

OpenPgpDecryptor::OpenPgpDecryptor(qint64 maxPlaintextBytes) : m_maxPlaintextBytes(maxPlaintextBytes)
{
}

bool OpenPgpDecryptor::engineAvailable()
{
    GpgME::initializeLibrary();
    return GpgME::checkEngine(GpgME::OpenPGP).code() == GPG_ERR_NO_ERROR;
}

PgpDecryptResult OpenPgpDecryptor::decrypt(const QByteArray& ciphertext, const QString& homeDirectory) const
{
    PgpDecryptResult result;

    if (ciphertext.isEmpty()) {
        result.status = PgpDecryptStatus::Malformed;
        return result;
    }

    GpgME::initializeLibrary();
    const std::unique_ptr<GpgME::Context> context(GpgME::Context::createForProtocol(GpgME::OpenPGP));
    if (!context) {
        result.status = PgpDecryptStatus::EngineUnavailable;
        return result;
    }

    if (!homeDirectory.isEmpty()
        && context->setEngineHomeDirectory(homeDirectory.toUtf8().constData())) {
        result.status = PgpDecryptStatus::EngineUnavailable;
        return result;
    }

    GpgME::Data input(ciphertext.constData(), static_cast<size_t>(ciphertext.size()), /*copy=*/false);
    BoundedSink sink(m_maxPlaintextBytes);
    GpgME::Data output(&sink);

    const GpgME::DecryptionResult decryption = context->decrypt(input, output);

    // Checked BEFORE the error code. Hitting the ceiling makes gpgme report a
    // write failure, which would otherwise be mapped to Malformed and tell
    // the user their mail was corrupt when it was merely enormous.
    if (sink.exceeded()) {
        result.status = PgpDecryptStatus::TooLarge;
        return result;
    }

    if (decryption.error()) {
        result.status = statusFromError(decryption.error());
        return result;
    }

    result.status = PgpDecryptStatus::Decrypted;
    result.plaintext = sink.data();
    return result;
}
