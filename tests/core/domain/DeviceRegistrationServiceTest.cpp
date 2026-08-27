#include "domain/DeviceRegistrationService.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/CertificatePinSink.h"
#include "net/HttpClient.h"
#include "net/NativeRegistrationClient.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include "../net/FakeRelayServer.h"

#include "stores/SecureStore.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

namespace {

// A SecureStore that accepts reads but refuses every write -- what
// SecureStoreKeychain does on a system with no Secret Service provider
// running (a bare WM session, a locked wallet, a Flatpak on a host with
// neither gnome-keyring nor kwalletd).
class UnwritableSecureStore : public SecureStore
{
public:
    bool set(const QString&, const QString&) override { return false; }
    std::optional<QString> get(const QString& key) const override
    {
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool remove(const QString& key) override
    {
        m_values.remove(key);
        return true;
    }
    bool contains(const QString& key) const override { return m_values.contains(key); }

private:
    QHash<QString, QString> m_values;
};

// A store that refuses exactly one write -- the freshly-minted device
// secret -- and accepts everything else, including the removals and the
// gate-flag reset that PairingStore::clear() performs. That is the shape
// that separates the two persist-failure outcomes: the save is refused, but
// the half-written record can still be taken back.
class SecretRejectingSecureStore : public SecureStore
{
public:
    explicit SecretRejectingSecureStore(QString rejectedValue)
        : m_rejectedValue(std::move(rejectedValue))
    {
    }

    bool set(const QString& key, const QString& value) override
    {
        if (value == m_rejectedValue)
            return false;
        m_values.insert(key, value);
        return true;
    }
    std::optional<QString> get(const QString& key) const override
    {
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool remove(const QString& key) override
    {
        m_values.remove(key);
        return true;
    }
    bool contains(const QString& key) const override { return m_values.contains(key); }

private:
    QString m_rejectedValue;
    QHash<QString, QString> m_values;
};

} // namespace

class DeviceRegistrationServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void successfulPairPersistsPairingAndSettings();
    void pullEndpointFromDifferentOriginThanServerIsRejected();
    void unauthorizedPairLeavesStoresUntouched();
    void reregisterIfPairedSendsStoredCredentialsAndUpdatesDeviceId();
    void reregisterIfPairedWithNoPriorPairingMakesNoRequest();
    void reregisterIfPairedOn401LeavesStoredPairingUnchanged();

    // Review-finding regressions.
    void pairFailsWhenCredentialsCannotBePersisted();
    void pairReportsARemovedHalfWrittenRecordSeparately();
    void reregisterIsDeferredWithoutContactingTheServerWhileCredentialsAreSealed();
    void failedReregistrationKeepsTheCertificatePinEnforced();
    void unpersistableRegistrationKeepsTheCertificatePinEnforced();
    void deferredRegistrationKeepsTheCertificatePinEnforced();

