#include "pgp/PgpSendPlanner.h"

#include "pgp/OpenPgpEncryptor.h"

namespace {

// Addresses are matched case-insensitively, because a user typing
// "Alice@Example.com" and a relay answering "alice@example.com" are the same
// recipient and a lookup miss here reads to the user as "no key on file".
QString fingerprintFor(const QHash<QString, QString>& byAddress, const QString& address)
{
    const QString wanted = address.trimmed().toLower();
    for (auto it = byAddress.constBegin(); it != byAddress.constEnd(); ++it) {
        if (it.key().trimmed().toLower() == wanted)
            return it.value().trimmed();
    }
    return {};
}

PgpSendPlanStatus statusFromEncrypt(PgpEncryptStatus status)
{
    switch (status) {
    case PgpEncryptStatus::Encrypted:
        return PgpSendPlanStatus::Built;
    case PgpEncryptStatus::NoSigningKey:
        return PgpSendPlanStatus::SigningUnavailable;
    case PgpEncryptStatus::RecipientKeyUnusable:
        return PgpSendPlanStatus::RecipientWithoutKey;
    case PgpEncryptStatus::EngineUnavailable:
        return PgpSendPlanStatus::EngineUnavailable;
    case PgpEncryptStatus::CancelledOrWrongPassphrase:
    case PgpEncryptStatus::Failed:
        break;
    }
    return PgpSendPlanStatus::EncryptionFailed;
}

} // namespace

PgpSendPlan buildPgpSendPlan(const OutgoingMessage& message, const QStringList& bcc,
                              const QHash<QString, QString>& fingerprintsByAddress,
                              const QString& senderFingerprint, const QString& homeDirectory)
{
    PgpSendPlan plan;

    QStringList toCc = message.to;
    toCc.append(message.cc);
    if (toCc.isEmpty() && bcc.isEmpty()) {
        plan.status = PgpSendPlanStatus::EncryptionFailed;
        plan.detail = QStringLiteral("no recipients");
        return plan;
    }

    // Every recipient is checked BEFORE anything is encrypted. Discovering a
    // missing key half-way through would leave ciphertext already built for
    // the others, and the tempting thing to do with it is send it.
    QStringList toCcFingerprints;
    for (const QString& address : toCc) {
        const QString fingerprint = fingerprintFor(fingerprintsByAddress, address);
        if (fingerprint.isEmpty())
            plan.recipientsWithoutKeys.append(address);
        else
            toCcFingerprints.append(fingerprint);
    }
    QStringList bccFingerprints;
    for (const QString& address : bcc) {
        const QString fingerprint = fingerprintFor(fingerprintsByAddress, address);
        if (fingerprint.isEmpty())
            plan.recipientsWithoutKeys.append(address);
        else
            bccFingerprints.append(fingerprint);
    }
    if (!plan.recipientsWithoutKeys.isEmpty()) {
        plan.status = PgpSendPlanStatus::RecipientWithoutKey;
        plan.detail = QStringLiteral("a recipient has no usable key");
        return plan;
    }

    // Built once and encrypted many times: the readable message is identical
    // for every delivery, including the Bcc ones. Only who it is encrypted to
    // and where SMTP sends it differ.
    const QByteArray content =
        protectedContent(message, randomMimeBoundary(message.body.toUtf8()));

    const auto encryptInto = [&](const QStringList& fingerprints, const QStringList& smtpRecipients,
                                  PgpSendPlan& into) -> bool {
        const PgpEncryptResult encrypted =
            signAndEncrypt(content, message.from, fingerprints, homeDirectory);
        if (encrypted.status != PgpEncryptStatus::Encrypted) {
            into.status = statusFromEncrypt(encrypted.status);
            into.detail = encrypted.detail;
            into.recipientsWithoutKeys = encrypted.unusableRecipients;
            into.deliveries.clear();
            return false;
        }
        PgpDelivery delivery;
        delivery.smtpRecipients = smtpRecipients;
        delivery.message = pgpMimeDelivery(message, encrypted.armoredCiphertext,
                                            randomMimeBoundary(encrypted.armoredCiphertext.toUtf8()));
        into.deliveries.append(delivery);
        return true;
    };

    if (!toCcFingerprints.isEmpty() && !encryptInto(toCcFingerprints, toCc, plan))
        return plan;

    // One delivery per Bcc recipient, each encrypted to that recipient alone.
    // A single ciphertext encrypted to everyone would name every Bcc recipient
    // in its key IDs.
    for (int i = 0; i < bccFingerprints.size(); ++i) {
        if (!encryptInto({ bccFingerprints.at(i) }, { bcc.at(i) }, plan))
            return plan;
    }

    // The Sent copy, encrypted to the sender's own key. Its absence is not a
    // failure -- the message went out -- but it is never papered over with a
    // readable copy, because that copy lands on the account's IMAP host, which
    // holds no key at all.
    if (senderFingerprint.trimmed().isEmpty()) {
        plan.sentCopyUnavailable = true;
    } else {
        const PgpEncryptResult copy =
            signAndEncrypt(content, message.from, { senderFingerprint.trimmed() }, homeDirectory);
        if (copy.status == PgpEncryptStatus::Encrypted) {
            plan.sentCopy = pgpMimeDelivery(message, copy.armoredCiphertext,
                                             randomMimeBoundary(copy.armoredCiphertext.toUtf8()));
        } else {
            // The deliveries are already built and are going out. Losing the
            // copy is reported, not escalated into a failed send.
            plan.sentCopyUnavailable = true;
        }
    }

    plan.status = PgpSendPlanStatus::Built;
    return plan;
}
