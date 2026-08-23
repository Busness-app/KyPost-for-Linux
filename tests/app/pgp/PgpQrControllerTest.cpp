#include "pgp/PgpQrController.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "domain/PgpQrRepository.h"
#include "net/HttpClient.h"
#include "net/NetworkExecutor.h"
#include "net/PgpQrClient.h"
#include "stores/SecureStoreFile.h"

#include "../../core/net/FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class PgpQrControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void refreshMyQrCodeWithoutPairingSetsNotPairedError();
    void refreshMyQrCodeSuccessPopulatesUrlAndExpiresAt();
    void myQrCodeIsNotOfferedForAnAccountThisDeviceNoLongerHas();
    void refreshMyQrCodeNoPgpIdentitySetsFriendlyMessage();
    void myQrImageDataUrlIsEmptyBeforeRefreshAndPopulatedAfter();
    void scanQrPayloadRejectsNonPgpQrUrl();
    void scanQrPayloadRejectsNonHttpScheme();
    void scanQrPayloadRejectsLinkLocalMetadataHost();
    void scanQrPayloadSuccessPopulatesScanResult();
    void scanQrPayloadSuccessPopulatesContactCardFields();
    void scanQrPayloadWithNoContactCardReturnsAllEmptyFields();
    void scanQrPayload404SetsFriendlyMessage();
    void clearScanResultResetsFields();

    // Behaviour that only exists once the fetches are asynchronous.
    void refreshMyQrCodeReturnsWithoutBlocking();
    void repeatedScansWhileOneIsInFlightAreCoalesced();
    void scanRejectsDotSegmentPathsThatResolveOffTheKeyEndpoint();

private:
    static void savePairing(PairingStore& pairingStore, quint16 port);
};

void PgpQrControllerTest::savePairing(PairingStore& pairingStore, quint16 port)
{
    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("secret-1");
    pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    pairing.registrationUrl = QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(port);
    pairing.pairingToken = QStringLiteral("pair-tok");
    pairing.deviceId = QStringLiteral("device-1");
    pairing.deviceName = QStringLiteral("My Linux Desktop");
    QVERIFY(pairingStore.save(pairing));
}

void PgpQrControllerTest::refreshMyQrCodeWithoutPairingSetsNotPairedError()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore); // never saved -- not paired

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    controller.refreshMyQrCode();

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    QCOMPARE(controller.lastError(), QStringLiteral("Not paired"));
    QCOMPARE(controller.myQrUrl(), QString());
}

void PgpQrControllerTest::refreshMyQrCodeSuccessPopulatesUrlAndExpiresAt()
{
    const QByteArray body =
        R"({"token":"tok-1","expiresAt":"2026-07-17T12:02:00Z","url":"https://example.com/api/pgp/qr/key?t=tok-1"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    QSignalSpy dataSpy(&controller, &PgpQrController::myQrDataChanged);
    controller.refreshMyQrCode();

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    QCOMPARE(controller.lastError(), QString());
    QCOMPARE(controller.myQrUrl(), QStringLiteral("https://example.com/api/pgp/qr/key?t=tok-1"));
    QCOMPARE(controller.myQrExpiresAt(), QStringLiteral("2026-07-17T12:02:00Z"));
    QVERIFY(dataSpy.count() >= 1);
}

// The QR encodes a key-exchange token for THIS account. After the account is
// replaced, the screen keeps showing the previous one's -- nothing re-fetches
// it and nothing clears it -- so a correspondent who scans it exchanges keys
// with the account this device just removed, believing it is the current one.
//
// Same shape as the other stale-reply sites: what is on screen belongs to an
// account that is gone, and nothing about it says so.
void PgpQrControllerTest::myQrCodeIsNotOfferedForAnAccountThisDeviceNoLongerHas()
{
    const QByteArray body =
        R"({"token":"tok-1","expiresAt":"2026-07-17T12:02:00Z","url":"https://example.com/api/pgp/qr/key?t=tok-1"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    controller.refreshMyQrCode();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);
    QVERIFY(!controller.myQrUrl().isEmpty());

    // A different account on this device.
    DevicePairing replacement;
    replacement.subscriberId = QStringLiteral("sub-2");
    replacement.deviceSecret = QStringLiteral("secret-2");
    replacement.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    replacement.deviceId = QStringLiteral("dev-2");
    QVERIFY(pairingStore.save(replacement));

    QVERIFY2(controller.myQrUrl().isEmpty(),
             "the previous account's key-exchange QR is still on offer");
    QVERIFY2(controller.myQrExpiresAt().isEmpty(),
             "the previous account's QR expiry is still shown");
}