    // The async three-phase split.
    void anAbandonedPairAttemptRestoresThePin();
    void aPairAttemptRefusedByTheGuardsNeverClearsThePin();

private:
    static PairingParams sampleParams(quint16 port);
};

PairingParams DeviceRegistrationServiceTest::sampleParams(quint16 port)
{
    PairingParams params;
    params.subscriberId = QStringLiteral("sub-1");
    params.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    params.registrationUrl = QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(port);
    params.pairingToken = QStringLiteral("pair-tok");
    params.deviceName = QStringLiteral("My Linux Desktop");
    return params;
}

void DeviceRegistrationServiceTest::successfulPairPersistsPairingAndSettings()
{
    // pullEndpoint deliberately shares origin with a fixed serverBaseUrl
    // literal (not the fake server's own dynamic port -- registrationUrl,
    // set separately below, is what actually gets contacted) -- exercises
    // the "accepted as-is" path of the sameOrigin() check in
    // DeviceRegistrationService::pair().
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-1","deviceSecret":"fresh-device-secret",)"
                             R"("devices":1,"deliveryMode":"pull",)"
                             R"("pullEndpoint":"http://relay.example:9443/custom/pull","transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    PairingParams params = sampleParams(fake.port());
    params.serverBaseUrl = QStringLiteral("http://relay.example:9443");

    const NativeRegistrationResult result = service.pair(params, QStringLiteral("https://push.example/endpoint"));

    QCOMPARE(result.outcome, RegistrationOutcome::Success);

    // Verify persistence via a second PairingStore instance over the same
    // SecureStoreFile directory, proving the write actually landed on disk
    // rather than merely being visible through the original instance.
    PairingStore verifyPairingStore(secureStore);
    const std::optional<DevicePairing> loaded = verifyPairingStore.load();
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->subscriberId, QStringLiteral("sub-1"));
    QCOMPARE(loaded->deviceSecret, QStringLiteral("fresh-device-secret"));
    QCOMPARE(loaded->serverBaseUrl, params.serverBaseUrl);
    QCOMPARE(loaded->registrationUrl, params.registrationUrl);
    QCOMPARE(loaded->pairingToken, QStringLiteral("pair-tok"));
    QCOMPARE(loaded->deviceId, QStringLiteral("dev-1"));
    QCOMPARE(loaded->deviceName, QStringLiteral("My Linux Desktop"));

    QCOMPARE(settingsStore.deliveryMode(), QStringLiteral("pull"));
    QCOMPARE(settingsStore.pullEndpoint(), QStringLiteral("http://relay.example:9443/custom/pull"));
    QCOMPARE(settingsStore.transport(), QStringLiteral("unifiedpush"));
}

void DeviceRegistrationServiceTest::pullEndpointFromDifferentOriginThanServerIsRejected()
{
    // VibeSec regression guard: a pullEndpoint on a different host than the
    // server the user actually paired with must be ignored in favor of a
    // same-origin derived one, not persisted verbatim -- otherwise a
    // compromised/malicious relay could silently redirect all future
    // credentialed polling (deviceId/deviceSecret ride along on every pull)
    // to an arbitrary attacker host.
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-1","deviceSecret":"sec-1","devices":1,)"
                             R"("deliveryMode":"pull","pullEndpoint":"http://attacker.example/steal",)"
                             R"("transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const NativeRegistrationResult result =
        service.pair(sampleParams(fake.port()), QStringLiteral("https://push.example/endpoint"));

    QCOMPARE(result.outcome, RegistrationOutcome::Success);
    QCOMPARE(settingsStore.pullEndpoint(),
             QStringLiteral("http://127.0.0.1:%1/api/notifications/native/pull").arg(fake.port()));
}

void DeviceRegistrationServiceTest::unauthorizedPairLeavesStoresUntouched()
{
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "{}"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const NativeRegistrationResult result =
        service.pair(sampleParams(fake.port()), QStringLiteral("https://push.example/endpoint"));

    QCOMPARE(result.outcome, RegistrationOutcome::Unauthorized);
    QVERIFY(!pairingStore.load().has_value());
    QVERIFY(settingsStore.deliveryMode().isEmpty());
}

