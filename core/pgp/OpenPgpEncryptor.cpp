#include "pgp/OpenPgpEncryptor.h"

#include "pgp/GpgmeInit.h"

#include <QVector>

#include <gpgme.h>

namespace {

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

// Owns the recipient keys, which gpgme hands out with a reference each.
struct KeyList
{
    QVector<gpgme_key_t> keys;
    ~KeyList()
    {
        for (gpgme_key_t key : keys) {
            if (key != nullptr)
                gpgme_key_unref(key);
        }
    }
    // gpgme wants a NULL-terminated array.
    QVector<gpgme_key_t> terminated() const
    {
        QVector<gpgme_key_t> array = keys;
        array.append(nullptr);
        return array;
    }
};

PgpEncryptStatus statusFromError(gpgme_error_t error)
{
    switch (gpgme_err_code(error)) {
    case GPG_ERR_CANCELED:
    case GPG_ERR_FULLY_CANCELED:
    case GPG_ERR_BAD_PASSPHRASE:
        return PgpEncryptStatus::CancelledOrWrongPassphrase;
    case GPG_ERR_UNUSABLE_SECKEY:
    case GPG_ERR_NO_SECKEY:
        return PgpEncryptStatus::NoSigningKey;
    case GPG_ERR_UNUSABLE_PUBKEY:
        return PgpEncryptStatus::RecipientKeyUnusable;
    default:
        return PgpEncryptStatus::Failed;
    }
}

QByteArray readAll(gpgme_data_t data)
{
    QByteArray out;
    if (gpgme_data_seek(data, 0, SEEK_SET) < 0)
        return out;
    char buffer[8192];
    ssize_t read = 0;
    while ((read = gpgme_data_read(data, buffer, sizeof(buffer))) > 0)
        out.append(buffer, static_cast<int>(read));
    return out;
}

} // namespace