void PgpQrControllerTest::refreshMyQrCodeNoPgpIdentitySetsFriendlyMessage()
{
    FakeRelayServer fake(httpResponse(400, "Bad Request", "no pgp identity configured\n", "text/plain"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    controller.refreshMyQrCode();

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    QCOMPARE(controller.lastError(), QStringLiteral("You haven't set up PGP encryption yet"));
}

void PgpQrControllerTest::myQrImageDataUrlIsEmptyBeforeRefreshAndPopulatedAfter()
{
    const QByteArray body =
        R"({"token":"tok-1","expiresAt":"2026-07-17T12:02:00Z","url":"https://example.com/api/pgp/qr/key?t=tok-1"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    // Nothing fetched yet -- no URL to encode.
    QCOMPARE(controller.myQrImageDataUrl(), QString());

    controller.refreshMyQrCode();

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    const QString dataUrl = controller.myQrImageDataUrl();
    QVERIFY(dataUrl.startsWith(QStringLiteral("data:image/png;base64,")));
    // A real, non-trivial PNG payload was actually encoded, not a stub.
    QVERIFY(dataUrl.length() > 100);
}

void PgpQrControllerTest::scanQrPayloadRejectsNonPgpQrUrl()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    // No FakeRelayServer at all -- an invalid payload must be rejected
    // before any network call is attempted.
    controller.scanQrPayload(QStringLiteral("https://example.com/totally/unrelated"));

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    QCOMPARE(controller.lastError(), QStringLiteral("That QR code isn't a PGP key-exchange code"));
    QCOMPARE(controller.scannedFingerprint(), QString());
}

void PgpQrControllerTest::scanQrPayloadRejectsNonHttpScheme()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    // A file:// QR payload must never reach HttpClient/QNetworkAccessManager
    // -- doing so would let a scanned QR code read local files back as if
    // they were key material.
    controller.scanQrPayload(QStringLiteral("file:///etc/passwd#/api/pgp/qr/key"));

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    QCOMPARE(controller.lastError(), QStringLiteral("That QR code isn't a PGP key-exchange code"));
    QCOMPARE(controller.scannedFingerprint(), QString());
}

void PgpQrControllerTest::scanQrPayloadRejectsLinkLocalMetadataHost()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    // 169.254.169.254 is the cloud-metadata address on AWS/Azure/DigitalOcean
    // -- must be rejected before any request is attempted, same as the
    // file:// case above.
    controller.scanQrPayload(QStringLiteral("http://169.254.169.254/api/pgp/qr/key"));

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    QCOMPARE(controller.lastError(), QStringLiteral("That QR code isn't a PGP key-exchange code"));
    QCOMPARE(controller.scannedFingerprint(), QString());
}

void PgpQrControllerTest::scanQrPayloadSuccessPopulatesScanResult()
{
    const QByteArray body =
        R"({"name":"Ada","fingerprint":"ABCD1234","publicKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    QSignalSpy scanSpy(&controller, &PgpQrController::scanResultChanged);
    const QString qrUrl = QStringLiteral("http://127.0.0.1:%1/api/pgp/qr/key?t=tok-1").arg(fake.port());
    controller.scanQrPayload(qrUrl);

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    QCOMPARE(controller.lastError(), QString());
    QCOMPARE(controller.scannedName(), QStringLiteral("Ada"));
    QCOMPARE(controller.scannedFingerprint(), QStringLiteral("ABCD1234"));
    QCOMPARE(controller.scannedPublicKey(), QStringLiteral("-----BEGIN PGP PUBLIC KEY BLOCK-----"));
    QVERIFY(scanSpy.count() >= 1);
}

