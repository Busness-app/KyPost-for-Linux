#include "pgp/PgpQrController.h"

#include "contacts/ContactFieldMapping.h"
#include "domain/PgpQrRepository.h"
#include "net/NetworkError.h"
#include "net/PgpQrClient.h"
#include "pgp/PgpQrTargetValidator.h"
#include "util/ReentrancyGuard.h"

#include <KLocalizedString>

// Plain ZXing:: C++ API (not ZXingQt.h's Q_GADGET/Q_OBJECT wrapper types --
// those need moc processing, which AUTOMOC won't reach into for a vendored
// system header; see app/CMakeLists.txt's PgpQrController comment).
#include <ZXing/CreateBarcode.h>
#include <ZXing/WriteBarcode.h>

#include <QBuffer>
#include <QImage>
#include <QUrl>
#include <QVariantList>

PgpQrController::PgpQrController(PgpQrRepository& repository, PgpQrClient& client, QObject* parent)
    : QObject(parent)
    , m_repository(repository)
    , m_client(client)
{
}

bool PgpQrController::isBusy() const
{
    return m_isBusy;
}

QString PgpQrController::lastError() const
{
    return m_lastError;
}

QString PgpQrController::myQrUrl() const
{
    return m_myQrUrl;
}

QString PgpQrController::myQrExpiresAt() const
{
    return m_myQrExpiresAt;
}

QString PgpQrController::scannedName() const
{
    return m_scannedName;
}

QString PgpQrController::scannedFingerprint() const
{
    return m_scannedFingerprint;
}

QString PgpQrController::scannedPublicKey() const
{
    return m_scannedPublicKey;
}

void PgpQrController::setBusy(bool busy)
{
    if (m_isBusy == busy)
        return;
    m_isBusy = busy;
    emit isBusyChanged();
}

void PgpQrController::setLastError(const QString& error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

void PgpQrController::refreshMyQrCode()
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;

    setBusy(true);
    const PgpQrTokenOutcome outcome = m_repository.fetchMyToken();
    setBusy(false);

    switch (outcome.status) {
    case PgpQrTokenStatus::Success:
        setLastError(QString());
        m_myQrUrl = outcome.url;
        m_myQrExpiresAt = outcome.expiresAt;
        emit myQrDataChanged();
        break;
    case PgpQrTokenStatus::NotPaired:
        setLastError(i18n("Not paired"));
        break;
    case PgpQrTokenStatus::NoPgpIdentity:
        setLastError(i18n("You haven't set up PGP encryption yet"));
        break;
    case PgpQrTokenStatus::Unauthorized:
        setLastError(i18n("Session expired -- please re-pair this device"));
        break;
    case PgpQrTokenStatus::ServiceUnavailable:
        setLastError(i18n("PGP QR exchange is unavailable right now"));
        break;
    case PgpQrTokenStatus::Retry:
        setLastError(outcome.detail.isEmpty() ? i18n("Failed to load QR code, try again") : outcome.detail);
        break;
    }
}