PgpEncryptResult signAndEncrypt(const QByteArray& plaintext, const QString& signerAddress,
                                 const QStringList& recipientFingerprints,
                                 const QString& homeDirectory)
{
    PgpEncryptResult out;

    if (plaintext.isEmpty() || recipientFingerprints.isEmpty()) {
        out.status = PgpEncryptStatus::Failed;
        out.detail = QStringLiteral("nothing to encrypt, or nobody to encrypt to");
        return out;
    }

    ensureGpgmeInitialised();
    if (gpgme_err_code(gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP)) != GPG_ERR_NO_ERROR) {
        out.status = PgpEncryptStatus::EngineUnavailable;
        return out;
    }

    ContextHandle context;
    if (gpgme_err_code(gpgme_new(&context.handle)) != GPG_ERR_NO_ERROR || context.handle == nullptr) {
        out.status = PgpEncryptStatus::EngineUnavailable;
        return out;
    }

    const QByteArray home = homeDirectory.toUtf8();
    if (!home.isEmpty()
        && gpgme_err_code(gpgme_ctx_set_engine_info(context.handle, GPGME_PROTOCOL_OpenPGP, nullptr,
                                                      home.constData()))
            != GPG_ERR_NO_ERROR) {
        out.status = PgpEncryptStatus::EngineUnavailable;
        return out;
    }

    // RFC 3156 carries the ciphertext as armor inside a MIME part.
    gpgme_set_armor(context.handle, 1);

    // The signing key first: with no secret key there is nothing to attempt,
    // and finding that out before pinentry opens saves the user a prompt they
    // cannot satisfy.
    gpgme_key_t signer = nullptr;
    if (gpgme_err_code(gpgme_get_key(context.handle, signerAddress.toUtf8().constData(), &signer,
                                       /*secret=*/1))
            != GPG_ERR_NO_ERROR
        || signer == nullptr) {
        out.status = PgpEncryptStatus::NoSigningKey;
        out.detail = QStringLiteral("no secret key for the sender address");
        return out;
    }
    gpgme_signers_clear(context.handle);
    const gpgme_error_t signerError = gpgme_signers_add(context.handle, signer);
    gpgme_key_unref(signer);
    if (gpgme_err_code(signerError) != GPG_ERR_NO_ERROR) {
        out.status = PgpEncryptStatus::NoSigningKey;
        out.detail = QStringLiteral("gpg would not use the sender's key to sign");
        return out;
    }

    // Every recipient key must be present BEFORE anything is encrypted. A
    // missing one discovered afterwards would already have produced ciphertext
    // for the others, and the tempting thing to do with it is send it.
    KeyList recipients;
    for (const QString& fingerprint : recipientFingerprints) {
        gpgme_key_t key = nullptr;
        if (gpgme_err_code(
                gpgme_get_key(context.handle, fingerprint.toUtf8().constData(), &key, /*secret=*/0))
                != GPG_ERR_NO_ERROR
            || key == nullptr) {
            out.unusableRecipients.append(fingerprint);
            continue;
        }
        recipients.keys.append(key);
    }
    if (!out.unusableRecipients.isEmpty()) {
        out.status = PgpEncryptStatus::RecipientKeyUnusable;
        out.detail = QStringLiteral("a recipient key is not in the keyring");
        return out;
    }

    DataHandle input;
    if (gpgme_err_code(gpgme_data_new_from_mem(&input.handle, plaintext.constData(),
                                                 static_cast<size_t>(plaintext.size()), /*copy=*/0))
        != GPG_ERR_NO_ERROR) {
        out.status = PgpEncryptStatus::Failed;
        return out;
    }

    DataHandle output;
    if (gpgme_err_code(gpgme_data_new(&output.handle)) != GPG_ERR_NO_ERROR) {
        out.status = PgpEncryptStatus::Failed;
        return out;
    }

    const QVector<gpgme_key_t> keyArray = recipients.terminated();
    const gpgme_error_t error =
        gpgme_op_encrypt_sign(context.handle, const_cast<gpgme_key_t*>(keyArray.constData()),
                               GPGME_ENCRYPT_ALWAYS_TRUST, input.handle, output.handle);

    // Checked even on success. gpgme reports "encrypted, but not to these"
    // rather than failing, and sending that ciphertext means the recipient
    // whose key was newest is the one who cannot read the message.
    if (const gpgme_encrypt_result_t encryptResult = gpgme_op_encrypt_result(context.handle);
        encryptResult != nullptr) {
        for (gpgme_invalid_key_t invalid = encryptResult->invalid_recipients; invalid != nullptr;
             invalid = invalid->next) {
            out.unusableRecipients.append(QString::fromUtf8(invalid->fpr));
        }
    }
    if (!out.unusableRecipients.isEmpty()) {
        out.status = PgpEncryptStatus::RecipientKeyUnusable;
        out.detail = QStringLiteral("gpg refused a recipient key");
        return out;
    }

    if (const gpgme_sign_result_t signResult = gpgme_op_sign_result(context.handle);
        signResult != nullptr && signResult->invalid_signers != nullptr) {
        out.status = PgpEncryptStatus::NoSigningKey;
        out.detail = QStringLiteral("gpg refused the signing key");
        return out;
    }

    if (gpgme_err_code(error) != GPG_ERR_NO_ERROR) {
        out.status = statusFromError(error);
        out.detail = QStringLiteral("gpgme reported %1").arg(gpgme_err_code(error));
        return out;
    }

    const QByteArray armored = readAll(output.handle);
    if (armored.isEmpty()) {
        out.status = PgpEncryptStatus::Failed;
        out.detail = QStringLiteral("gpg produced no ciphertext");
        return out;
    }

    out.armoredCiphertext = QString::fromUtf8(armored);
    out.status = PgpEncryptStatus::Encrypted;
    return out;
}

QString ownKeyFingerprint(const QString& address, const QString& homeDirectory)
{
    ensureGpgmeInitialised();

    ContextHandle context;
    if (gpgme_err_code(gpgme_new(&context.handle)) != GPG_ERR_NO_ERROR || context.handle == nullptr)
        return {};

    const QByteArray home = homeDirectory.toUtf8();
    if (!home.isEmpty()
        && gpgme_err_code(gpgme_ctx_set_engine_info(context.handle, GPGME_PROTOCOL_OpenPGP, nullptr,
                                                      home.constData()))
            != GPG_ERR_NO_ERROR) {
        return {};
    }

    gpgme_key_t key = nullptr;
    // secret=1: the Sent copy is only worth encrypting to a key this user can
    // actually open it with.
    if (gpgme_err_code(gpgme_get_key(context.handle, address.toUtf8().constData(), &key, /*secret=*/1))
            != GPG_ERR_NO_ERROR
        || key == nullptr) {
        return {};
    }
    const QString fingerprint = key->fpr != nullptr ? QString::fromLatin1(key->fpr) : QString();
    gpgme_key_unref(key);
    return fingerprint;
}