void PgpQrControllerTest::scanQrPayloadSuccessPopulatesContactCardFields()
{
    const QByteArray body = R"({
        "name":"Ada","fingerprint":"ABCD1234","publicKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----",
        "contactCard":{
            "fn":"Ada Lovelace","org":"Analytical Engines Ltd","notes":"Pioneer of computing",
            "emails":[{"label":"work","value":"ada@example.com"}],
            "phones":[{"label":"mobile","value":"+1-555-0100"}],
            "addresses":[{"label":"home","street":"12 Torrington Street","city":"London","country":"UK"}],
            "department":"Engineering","pronouns":"she/her",
            "ims":[{"service":"Matrix","label":"work","value":"@ada:example.org"}],
            "websites":[{"label":"blog","value":"https://ada.example.com"}],
            "relations":[{"label":"spouse","name":"William King"}],
            "events":[{"label":"anniversary","date":"2026-06-01"}],
            "customFields":[{"label":"Employee ID","value":"42"}]
        }
    })";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    const QString qrUrl = QStringLiteral("http://127.0.0.1:%1/api/pgp/qr/key?t=tok-1").arg(fake.port());
    controller.scanQrPayload(qrUrl);

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    const QVariantMap fields = controller.scannedContactCardFields();
    QCOMPARE(fields.value(QStringLiteral("org")).toString(), QStringLiteral("Analytical Engines Ltd"));
    QCOMPARE(fields.value(QStringLiteral("notes")).toString(), QStringLiteral("Pioneer of computing"));
    const QVariantList emails = fields.value(QStringLiteral("emails")).toList();
    QCOMPARE(emails.size(), 1);
    QCOMPARE(emails.first().toMap().value(QStringLiteral("value")).toString(), QStringLiteral("ada@example.com"));

    const QVariantList phones = fields.value(QStringLiteral("phones")).toList();
    QCOMPARE(phones.size(), 1);
    QCOMPARE(phones.first().toMap().value(QStringLiteral("value")).toString(), QStringLiteral("+1-555-0100"));

    const QVariantList addresses = fields.value(QStringLiteral("addresses")).toList();
    QCOMPARE(addresses.size(), 1);
    QCOMPARE(addresses.first().toMap().value(QStringLiteral("street")).toString(),
              QStringLiteral("12 Torrington Street"));
    QCOMPARE(addresses.first().toMap().value(QStringLiteral("country")).toString(), QStringLiteral("UK"));

    QCOMPARE(fields.value(QStringLiteral("department")).toString(), QStringLiteral("Engineering"));
    QCOMPARE(fields.value(QStringLiteral("pronouns")).toString(), QStringLiteral("she/her"));

    const QVariantList ims = fields.value(QStringLiteral("ims")).toList();
    QCOMPARE(ims.size(), 1);
    QCOMPARE(ims.first().toMap().value(QStringLiteral("value")).toString(), QStringLiteral("@ada:example.org"));

    const QVariantList websites = fields.value(QStringLiteral("websites")).toList();
    QCOMPARE(websites.size(), 1);
    QCOMPARE(websites.first().toMap().value(QStringLiteral("value")).toString(), QStringLiteral("https://ada.example.com"));

    const QVariantList relations = fields.value(QStringLiteral("relations")).toList();
    QCOMPARE(relations.size(), 1);
    QCOMPARE(relations.first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("William King"));

    const QVariantList events = fields.value(QStringLiteral("events")).toList();
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().toMap().value(QStringLiteral("date")).toString(), QStringLiteral("2026-06-01"));

    const QVariantList customFields = fields.value(QStringLiteral("customFields")).toList();
    QCOMPARE(customFields.size(), 1);
    QCOMPARE(customFields.first().toMap().value(QStringLiteral("value")).toString(), QStringLiteral("42"));
}

void PgpQrControllerTest::scanQrPayloadWithNoContactCardReturnsAllEmptyFields()
{
    const QByteArray body =
        R"({"name":"Ada","fingerprint":"ABCD1234","publicKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    const QString qrUrl = QStringLiteral("http://127.0.0.1:%1/api/pgp/qr/key?t=tok-1").arg(fake.port());
    controller.scanQrPayload(qrUrl);

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    const QVariantMap fields = controller.scannedContactCardFields();
    QCOMPARE(fields.value(QStringLiteral("org")).toString(), QString());
    QVERIFY(fields.value(QStringLiteral("emails")).toList().isEmpty());
    QVERIFY(fields.value(QStringLiteral("phones")).toList().isEmpty());
    QVERIFY(fields.value(QStringLiteral("addresses")).toList().isEmpty());
    QVERIFY(fields.value(QStringLiteral("ims")).toList().isEmpty());
    QVERIFY(fields.value(QStringLiteral("customFields")).toList().isEmpty());
}

