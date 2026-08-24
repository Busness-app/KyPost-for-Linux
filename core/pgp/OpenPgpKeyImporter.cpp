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
// Refuses anything carrying more than ONE key, and returns that key's
// fingerprint plus whether anything actually changed.
//
// The refusal is the security-relevant part. gpgme_op_import() imports EVERY
// key in the blob, and gpgme_op_import_result()->imports is a LINKED LIST
// with one entry per key. Reading only the head of that list -- which this
// function used to do, under a comment claiming it stopped bundle smuggling
// -- accepted a bundle whose first key was the expected one and whose
// second, third and fourth were the attacker's: the scratch check passed on
// the head fingerprint, and the whole bundle then went into the user's real
// keyring. Measured, not assumed: a two-key export reports considered=2 with
// two distinct fingerprints in the list.
//
// Compared by fingerprint rather than by counting list entries, because a
// secret-key import legitimately reports the SAME fingerprint twice (once
// public, once secret).
bool importInto(const QString& home, const QByteArray& armored, bool requireSecret,
                QString* fingerprint, bool* changed, QString* detail)
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

    // Every status entry, not just the head. `considered` counts what gpg was
    // asked to import even when a key was rejected or already held, so it
    // catches a bundle whose extra keys did not make it in as well.
    QString only;
    for (gpgme_import_status_t status = result->imports; status != nullptr; status = status->next) {
        if (status->fpr == nullptr)
            continue;
        const QString fpr = QString::fromLatin1(status->fpr);
        if (!only.isEmpty() && fpr.compare(only, Qt::CaseInsensitive) != 0) {
            *detail = QStringLiteral("the bytes carried more than one OpenPGP key");
            return false;
        }
        only = fpr;
    }
    if (only.isEmpty()) {
        *detail = QStringLiteral("the bytes carried no OpenPGP key");
        return false;
    }
    if (result->considered != 1) {
        *detail = QStringLiteral("the bytes carried more than one OpenPGP key");
        return false;
    }

    *fingerprint = only;
    if (requireSecret) {
        gpgme_key_t key = nullptr;
        const gpgme_error_t lookup = gpgme_get_key(context.handle, result->imports->fpr, &key, 1);
        const bool hasSecret = gpgme_err_code(lookup) == GPG_ERR_NO_ERROR && key != nullptr && key->secret;
        if (key != nullptr)
            gpgme_key_unref(key);
        if (!hasSecret) {
            *detail = QStringLiteral("the bytes carried no private OpenPGP key");
            return false;
        }
    }
    // `unchanged` counts keys gpg already had in full. Anything else means the
    // keyring gained material -- a new key, or new signatures or user IDs on
    // one it already held.
    *changed = result->unchanged == 0;
    return true;
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
    if (!importInto(scratch.path(), armoredPublicKey, false, &observed, &changedInScratch, &detail)) {
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
    if (!importInto(homeDirectory, armoredPublicKey, false, &importedFingerprint, &changed, &detail)) {
        out.status = PgpImportStatus::Rejected;
        out.detail = detail;
        return out;
    }

    out.fingerprint = importedFingerprint;
    out.status = changed ? PgpImportStatus::Imported : PgpImportStatus::Unchanged;
    return out;
}

PgpImportResult importPrivateKey(const SecureBytes& armoredPrivateKey, const QString& expectedFingerprint,
                                  const QString& homeDirectory)
{
    PgpImportResult out;
    if (armoredPrivateKey.isEmpty()) {
        out.status = PgpImportStatus::Rejected;
        out.detail = QStringLiteral("no key bytes");
        return out;
    }
    initialiseGpgme();
    if (gpgme_err_code(gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP)) != GPG_ERR_NO_ERROR) {
        out.status = PgpImportStatus::EngineUnavailable;
        return out;
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        out.status = PgpImportStatus::Rejected;
        out.detail = QStringLiteral("could not create a scratch keyring");
        return out;
    }
    QFile::setPermissions(scratch.path(),
                           QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QString observed;
    bool scratchChanged = false;
    QString detail;
    if (!importInto(scratch.path(), armoredPrivateKey.bytes(), true, &observed, &scratchChanged, &detail)) {
        out.status = PgpImportStatus::Rejected;
        out.detail = detail;
        return out;
    }
    out.fingerprint = observed;
    if (expectedFingerprint.isEmpty()
        || observed.compare(expectedFingerprint, Qt::CaseInsensitive) != 0) {
        out.status = PgpImportStatus::Rejected;
        out.detail = QStringLiteral("fingerprint mismatch");
        return out;
    }
    QString imported;
    bool changed = false;
    if (!importInto(homeDirectory, armoredPrivateKey.bytes(), true, &imported, &changed, &detail)) {
        out.status = PgpImportStatus::Rejected;
        out.detail = detail;
        return out;
    }
    out.fingerprint = imported;
    out.status = changed ? PgpImportStatus::Imported : PgpImportStatus::Unchanged;
    return out;
}
