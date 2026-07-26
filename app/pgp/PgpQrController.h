#pragma once

#include "models/Contact.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

class PgpQrRepository;
class PgpQrClient;

// QML-facing bridge over core/domain's PgpQrRepository (the "My QR Code"
// token-fetch side, which needs this device's own pairing resolved) and
// core/net's PgpQrClient directly (the "Scan to Add Contact Key" side,
// which needs no pairing at all -- the token in the scanned URL is the
// sole credential, and the scan target may be a different server than
// this device's own paired one). Registered as the "PgpQr" QML singleton.
//
// Persistence of a scanned key onto a contact is deliberately left to QML
// gluing this singleton and the existing "ContactsApp" singleton together
// (calling ContactsApp.createContact/updateContact with the scanned
// publicKey) -- no C++ coupling between the two controllers, matching how
// this repo already wires independent singletons together only at the
// QML/main.cpp level (e.g. NotificationDispatcher -> MailController).
class PgpQrController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isBusy READ isBusy NOTIFY isBusyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString myQrUrl READ myQrUrl NOTIFY myQrDataChanged)
    Q_PROPERTY(QString myQrExpiresAt READ myQrExpiresAt NOTIFY myQrDataChanged)
    Q_PROPERTY(QString scannedName READ scannedName NOTIFY scanResultChanged)
    Q_PROPERTY(QString scannedFingerprint READ scannedFingerprint NOTIFY scanResultChanged)
    Q_PROPERTY(QString scannedPublicKey READ scannedPublicKey NOTIFY scanResultChanged)

public:
    PgpQrController(PgpQrRepository& repository, PgpQrClient& client, QObject* parent = nullptr);

    bool isBusy() const;
    QString lastError() const;
    QString myQrUrl() const;
    QString myQrExpiresAt() const;
    QString scannedName() const;
    QString scannedFingerprint() const;
    QString scannedPublicKey() const;

public slots:
    // Calls repository.fetchMyToken(); on Success, myQrUrl/myQrExpiresAt
    // update (PgpMyQrCode.qml renders a QZXing-encoded Image of myQrUrl and
    // re-arms its own auto-refresh timer off myQrExpiresAt -- only after a
    // Success, never after NoPgpIdentity/ServiceUnavailable, which show a
    // static message and stop retrying instead).
    void refreshMyQrCode();

    // Encodes myQrUrl as a QR code (via zxing-cpp, see PgpQrController.cpp)
    // and returns it as a "data:image/png;base64,..." URL -- PgpMyQrCode.qml
    // binds this straight to an Image's `source`, no QQuickImageProvider
    // registration needed. Returns "" when myQrUrl is empty (nothing to
    // encode yet, e.g. before the first refreshMyQrCode() success).
    Q_INVOKABLE QString myQrImageDataUrl() const;

    // Validates decodedText looks like a "/api/pgp/qr/key" URL, then calls
    // client.fetchKey() directly (no pairing involved). On success,
    // scannedName/scannedFingerprint/scannedPublicKey populate for
    // PgpScanContactKey.qml to show for out-of-band fingerprint
    // confirmation before the caller saves it onto a contact.
    void scanQrPayload(const QString& decodedText);

    // Re-arms the scan screen for another attempt (clears scannedName/
    // scannedFingerprint/scannedPublicKey/lastError).
    void clearScanResult();

    // The shareable subset of the scanned person's contact details (server's
    // optional "contactCard" on the key response, see PgpQrClient::
    // fetchKey()), reshaped to exactly the field keys
    // ContactsController::createContact/updateContact already accept (org,
    // notes, emails, phones, addresses, department, pronouns,
    // phoneticGivenName, phoneticFamilyName, ims, websites, relations,
    // events, customFields) -- every value defaults to an empty string/list
    // when no contactCard was present in the last scan (or none has been
    // made yet). fn/pgpKey are deliberately NOT included here -- callers
    // already have those from scannedName()/scannedPublicKey() (the
    // out-of-band-confirmed identity), this is only the rest of the card.
    // birthday is omitted because the create/edit form doesn't expose that
    // field either.
    Q_INVOKABLE QVariantMap scannedContactCardFields() const;

signals:
    void isBusyChanged();
    void lastErrorChanged();
    void myQrDataChanged();
    void scanResultChanged();

private:
    void setBusy(bool busy);
    void setLastError(const QString& error);

    PgpQrRepository& m_repository;
    PgpQrClient& m_client;
    bool m_isBusy = false;
    QString m_lastError;
    QString m_myQrUrl;
    QString m_myQrExpiresAt;
    QString m_scannedName;
    QString m_scannedFingerprint;
    QString m_scannedPublicKey;
    Contact m_scannedContactCard;
    // Guards this controller's network-calling slots against re-entering
    // through the nested QEventLoop HttpClient runs -- QML keeps delivering
    // clicks while a blocking call is suspended. See
    // core/util/ReentrancyGuard.h.
    bool m_inNetworkCall = false;
};