void PgpQrControllerTest::scanQrPayload404SetsFriendlyMessage()
{
    FakeRelayServer fake(httpResponse(404, "Not Found", "no pgp identity configured\n", "text/plain"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    const QString qrUrl = QStringLiteral("http://127.0.0.1:%1/api/pgp/qr/key?t=tok-1").arg(fake.port());
    controller.scanQrPayload(qrUrl);

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    QCOMPARE(controller.lastError(), QStringLiteral("This person hasn't set up PGP encryption yet"));
}

void PgpQrControllerTest::clearScanResultResetsFields()
{
    const QByteArray body =
        R"({"name":"Ada","fingerprint":"ABCD1234","publicKey":"-----BEGIN PGP PUBLIC KEY BLOCK-----"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    const QString qrUrl = QStringLiteral("http://127.0.0.1:%1/api/pgp/qr/key?t=tok-1").arg(fake.port());
    controller.scanQrPayload(qrUrl);

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);
    QVERIFY(!controller.scannedFingerprint().isEmpty());

    controller.clearScanResult();

    QCOMPARE(controller.scannedName(), QString());
    QCOMPARE(controller.scannedFingerprint(), QString());
    QCOMPARE(controller.scannedPublicKey(), QString());
    QCOMPARE(controller.lastError(), QString());
    QVERIFY(controller.scannedContactCardFields().value(QStringLiteral("org")).toString().isEmpty());
}


// The endpoint gate was `path().contains("/api/pgp/qr/key")`. Qt resolves dot
// segments when it puts the request on the wire but keeps them in
// QUrl::path(), so ".../api/pgp/qr/key/../../../../internal/admin" satisfied
// the check while actually requesting "/internal/admin" -- and
// isSafeQrTarget deliberately allows loopback over plain http, where the
// certificate pin never engages.
void PgpQrControllerTest::scanRejectsDotSegmentPathsThatResolveOffTheKeyEndpoint()
{
    FakeRelayServer fake(httpResponse(200, "OK", "{}"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(3000);
    PgpQrController controller(repository, executor);

    controller.scanQrPayload(QStringLiteral("http://127.0.0.1:%1/api/pgp/qr/key/../../../../internal/admin")
                                  .arg(fake.port()));

    // The request is now dispatched off this thread; isBusy is set before
    // the call returns and cleared by the completion handler, so it is the
    // barrier for "the answer has landed".
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 5000);

    // Refused before any request went out.
    QVERIFY(!controller.lastError().isEmpty());
    QVERIFY(fake.receivedRequest().isEmpty());
    QVERIFY(controller.scannedFingerprint().isEmpty());
}

// The point of the conversion: the QR fetch hands control straight back
// instead of sitting inside HttpClient's nested event loop for the length of
// a round trip. PgpMyQrCode.qml calls this from Component.onCompleted, so
// under the old shape opening the screen froze the UI until the relay
// answered.
void PgpQrControllerTest::refreshMyQrCodeReturnsWithoutBlocking()
{
    // Accepts the connection and never answers, so the request runs until
    // the executor's transfer timeout.
    FakeRelayServer fake(QByteArray{});

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);
    savePairing(pairingStore, fake.port());

    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpQrClient client(http);
    PgpQrRepository repository(client, pairingStore);
    NetworkExecutor executor(1500);
    PgpQrController controller(repository, executor);

    QElapsedTimer timer;
    timer.start();
    controller.refreshMyQrCode();
    const qint64 elapsed = timer.elapsed();

    QVERIFY2(elapsed < 200, qPrintable(QStringLiteral("refreshMyQrCode() blocked for %1 ms").arg(elapsed)));
    QVERIFY(controller.isBusy());

    QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 10000);
    QVERIFY(!controller.lastError().isEmpty());
}

// A camera decodes the same QR many times a second, so without coalescing
// every frame would start another fetch of the same key.
//
// Asserted as "ten calls cost exactly what one call costs" rather than as an
// absolute connection count: against a server that accepts and never
// answers, a single request does not map to a single TCP connection --
// Qt retries an idempotent GET after the transfer timeout, which was
// measured here as 3 connections for 1 request. Comparing the two runs
// asserts the property (the extra calls were dropped) without depending on
// Qt's retry behaviour.
void PgpQrControllerTest::repeatedScansWhileOneIsInFlightAreCoalesced()
{
    const auto connectionsForScanCount = [this](int scans) {
        FakeRelayServer fake(QByteArray{});

        QTemporaryDir secureDir;
        [&] { QVERIFY(secureDir.isValid()); }();
        SecureStoreFile secureStore(secureDir.path());
        PairingStore pairingStore(secureStore);

        QNetworkAccessManager manager;
        HttpClient http(manager);
        PgpQrClient client(http);
        PgpQrRepository repository(client, pairingStore);
        NetworkExecutor executor(1500);
        PgpQrController controller(repository, executor);

        const QString url = QStringLiteral("http://127.0.0.1:%1/api/pgp/qr/key?t=tok").arg(fake.port());
        for (int i = 0; i < scans; ++i)
            controller.scanQrPayload(url);

        [&] { QVERIFY(controller.isBusy()); }();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(), 10000); }();
        return fake.connectionCount();
    };

    const int one = connectionsForScanCount(1);
    const int ten = connectionsForScanCount(10);
    QVERIFY(one > 0);
    QCOMPARE(ten, one);
}

QTEST_GUILESS_MAIN(PgpQrControllerTest)
#include "PgpQrControllerTest.moc"