void DeviceRegistrationServiceTest::reregisterIfPairedSendsStoredCredentialsAndUpdatesDeviceId()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    DevicePairing existing;
    existing.subscriberId = QStringLiteral("sub-existing");
    existing.serverBaseUrl = QStringLiteral("http://127.0.0.1:1");
    existing.registrationUrl = QStringLiteral("http://placeholder/api/notifications/native/register");
    existing.pairingToken = QStringLiteral("existing-token");
    existing.deviceId = QStringLiteral("old-device-id");
    existing.deviceName = QStringLiteral("Existing Desktop");
    existing.deviceSecret = QStringLiteral("old-device-secret");
    QVERIFY(pairingStore.save(existing));

    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"new-device-id","deviceSecret":"new-device-secret",)"
                             R"("devices":1,"deliveryMode":"push",)"
                             R"("pullEndpoint":"http://relay.example/api/notifications/native/pull","transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    // registrationUrl in the stored pairing points nowhere real; the
    // service must use it verbatim, so re-point the stored pairing at the
    // fake server before triggering reregisterIfPaired().
    existing.registrationUrl = QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port());
    QVERIFY(pairingStore.save(existing));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const std::optional<NativeRegistrationResult> result =
        service.reregisterIfPaired(QStringLiteral("https://push.example/endpoint"));

    QVERIFY(result.has_value());
    QCOMPARE(result->outcome, RegistrationOutcome::Success);

    const QJsonObject sent = fake.receivedJsonBody();
    QCOMPARE(sent.value(QStringLiteral("subscriberId")).toString(), QStringLiteral("sub-existing"));
    QVERIFY(!sent.contains(QStringLiteral("subscriberHash")));
    QCOMPARE(sent.value(QStringLiteral("pairingToken")).toString(), QStringLiteral("existing-token"));

    const std::optional<DevicePairing> loaded = pairingStore.load();
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->deviceId, QStringLiteral("new-device-id"));
    // Every successful register mints a brand-new secret, invalidating the
    // previous one -- the stored value must be overwritten, not kept.
    QCOMPARE(loaded->deviceSecret, QStringLiteral("new-device-secret"));
}

void DeviceRegistrationServiceTest::reregisterIfPairedWithNoPriorPairingMakesNoRequest()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const std::optional<NativeRegistrationResult> result =
        service.reregisterIfPaired(QStringLiteral("https://push.example/endpoint"));

    QVERIFY(!result.has_value());
    QVERIFY(fake.receivedRequest().isEmpty());
}

void DeviceRegistrationServiceTest::reregisterIfPairedOn401LeavesStoredPairingUnchanged()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    FakeRelayServer fake(httpResponse(401, "Unauthorized", "{}"));

    DevicePairing existing;
    existing.subscriberId = QStringLiteral("sub-existing");
    existing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    existing.registrationUrl = QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port());
    existing.pairingToken = QStringLiteral("existing-token");
    existing.deviceId = QStringLiteral("old-device-id");
    existing.deviceName = QStringLiteral("Existing Desktop");
    existing.deviceSecret = QStringLiteral("existing-device-secret");
    QVERIFY(pairingStore.save(existing));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const std::optional<NativeRegistrationResult> result =
        service.reregisterIfPaired(QStringLiteral("https://push.example/endpoint"));

    QVERIFY(result.has_value());
    QCOMPARE(result->outcome, RegistrationOutcome::Unauthorized);

    const std::optional<DevicePairing> loaded = pairingStore.load();
    QVERIFY(loaded.has_value());
    QCOMPARE(*loaded, existing);
}


// Certificate pinning must survive a registration that does not complete.
//
// pair() has to clear the pin before it registers -- the registration is
// what establishes the new trust anchor, and the certificate-mismatch
// banner's own "unpair and pair again" advice is unfollowable otherwise.
// But it only re-armed the pin on the success path, so EVERY other exit left
// enforcement off for the rest of the process, silently.
//
// That is remotely reachable rather than theoretical: reregisterIfPaired()
// runs unattended on every push-endpoint rotation and tier change, so an
// on-path attacker who can make one of those fail (drop the connection,
// answer 500, answer non-JSON) disarms pinning and can then present any
// CA-issued certificate to a client that keeps sending
// X-Kypost-Device-Secret. The mismatch banner never fires, because there is
// no longer a pin to mismatch.
void DeviceRegistrationServiceTest::failedReregistrationKeepsTheCertificatePinEnforced()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    FakeRelayServer fake(httpResponse(500, "Internal Server Error", "{}"));

    DevicePairing existing;
    existing.subscriberId = QStringLiteral("sub-existing");
    existing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    existing.registrationUrl = QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port());
    existing.pairingToken = QStringLiteral("existing-token");
    existing.deviceId = QStringLiteral("old-device-id");
    existing.deviceSecret = QStringLiteral("existing-device-secret");
    QVERIFY(pairingStore.save(existing));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const QByteArray pin = QByteArray(32, 'A');
    const QUrl pinnedOrigin(existing.serverBaseUrl);
    http.setCertificatePin(pin, pinnedOrigin);

    const std::optional<NativeRegistrationResult> result =
        service.reregisterIfPaired(QStringLiteral("https://push.example/endpoint"));
    QVERIFY(result.has_value());
    QVERIFY(result->outcome != RegistrationOutcome::Success);

    // Both halves: the bytes AND the origin they are scoped to. A pin
    // restored without its origin enforces against nothing.
    QCOMPARE(http.certificatePin(), pin);
    QCOMPARE(http.certificatePinState().origin, pinnedOrigin);
    QVERIFY(http.certificatePinState().isEnforcing());
}

