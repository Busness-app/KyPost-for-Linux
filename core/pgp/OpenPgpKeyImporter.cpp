#include "pgp/OpenPgpKeyImporter.h"

#include <QFile>
#include <QTemporaryDir>

#include <gpgme.h>

namespace {

// Same RAII shape as OpenPgpDecryptor's, and for the same reason: gpgme's API
// is C and every early return here has to release two handles.
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

void initialiseGpgme()
{
    static const bool initialised = []() {
        gpgme_check_version(nullptr);
        return true;
    }();
    Q_UNUSED(initialised);
}

// Imports into `home` and reports what gpg made of it.
//
// Returns the fingerprint of the first key in the import result, plus whether
// anything actually changed. An armored blob can carry several keys; only the
// first is reported, because the caller resolved ONE address and a bundle
// smuggling extra keys past a single-fingerprint check is exactly what the
// comparison exists to stop.
bool importInto(const QString& home, const QByteArray& armored, QString* fingerprint, bool* changed,
                 QString* detail)
{
    initialiseGpgme();

    ContextHandle context;
    if (gpgme_err_code(gpgme_new(&context.handle)) != GPG_ERR_NO_ERROR || context.handle == nullptr) {
        *detail = QStringLiteral("could not create a gpgme context");
        return false;
    }

    const QByteArray homeUtf8 = home.toUtf8();
    if (!homeUtf8.isEmpty()
        && gpgme_err_code(gpgme_ctx_set_engine_info(context.handle, GPGME_PROTOCOL_OpenPGP, nullptr,
                                                      homeUtf8.constData()))
            != GPG_ERR_NO_ERROR) {
        *detail = QStringLiteral("could not point gpgme at the keyring");
        return false;
    }

    DataHandle keyData;
    if (gpgme_err_code(gpgme_data_new_from_mem(&keyData.handle, armored.constData(),
                                                 static_cast<size_t>(armored.size()), /*copy=*/0))
        != GPG_ERR_NO_ERROR) {
        *detail = QStringLiteral("could not read the key bytes");
        return false;
    }

    if (gpgme_err_code(gpgme_op_import(context.handle, keyData.handle)) != GPG_ERR_NO_ERROR) {
        *detail = QStringLiteral("gpg refused the key");
        return false;
    }

    const gpgme_import_result_t result = gpgme_op_import_result(context.handle);
    if (result == nullptr || result->considered == 0 || result->imports == nullptr) {
        *detail = QStringLiteral("the bytes carried no OpenPGP key");
        return false;
    }

    *fingerprint = QString::fromLatin1(result->imports->fpr);
    // `unchanged` counts keys gpg already had in full. Anything else means the
    // keyring gained material -- a new key, or new signatures or user IDs on
    // one it already held.
    *changed = result->unchanged == 0;
    return !fingerprint->isEmpty();
}

} // namespace

PgpImportResult importPublicKey(const QByteArray& armoredPublicKey, const QString& expectedFingerprint,
                                 const QString& homeDirectory)
{
    PgpImportResult out;

    if (armoredPublicKey.trimmed().isEmpty()) {
        out.status = PgpImportStatus::Rejected;
        out.detail = QStringLiteral("no key bytes");
        return out;
    }

    initialiseGpgme();
    if (gpgme_err_code(gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP)) != GPG_ERR_NO_ERROR) {
        out.status = PgpImportStatus::EngineUnavailable;
        return out;
    }

    // Step one, in a keyring nobody owns: find out what this actually IS
    // before the user's own keyring is touched. A mismatch discovered after
    // importing would leave this code deleting from a keyring it did not
    // create, and an interrupted run would leave the bad key behind.
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        out.status = PgpImportStatus::Rejected;
        out.detail = QStringLiteral("could not create a scratch keyring");
        return out;
    }
    // gpg refuses a home directory others can read.
    QFile::setPermissions(scratch.path(),
                           QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QString observed;
    bool changedInScratch = false;
    QString detail;
    if (!importInto(scratch.path(), armoredPublicKey, &observed, &changedInScratch, &detail)) {
        out.status = PgpImportStatus::Rejected;
        out.detail = detail;
        return out;
    }
    out.fingerprint = observed;

    if (!expectedFingerprint.isEmpty()
        && observed.compare(expectedFingerprint, Qt::CaseInsensitive) != 0) {
        // The relay's key and the relay's claim about that key disagree. That
        // is not a reason to pick one of them.
        out.status = PgpImportStatus::Rejected;
        out.detail = QStringLiteral("fingerprint mismatch");
        return out;
    }

    // Step two: the real keyring, only now.
    QString importedFingerprint;
    bool changed = false;
    if (!importInto(homeDirectory, armoredPublicKey, &importedFingerprint, &changed, &detail)) {
        out.status = PgpImportStatus::Rejected;
        out.detail = detail;
        return out;
    }

    out.fingerprint = importedFingerprint;
    out.status = changed ? PgpImportStatus::Imported : PgpImportStatus::Unchanged;
    return out;
}