QString PgpQrController::myQrImageDataUrl() const
{
    if (m_myQrUrl.isEmpty())
        return QString();

    const ZXing::CreatorOptions creatorOptions(ZXing::BarcodeFormat::QRCode);
    const ZXing::Barcode barcode = ZXing::CreateBarcodeFromText(m_myQrUrl.toStdString(), creatorOptions);
    if (!barcode.isValid())
        return QString();

    // WriteBarcodeToImage renders single-byte-per-pixel (Lum/grayscale)
    // data with no padding between rows -- width itself is the correct
    // bytesPerLine, same as zxing-cpp's own ZXingQt::Barcode::toImage()
    // reference implementation. .copy() takes ownership of the pixel data
    // before ZXing::Image (and the buffer QImage was only a view over) goes
    // out of scope.
    const ZXing::Image zxImage = ZXing::WriteBarcodeToImage(barcode);
    const QImage image =
        QImage(zxImage.data(), zxImage.width(), zxImage.height(), zxImage.width(), QImage::Format_Grayscale8).copy();

    QByteArray pngBytes;
    QBuffer buffer(&pngBytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(pngBytes.toBase64());
}

namespace {

// Both the initial QR target and every redirect hop must satisfy this.
//
// The path is normalized first: Qt resolves dot segments when it puts the
// request on the wire but keeps them in QUrl::path(), so
// "/api/pgp/qr/key/../../../../internal/admin" satisfied a naive
// contains() check while actually requesting "/internal/admin".
bool isPermittedQrEndpoint(const QUrl& url)
{
    if (!isSafeQrTarget(url))
        return false;
    return url.adjusted(QUrl::NormalizePathSegments).path().contains(QStringLiteral("/api/pgp/qr/key"));
}

} // namespace

void PgpQrController::scanQrPayload(const QString& decodedText)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;

    // No RelayAuth -- the token in the URL is the sole credential, and the
    // scan target may be a different server than this device's own paired
    // one, so this deliberately doesn't route through m_repository at all.
    const QUrl qrUrl(decodedText);
    if (!qrUrl.isValid() || !isPermittedQrEndpoint(qrUrl)) {
        setLastError(i18n("That QR code isn't a PGP key-exchange code"));
        return;
    }

    setBusy(true);
    // VibeSec finding: a URL that legitimately passes isSafeQrTarget above
    // could still 302 the actual fetch to a link-local/metadata address --
    // HttpClient follows redirects by default, and that redirect target was
    // never re-validated. Passing isSafeQrTarget through as fetchKey's
    // redirect validator closes that gap: every hop, not just the
    // QR-encoded URL itself, has to pass the same check.
    // The redirect validator must apply the SAME predicate as the initial
    // check, path included. Passing only isSafeQrTarget let a permitted host
    // 302 the fetch to any path on a loopback service -- which
    // isSafeQrTarget deliberately allows over plain http, so the certificate
    // pin never engaged either.
    const PgpQrKeyResult result = m_client.fetchKey(qrUrl, isPermittedQrEndpoint);
    setBusy(false);

    if (!result.error.has_value()) {
        setLastError(QString());
        m_scannedName = result.name;
        m_scannedFingerprint = result.fingerprint;
        m_scannedPublicKey = result.publicKey;
        m_scannedContactCard = result.contactCard.value_or(Contact());
        emit scanResultChanged();
        return;
    }

    if (result.statusCode == 404) {
        setLastError(i18n("This person hasn't set up PGP encryption yet"));
    } else if (*result.error == NetworkError::Unauthorized) {
        // 403 here means "invalid or expired token" (handlePGPQRKey), not
        // this device's own auth -- distinct wording from the token-fetch
        // side's 401 handling above.
        setLastError(i18n("That code has expired -- ask them to refresh and scan again"));
    } else if (*result.error == NetworkError::ServiceUnavailable) {
        setLastError(i18n("PGP QR exchange is unavailable right now"));
    } else {
        setLastError(result.detail.isEmpty() ? i18n("Failed to fetch key, try again") : result.detail);
    }
}

void PgpQrController::clearScanResult()
{
    setLastError(QString());
    m_scannedName.clear();
    m_scannedFingerprint.clear();
    m_scannedPublicKey.clear();
    m_scannedContactCard = Contact();
    emit scanResultChanged();
}

QVariantMap PgpQrController::scannedContactCardFields() const
{
    QVariantMap fields;
    fields[QStringLiteral("org")] = m_scannedContactCard.org.value_or(QString());
    fields[QStringLiteral("notes")] = m_scannedContactCard.notes.value_or(QString());
    fields[QStringLiteral("emails")] = entriesToVariantList(m_scannedContactCard.emails, emailEntryToMap);
    fields[QStringLiteral("phones")] = entriesToVariantList(m_scannedContactCard.phones, phoneEntryToMap);
    fields[QStringLiteral("addresses")] = entriesToVariantList(m_scannedContactCard.addresses, addressEntryToMap);
    fields[QStringLiteral("department")] = m_scannedContactCard.department.value_or(QString());
    fields[QStringLiteral("pronouns")] = m_scannedContactCard.pronouns.value_or(QString());
    fields[QStringLiteral("phoneticGivenName")] = m_scannedContactCard.phoneticGivenName.value_or(QString());
    fields[QStringLiteral("phoneticFamilyName")] = m_scannedContactCard.phoneticFamilyName.value_or(QString());
    fields[QStringLiteral("ims")] = entriesToVariantList(m_scannedContactCard.ims, imEntryToMap);
    fields[QStringLiteral("websites")] = entriesToVariantList(m_scannedContactCard.websites, urlEntryToMap);
    fields[QStringLiteral("relations")] = entriesToVariantList(m_scannedContactCard.relations, relationEntryToMap);
    fields[QStringLiteral("events")] = entriesToVariantList(m_scannedContactCard.events, eventEntryToMap);
    fields[QStringLiteral("customFields")] =
        entriesToVariantList(m_scannedContactCard.customFields, customFieldEntryToMap);
    return fields;
}