// Same requirement on the "registered, but the store refused the write"
// path, which returns Failure from further down the function.
void DeviceRegistrationServiceTest::unpersistableRegistrationKeepsTheCertificatePinEnforced()
{
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-1","deviceSecret":"fresh-device-secret",)"
                             R"("devices":1,"deliveryMode":"pull","transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    UnwritableSecureStore secureStore;
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const QByteArray pin = QByteArray(32, 'B');
    const QUrl pinnedOrigin(QStringLiteral("https://relay.example"));
    http.setCertificatePin(pin, pinnedOrigin);

    const NativeRegistrationResult result = service.pair(sampleParams(fake.port()), QStringLiteral("tok"));
    QCOMPARE(result.outcome, RegistrationOutcome::CredentialsNotSavedAndNotCleared);

    QCOMPARE(http.certificatePin(), pin);
    QCOMPARE(http.certificatePinState().origin, pinnedOrigin);
}

// And on the earliest exit of all -- the sealed-credentials deferral, which
// returns before the network is touched. It never cleared the pin, but the
// guard now owns that decision, so pin the behaviour rather than the
// implementation detail that used to make it true.
void DeviceRegistrationServiceTest::deferredRegistrationKeepsTheCertificatePinEnforced()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    DevicePairing existing;
    existing.subscriberId = QStringLiteral("sub-existing");
    existing.serverBaseUrl = QStringLiteral("https://relay.example");
    existing.registrationUrl = QStringLiteral("https://relay.example/api/notifications/native/register");
    existing.deviceId = QStringLiteral("dev");
    existing.deviceSecret = QStringLiteral("secret");
    QVERIFY(pairingStore.save(existing));
    // Seal, then re-lock: the gate is on and this session holds no key, so
    // pair() must defer.
    QVERIFY(pairingStore.sealDeviceSecret(QStringLiteral("428391")));
    pairingStore.lockDeviceSecret();

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const QByteArray pin = QByteArray(32, 'C');
    const QUrl pinnedOrigin(existing.serverBaseUrl);
    http.setCertificatePin(pin, pinnedOrigin);

    const std::optional<NativeRegistrationResult> result =
        service.reregisterIfPaired(QStringLiteral("https://push.example/endpoint"));
    QVERIFY(result.has_value());
    QCOMPARE(result->outcome, RegistrationOutcome::CredentialsLocked);

    QCOMPARE(http.certificatePin(), pin);
    QCOMPARE(http.certificatePinState().origin, pinnedOrigin);
}

// A registration that reaches the server but cannot be written to disk must
// NOT report success.
//
// The save() result used to be discarded. The server minted and burned a
// one-shot deviceSecret, nothing landed in the store, and PairingController
// went to pairingState="paired" and showed the user a success screen. The
// next launch was unpaired -- and re-pairing failed too, because the pairing
// token had already been consumed by the attempt that "worked".
void DeviceRegistrationServiceTest::pairFailsWhenCredentialsCannotBePersisted()
{
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-1","deviceSecret":"fresh-device-secret",)"
                             R"("devices":1,"deliveryMode":"pull","transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    UnwritableSecureStore secureStore;
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);
    const NativeRegistrationResult result = service.pair(sampleParams(fake.port()), QStringLiteral("tok"));

    // The user is still told WHY, but the outcome carries the fact and
    // PairingController carries the sentence -- core/ cannot call i18n(), so
    // an English `detail` here reached the UI untranslated (AGENTS.md 6c).
    // This store also refuses the credential-gate reset inside clear(), so
    // the half-written record could not be taken back either.
    QCOMPARE(result.outcome, RegistrationOutcome::CredentialsNotSavedAndNotCleared);
    QVERIFY2(result.detail.isEmpty(), "core/ must not hand the UI an untranslatable English sentence");

    // And nothing half-written is left behind claiming to be a pairing.
    QVERIFY(!pairingStore.isPaired());
}

// The other half of the same failure: the store refused the secret, but the
// partial record WAS removed. A single outcome for both would have to claim
// one of the two, and telling a user their device still holds a broken
// pairing record when it does not is the kind of wording that sends them
// looking for something to delete.
void DeviceRegistrationServiceTest::pairReportsARemovedHalfWrittenRecordSeparately()
{
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-1","deviceSecret":"fresh-device-secret",)"
                             R"("devices":1,"deliveryMode":"pull","transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);

    SecretRejectingSecureStore secureStore(QStringLiteral("fresh-device-secret"));
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);
    const NativeRegistrationResult result = service.pair(sampleParams(fake.port()), QStringLiteral("tok"));

    QCOMPARE(result.outcome, RegistrationOutcome::CredentialsNotSaved);
    QVERIFY2(result.detail.isEmpty(), "core/ must not hand the UI an untranslatable English sentence");
    QVERIFY(!pairingStore.isPaired());
}

void DeviceRegistrationServiceTest::reregisterIsDeferredWithoutContactingTheServerWhileCredentialsAreSealed()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    // A server that WOULD succeed, so a request reaching it is unambiguous.
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"ok":true,"deviceId":"new-device-id","deviceSecret":"rotated-secret","deliveryMode":"push","transport":"unifiedpush"})"));

    DevicePairing existing;
    existing.subscriberId = QStringLiteral("sub-existing");
    existing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    existing.registrationUrl = QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port());
    existing.pairingToken = QStringLiteral("existing-token");
    existing.deviceId = QStringLiteral("old-device-id");
    existing.deviceSecret = QStringLiteral("existing-device-secret");
    QVERIFY(pairingStore.save(existing));

    // Credential gate on, app locked -- the state every launch starts in
    // once the user has enabled it.
    QVERIFY(pairingStore.sealDeviceSecret(QStringLiteral("123456")));
    pairingStore.lockDeviceSecret();

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const std::optional<NativeRegistrationResult> result =
        service.reregisterIfPaired(QStringLiteral("https://push.example/endpoint"));

    QVERIFY(result.has_value());
    QCOMPARE(result->outcome, RegistrationOutcome::CredentialsLocked);

    // The point of checking BEFORE the request: a successful register mints
    // a new secret and retires the old one server-side. Discovering
    // afterwards that it cannot be sealed would leave this device holding a
    // credential the relay no longer accepts -- so nothing may be sent.
    QVERIFY(fake.receivedRequest().isEmpty());

    // The sealed blob and the gate are both untouched, and no plaintext
    // secret appeared on disk.
    QVERIFY(pairingStore.deviceSecretSealed());
    const std::optional<QString> raw = secureStore.get(QStringLiteral("pairing.deviceSecret"));
    QVERIFY(!raw.has_value() || raw->isEmpty());

    // ...and once unlocked, the same call goes through normally.
    QVERIFY(pairingStore.unsealDeviceSecret(QStringLiteral("123456")));
    const std::optional<NativeRegistrationResult> retried =
        service.reregisterIfPaired(QStringLiteral("https://push.example/endpoint"));
    QVERIFY(retried.has_value());
    QCOMPARE(retried->outcome, RegistrationOutcome::Success);
    QVERIFY(!fake.receivedRequest().isEmpty());
    // Rotated, and still sealed rather than written out in the clear.
    QVERIFY(pairingStore.deviceSecretSealed());
    QCOMPARE(pairingStore.load()->deviceSecret, QStringLiteral("rotated-secret"));
    const std::optional<QString> rawAfter = secureStore.get(QStringLiteral("pairing.deviceSecret"));
    QVERIFY(!rawAfter.has_value() || rawAfter->isEmpty());
}

// The hazard the async split introduces.
//
// beginPair() suspends the certificate pin and finishPair() decides whether
// to restore it or keep a new one. Between those two the request is out on
// another thread, so the attempt can simply be dropped -- the executor shuts
// down, the controller is torn down, the process is quitting. If that left
// the pin cleared, an abandoned pairing would silently disable pinning for
// the rest of the session: exactly the defect ScopedPinSuspension was added
// to fix, reintroduced by making the gap asynchronous.
void DeviceRegistrationServiceTest::anAbandonedPairAttemptRestoresThePin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const QByteArray pin(32, 'A');
    const QUrl origin(QStringLiteral("https://relay.example"));
    pinSink.setPin(pin, origin);

    {
        DeviceRegistrationService::PairAttempt attempt = service.beginPair();
        QVERIFY(!attempt.refusedOutcome().has_value());
        // Suspended for the duration of the request, as the synchronous form
        // does -- the old pin must not abort the handshake that establishes
        // the new anchor.
        QVERIFY(pinSink.pinState().spkiSha256.isEmpty());
        // ...and then dropped without ever calling finishPair().
    }

    QCOMPARE(pinSink.pinState().spkiSha256, pin);
    QCOMPARE(pinSink.pinState().origin, origin);

    // Moving it must neither double-restore nor lose the restore: the
    // attempt is handed to a completion handler by value.
    {
        DeviceRegistrationService::PairAttempt first = service.beginPair();
        QVERIFY(pinSink.pinState().spkiSha256.isEmpty());
        DeviceRegistrationService::PairAttempt second = std::move(first);
        QVERIFY(pinSink.pinState().spkiSha256.isEmpty());
    }
    QCOMPARE(pinSink.pinState().spkiSha256, pin);
}

// The guards refuse before the pin is touched at all, so a deferred attempt
// leaves enforcement exactly as it found it.
void DeviceRegistrationServiceTest::aPairAttemptRefusedByTheGuardsNeverClearsThePin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));

    DevicePairing existing;
    existing.subscriberId = QStringLiteral("sub-1");
    existing.serverBaseUrl = QStringLiteral("https://relay.example");
    existing.deviceId = QStringLiteral("dev");
    existing.deviceSecret = QStringLiteral("secret");
    QVERIFY(pairingStore.save(existing));
    QVERIFY(pairingStore.sealDeviceSecret(QStringLiteral("428391")));
    pairingStore.lockDeviceSecret(); // gate on, no session key -> must defer

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    HttpClientPinSink pinSink(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, pinSink);

    const QByteArray pin(32, 'B');
    pinSink.setPin(pin, QUrl(existing.serverBaseUrl));

    DeviceRegistrationService::PairAttempt attempt = service.beginPair();
    QCOMPARE(attempt.refusedOutcome().value(), RegistrationOutcome::CredentialsLocked);
    QCOMPARE(pinSink.pinState().spkiSha256, pin);
}

QTEST_GUILESS_MAIN(DeviceRegistrationServiceTest)
#include "DeviceRegistrationServiceTest.moc"
