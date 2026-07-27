#include "pairing/PairingController.h"

#include "domain/DeviceRegistrationService.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/DeregisterClient.h"
#include "net/HttpClient.h"
#include "net/NativeRegistrationClient.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include "../../core/net/FakeRelayServer.h"

#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>

class PairingControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void pairFromDeepLinkEntersConfirmStateWithoutNetworkCall();
    void confirmPendingPairWithNoPendingRequestFails();
    void cancelPendingPairDiscardsRequestWithNoNetworkCall();
    void pairFromDeepLinkHappyPathPairsAndPersists();
    void pairFromDeepLinkSendsDeviceTokenWhenSet();
    void pairFromDeepLinkDerivesRegistrationUrlFromSrvWhenRegOmitted();
    void pairFromDeepLinkMissingRequiredParam_data();
    void pairFromDeepLinkMissingRequiredParam();
    void pairFromDeepLinkRejectsNonNativePairHost();
    void pairFromDeepLinkRejectsPlaintextHttpServerUrl();
    void pairFromDeepLinkAllowsPlaintextHttpForLoopbackServerUrl();
    void pairFromDeepLinkRejectsRegOnDifferentOriginThanSrv();
    void pairFromDeepLinkNotifiesFreshPendingPairEvenWhenStateLabelUnchanged();
    void pairFromPastedLinkRejectsNonLinkTextWithNoNetworkCall();
    void refreshFromStoreReflectsPreSeededPairingStoreAndRemovePairingClears();
    void removePairingSkipsNetworkCallWhenNoDeviceSecretStored();
    void removePairingDeregistersServerSideWhenDeviceSecretPresent();
    void resetReturnsToIdleAfterFailure();
    void pairFromDeepLinkRefusedWhileAppLocked();
    void confirmPendingPairRefusedWhenAppLocksAfterConfirmStateEntered();
    void pairFromDeepLinkWorksAgainAfterUnlock();
    void pendingPairOriginDisclosesSchemeAndPort();

private:
    // Builds a kypost://native-pair?... link from a param map, letting
    // callers omit keys to exercise the missing-required-param path.
    static QUrl buildLink(const QMap<QString, QString>& params);
};

QUrl PairingControllerTest::buildLink(const QMap<QString, QString>& params)
{
    QUrl url;
    url.setScheme(QStringLiteral("kypost"));
    url.setHost(QStringLiteral("native-pair"));
    QUrlQuery query;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        query.addQueryItem(it.key(), it.value());
    url.setQuery(query);
    return url;
}

void PairingControllerTest::pairFromDeepLinkEntersConfirmStateWithoutNetworkCall()
{
    // VibeSec regression guard: a recognized link must never hit the
    // network until confirmPendingPair() is called explicitly -- see
    // PairingController::pairFromDeepLink's doc comment.
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"deviceId":"should-not-be-used"})"));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-confirm");
    params[QStringLiteral("srv")] = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-confirm");

    QVERIFY(controller.pairFromDeepLink(buildLink(params)));

    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));
    QCOMPARE(controller.pendingPairOrigin(), QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    QVERIFY(!controller.isPaired());
    QVERIFY(fake.receivedRequest().isEmpty());
    QVERIFY(!pairingStore.load().has_value());
}

void PairingControllerTest::confirmPendingPairWithNoPendingRequestFails()
{
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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QVERIFY(!controller.confirmPendingPair());
    QCOMPARE(controller.pairingState(), QStringLiteral("failed"));
}

void PairingControllerTest::cancelPendingPairDiscardsRequestWithNoNetworkCall()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"deviceId":"should-not-be-used"})"));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-cancel");
    params[QStringLiteral("srv")] = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-cancel");

    QVERIFY(controller.pairFromDeepLink(buildLink(params)));
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));

    controller.cancelPendingPair();

    QCOMPARE(controller.pairingState(), QStringLiteral("idle"));
    QVERIFY(!controller.isPaired());
    QVERIFY(fake.receivedRequest().isEmpty());
    QVERIFY(!pairingStore.load().has_value());

    // The pending request is gone -- a later confirm has nothing to act on.
    QVERIFY(!controller.confirmPendingPair());
    QVERIFY(fake.receivedRequest().isEmpty());
}

void PairingControllerTest::pairFromDeepLinkHappyPathPairsAndPersists()
{
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-1","deviceSecret":"fresh-device-secret",)"
                             R"("devices":1,"deliveryMode":"pull",)"
                             R"("pullEndpoint":"http://relay.example/api/notifications/native/pull",)"
                             R"("transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);
    QVERIFY(!controller.isPaired());
    QCOMPARE(controller.pairingState(), QStringLiteral("idle"));

    QSignalSpy pairingChangedSpy(&controller, &PairingController::pairingChanged);
    QSignalSpy stateChangedSpy(&controller, &PairingController::pairingStateChanged);

    const QString serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    const QString registrationUrl = serverBaseUrl + QStringLiteral("/api/notifications/native/register");

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-1");
    params[QStringLiteral("srv")] = serverBaseUrl;
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok");
    params[QStringLiteral("reg")] = registrationUrl;

    QVERIFY(controller.pairFromDeepLink(buildLink(params)));

    // VibeSec fix: a recognized link waits for explicit confirmation before
    // any network call -- see PairingController::pairFromDeepLink's doc
    // comment.
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));
    QCOMPARE(controller.pendingPairOrigin(), QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    QVERIFY(fake.receivedRequest().isEmpty());

    QVERIFY(controller.confirmPendingPair());

    QCOMPARE(controller.pairingState(), QStringLiteral("paired"));
    QVERIFY(controller.pairingError().isEmpty());
    QVERIFY(controller.isPaired());
    QCOMPARE(controller.deviceId(), QStringLiteral("dev-1"));
    QCOMPARE(controller.pairedServerHost(), QStringLiteral("127.0.0.1"));
    // Task 39: deliveryMode/transport read straight through SettingsStore,
    // written by DeviceRegistrationService::pair() from this same response
    // body ("deliveryMode":"pull","transport":"unifiedpush" above).
    QCOMPARE(controller.deliveryMode(), QStringLiteral("pull"));
    QCOMPARE(controller.transport(), QStringLiteral("unifiedpush"));
    // "confirm" then "working" then "paired" -- at least three transitions.
    QVERIFY(stateChangedSpy.count() >= 3);
    QVERIFY(pairingChangedSpy.count() >= 1);

    const std::optional<DevicePairing> loaded = pairingStore.load();
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->subscriberId, QStringLiteral("sub-1"));
    QCOMPARE(loaded->deviceSecret, QStringLiteral("fresh-device-secret"));
    QCOMPARE(loaded->registrationUrl, registrationUrl);
    QCOMPARE(loaded->deviceId, QStringLiteral("dev-1"));

    const QJsonObject sent = fake.receivedJsonBody();
    QCOMPARE(sent.value(QStringLiteral("subscriberId")).toString(), QStringLiteral("sub-1"));
    QVERIFY(!sent.contains(QStringLiteral("subscriberHash")));
    QCOMPARE(sent.value(QStringLiteral("pairingToken")).toString(), QStringLiteral("pair-tok"));
    // This test never calls setDeviceToken(), so m_deviceToken stays at its
    // default-constructed empty QString() -- verifies the no-endpoint-yet
    // case (e.g. pairing completes before UnifiedPushConnector has ever
    // reported an endpoint). See pairFromDeepLinkSendsDeviceTokenWhenSet
    // below for the real-endpoint case.
    QCOMPARE(sent.value(QStringLiteral("deviceToken")).toString(), QString());
}

void PairingControllerTest::pairFromDeepLinkSendsDeviceTokenWhenSet()
{
    // Task 43 regression guard: when setDeviceToken() has been called (as
    // main.cpp does whenever UnifiedPushConnector reports an endpoint,
    // including once immediately after pushConnector's construction --
    // see PairingController.h's class doc comment), pairFromParsedParams()
    // must send that value as deviceToken rather than QString(). Reverting
    // the Task 43 fix (passing QString() unconditionally instead of
    // m_deviceToken) would fail this test while leaving
    // pairFromDeepLinkHappyPathPairsAndPersists above green.
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-4","deviceSecret":"secret-4",)"
                             R"("devices":1,"deliveryMode":"pull",)"
                             R"("pullEndpoint":"http://relay.example/api/notifications/native/pull",)"
                             R"("transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);
    controller.setDeviceToken(QStringLiteral("some-real-endpoint"));

    const QString serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    const QString registrationUrl = serverBaseUrl + QStringLiteral("/api/notifications/native/register");

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-4");
    params[QStringLiteral("srv")] = serverBaseUrl;
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-4");
    params[QStringLiteral("reg")] = registrationUrl;

    QVERIFY(controller.pairFromDeepLink(buildLink(params)));
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));
    QVERIFY(controller.confirmPendingPair());
    QCOMPARE(controller.pairingState(), QStringLiteral("paired"));

    const QJsonObject sent = fake.receivedJsonBody();
    QCOMPARE(sent.value(QStringLiteral("deviceToken")).toString(), QStringLiteral("some-real-endpoint"));
}

void PairingControllerTest::pairFromDeepLinkDerivesRegistrationUrlFromSrvWhenRegOmitted()
{
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-2","deviceSecret":"secret-2",)"
                             R"("devices":1,"deliveryMode":"pull",)"
                             R"("pullEndpoint":"http://relay.example/api/notifications/native/pull",)"
                             R"("transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    // Trailing slash on srv exercises the strip-trailing-slash rule too.
    const QString serverBaseUrl = QStringLiteral("http://127.0.0.1:%1/").arg(fake.port());

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-2");
    params[QStringLiteral("srv")] = serverBaseUrl;
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-2");
    // reg deliberately omitted.

    QVERIFY(controller.pairFromDeepLink(buildLink(params)));
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));
    QVERIFY(controller.confirmPendingPair());

    const std::optional<DevicePairing> loaded = pairingStore.load();
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->registrationUrl,
             QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port()));

    // The derived path is what the request actually hit, not just what got
    // persisted.
    QVERIFY(fake.receivedRequest().contains("POST /api/notifications/native/register HTTP/1.1"));
}

void PairingControllerTest::pairFromDeepLinkMissingRequiredParam_data()
{
    QTest::addColumn<QString>("omittedKey");
    QTest::newRow("sub missing") << QStringLiteral("sub");
    QTest::newRow("srv missing") << QStringLiteral("srv");
    QTest::newRow("pt missing") << QStringLiteral("pt");
}

void PairingControllerTest::pairFromDeepLinkMissingRequiredParam()
{
    QFETCH(QString, omittedKey);

    // Response would signal success if hit -- the test only passes if it's
    // never hit at all.
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"deviceId":"should-not-be-used"})"));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-x");
    params[QStringLiteral("srv")] = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-x");
    params.remove(omittedKey);

    QSignalSpy stateChangedSpy(&controller, &PairingController::pairingStateChanged);

    QVERIFY(!controller.pairFromDeepLink(buildLink(params)));

    QCOMPARE(controller.pairingState(), QStringLiteral("failed"));
    QVERIFY(!controller.pairingError().isEmpty());
    QVERIFY(!controller.isPaired());
    QCOMPARE(stateChangedSpy.count(), 1); // idle -> failed directly, no "working" in between
    QVERIFY(fake.receivedRequest().isEmpty()); // zero network calls
    QVERIFY(!pairingStore.load().has_value());
}

void PairingControllerTest::pairFromDeepLinkRejectsNonNativePairHost()
{
    // kypost://desktop-pair is explicitly out of scope per Phase 6
    // global constraint 6 -- must be treated as unrecognized, not routed.
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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QUrl link;
    link.setScheme(QStringLiteral("kypost"));
    link.setHost(QStringLiteral("desktop-pair"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("sub"), QStringLiteral("sub-1"));
    query.addQueryItem(QStringLiteral("srv"), QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    query.addQueryItem(QStringLiteral("pt"), QStringLiteral("pair-tok"));
    link.setQuery(query);

    QVERIFY(!controller.pairFromDeepLink(link));
    QCOMPARE(controller.pairingState(), QStringLiteral("failed"));
    QVERIFY(fake.receivedRequest().isEmpty());
}

void PairingControllerTest::pairFromDeepLinkRejectsPlaintextHttpServerUrl()
{
    // VibeSec regression guard: a MITM who rewrites an otherwise-legitimate
    // https:// pairing link to http:// must not be able to make the app
    // pair (and send the pairing token + real push deviceToken) in
    // cleartext -- see PairingController.cpp's parseNativePairLink.
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"deviceId":"should-not-be-used"})"));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-http");
    // A non-loopback host over plaintext http -- must be rejected outright.
    params[QStringLiteral("srv")] = QStringLiteral("http://relay.example.com");
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-http");

    QVERIFY(!controller.pairFromDeepLink(buildLink(params)));

    QCOMPARE(controller.pairingState(), QStringLiteral("failed"));
    QVERIFY(!controller.isPaired());
    QVERIFY(fake.receivedRequest().isEmpty());
    QVERIFY(!pairingStore.load().has_value());
}

void PairingControllerTest::pairFromDeepLinkAllowsPlaintextHttpForLoopbackServerUrl()
{
    // Local/self-hosted development relays (and every other test in this
    // file) legitimately use http://127.0.0.1:<port> -- the https-only rule
    // must carve out loopback, not just accept https everywhere.
    const QByteArray body = R"({"ok":true,"synced":true,"deviceId":"dev-loop","deviceSecret":"secret-loop",)"
                             R"("devices":1,"deliveryMode":"pull",)"
                             R"("pullEndpoint":"http://relay.example/api/notifications/native/pull",)"
                             R"("transport":"unifiedpush"})";
    FakeRelayServer fake(httpResponse(200, "OK", body));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-loop");
    params[QStringLiteral("srv")] = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-loop");

    QVERIFY(controller.pairFromDeepLink(buildLink(params)));
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));
    QVERIFY(controller.confirmPendingPair());
    QCOMPARE(controller.pairingState(), QStringLiteral("paired"));
}

void PairingControllerTest::pairFromDeepLinkRejectsRegOnDifferentOriginThanSrv()
{
    // VibeSec regression guard: `reg` used to be able to point the actual
    // registration POST (carrying the subscriberId/pairingToken/real push
    // deviceToken) at a completely different host than `srv`, the only
    // value the confirm dialog ever displays -- see
    // PairingController.cpp's parseNativePairLink.
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"deviceId":"should-not-be-used"})"));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QMap<QString, QString> params;
    params[QStringLiteral("sub")] = QStringLiteral("sub-crossorigin");
    params[QStringLiteral("srv")] = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-crossorigin");
    // reg points at a different host entirely -- the confirm dialog would
    // still only ever show "127.0.0.1" from srv above.
    params[QStringLiteral("reg")] = QStringLiteral("http://attacker.example/register");

    QVERIFY(!controller.pairFromDeepLink(buildLink(params)));

    QCOMPARE(controller.pairingState(), QStringLiteral("failed"));
    QVERIFY(!controller.isPaired());
    QVERIFY(fake.receivedRequest().isEmpty());
    QVERIFY(!pairingStore.load().has_value());
}

void PairingControllerTest::pairFromDeepLinkNotifiesFreshPendingPairEvenWhenStateLabelUnchanged()
{
    // VibeSec regression guard: setPairingState() used to dedup on
    // (state, error) alone, so a SECOND kypost://native-pair link arriving
    // while the confirm dialog was already open (same "confirm"/"" state)
    // silently swapped m_pendingPair to the new (attacker) link's params
    // without ever emitting pairingStateChanged() -- pendingPairOrigin's
    // QML binding never re-evaluated, so the dialog kept showing the FIRST
    // link's host while "Pair" would have acted on the SECOND link's data.
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"deviceId":"should-not-be-used"})"));

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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QMap<QString, QString> firstParams;
    firstParams[QStringLiteral("sub")] = QStringLiteral("sub-first");
    firstParams[QStringLiteral("srv")] = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    firstParams[QStringLiteral("pt")] = QStringLiteral("pair-tok-first");

    QVERIFY(controller.pairFromDeepLink(buildLink(firstParams)));
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));
    QCOMPARE(controller.pendingPairOrigin(), QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));

    QSignalSpy stateChangedSpy(&controller, &PairingController::pairingStateChanged);

    QMap<QString, QString> secondParams;
    secondParams[QStringLiteral("sub")] = QStringLiteral("sub-second");
    secondParams[QStringLiteral("srv")] = QStringLiteral("https://192.0.2.1"); // TEST-NET-1, deliberately unreachable
    secondParams[QStringLiteral("pt")] = QStringLiteral("pair-tok-second");

    QVERIFY(controller.pairFromDeepLink(buildLink(secondParams)));

    // Still "confirm" (same label), but pairingStateChanged MUST fire again
    // so a bound QML label re-reads pendingPairOrigin -- otherwise the UI
    // shows stale (first link's) data while the pending params underneath
    // have already moved to the second link's.
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));
    QCOMPARE(controller.pendingPairOrigin(), QStringLiteral("https://192.0.2.1"));
    QVERIFY(stateChangedSpy.count() >= 1);
}

void PairingControllerTest::pairFromPastedLinkRejectsNonLinkTextWithNoNetworkCall()
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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QVERIFY(!controller.pairFromPastedLink(QStringLiteral("this is not a pairing link")));
    QCOMPARE(controller.pairingState(), QStringLiteral("failed"));
    QVERIFY(fake.receivedRequest().isEmpty());
}

void PairingControllerTest::refreshFromStoreReflectsPreSeededPairingStoreAndRemovePairingClears()
{
    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-seed");
    pairing.serverBaseUrl = QStringLiteral("https://relay.example.com:8443");
    pairing.registrationUrl = QStringLiteral("https://relay.example.com:8443/api/notifications/native/register");
    pairing.pairingToken = QStringLiteral("tok-seed");
    pairing.deviceId = QStringLiteral("dev-seed");
    pairing.deviceName = QStringLiteral("Seeded Device");
    pairing.deviceSecret = QStringLiteral("secret-seed");
    QVERIFY(pairingStore.save(pairing));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    // Construction alone must reflect the pre-seeded pairing -- see
    // PairingController's constructor comment; no explicit refreshFromStore()
    // call needed here.
    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QVERIFY(controller.isPaired());
    QCOMPARE(controller.deviceId(), QStringLiteral("dev-seed"));
    QCOMPARE(controller.pairedServerHost(), QStringLiteral("relay.example.com"));
    // Task 39: this seed only touches PairingStore, never
    // DeviceRegistrationService::pair() -- SettingsStore's delivery fields
    // stay at their "never registered" empty default regardless of
    // isPaired, matching Settings.qml's Notifications pane "Not yet
    // registered" fallback.
    QVERIFY(controller.deliveryMode().isEmpty());
    QVERIFY(controller.transport().isEmpty());

    controller.removePairing();

    QVERIFY(!controller.isPaired());
    QVERIFY(controller.deviceId().isEmpty());
    QVERIFY(controller.pairedServerHost().isEmpty());
    QVERIFY(!pairingStore.load().has_value());
}

void PairingControllerTest::removePairingSkipsNetworkCallWhenNoDeviceSecretStored()
{
    // A pairing created before the per-device-secret migration has no
    // deviceSecret at all -- removePairing() must fall back to exactly the
    // old local-only clear, never attempting a request (whose credentials
    // would be blank/meaningless).
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-nosecret");
    pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    pairing.registrationUrl =
        QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port());
    pairing.pairingToken = QStringLiteral("tok-nosecret");
    pairing.deviceId = QStringLiteral("dev-nosecret");
    pairing.deviceName = QStringLiteral("Pre-Migration Device");
    // deviceSecret deliberately left empty.
    QVERIFY(pairingStore.save(pairing));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);
    QVERIFY(controller.isPaired());

    controller.removePairing();

    QVERIFY(!controller.isPaired());
    QVERIFY(!pairingStore.load().has_value());
    QVERIFY(fake.receivedRequest().isEmpty());
}

void PairingControllerTest::removePairingDeregistersServerSideWhenDeviceSecretPresent()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true})"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    PairingStore pairingStore(secureStore);

    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-full");
    pairing.serverBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());
    pairing.registrationUrl = QStringLiteral("http://127.0.0.1:%1/api/notifications/native/register").arg(fake.port());
    pairing.pairingToken = QStringLiteral("tok-full");
    pairing.deviceId = QStringLiteral("dev-full");
    pairing.deviceName = QStringLiteral("Fully Paired Device");
    pairing.deviceSecret = QStringLiteral("secret-full");
    QVERIFY(pairingStore.save(pairing));

    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    NativeRegistrationClient client(http);
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);
    QVERIFY(controller.isPaired());

    controller.removePairing();

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("POST /api/notifications/native/deregister HTTP/1.1"));
    QVERIFY(request.contains("X-Kypost-Device-Id: dev-full"));
    QVERIFY(request.contains("X-Kypost-Device-Secret: secret-full"));

    // Local state clears regardless of the (here, successful) network
    // outcome -- see removePairing()'s doc comment.
    QVERIFY(!controller.isPaired());
    QVERIFY(!pairingStore.load().has_value());
}

void PairingControllerTest::resetReturnsToIdleAfterFailure()
{
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
    DeviceRegistrationService service(client, pairingStore, settingsStore, http);
    DeregisterClient deregisterClient(http);

    PairingController controller(service, pairingStore, settingsStore, deregisterClient);

    QVERIFY(!controller.pairFromPastedLink(QStringLiteral("not a link")));
    QCOMPARE(controller.pairingState(), QStringLiteral("failed"));

    controller.reset();

    QCOMPARE(controller.pairingState(), QStringLiteral("idle"));
    QVERIFY(controller.pairingError().isEmpty());
}

// Shared graph for the app-lock tests below. Every other test in this file
// builds this by hand on the stack; a macro keeps that shape (stack-local, no
// hidden ownership) without a fifth verbatim copy.
#define PAIRING_TEST_FIXTURE()                                                              \
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"deviceId":"dev-1"})"));      \
    QTemporaryDir secureDir;                                                                 \
    QVERIFY(secureDir.isValid());                                                            \
    SecureStoreFile secureStore(secureDir.path());                                           \
    PairingStore pairingStore(secureStore);                                                  \
    QTemporaryDir settingsDir;                                                               \
    QVERIFY(settingsDir.isValid());                                                          \
    SettingsStore settingsStore(settingsDir.filePath(QStringLiteral("settings.ini")));       \
    QNetworkAccessManager manager;                                                           \
    HttpClient http(manager);                                                                \
    NativeRegistrationClient regClient(http);                                                \
    DeviceRegistrationService service(regClient, pairingStore, settingsStore, http);          \
    DeregisterClient deregisterClient(http);                                                 \
    PairingController controller(service, pairingStore, settingsStore, deregisterClient);     \
    QMap<QString, QString> params;                                                           \
    params[QStringLiteral("sub")] = QStringLiteral("sub-lock");                               \
    params[QStringLiteral("srv")] = QStringLiteral("http://127.0.0.1:%1").arg(fake.port());   \
    params[QStringLiteral("pt")] = QStringLiteral("pair-tok-lock");

// A kypost://native-pair link can arrive from anywhere on the session bus while
// the app is locked. The confirm prompt is a QQC2 Popup, which Qt renders inside
// QQuickOverlay -- above any z-ordered sibling, including the app-lock overlay
// (z: 1000). Rather than depend on that stacking question, the controller
// refuses to enter the confirm state at all while locked, so the answer does not
// matter. Matches Android, where PushPairingActivity is a LockedActivity and
// finishes on start, discarding the intent.
void PairingControllerTest::pairFromDeepLinkRefusedWhileAppLocked()
{
    PAIRING_TEST_FIXTURE()

    controller.setAppLocked(true);
    QVERIFY(!controller.pairFromDeepLink(buildLink(params)));

    QCOMPARE(controller.pairingState(), QStringLiteral("failed"));
    // Nothing is left pending: an attacker-supplied payload must not survive
    // across the lock waiting for a later confirm.
    QVERIFY(controller.pendingPairOrigin().isEmpty());
    QVERIFY(!controller.isPaired());
    QVERIFY(fake.receivedRequest().isEmpty());
}

// The case the z-order bug actually exposes: the dialog is already open and
// visible when the lock engages. Pressing Pair must do nothing.
void PairingControllerTest::confirmPendingPairRefusedWhenAppLocksAfterConfirmStateEntered()
{
    PAIRING_TEST_FIXTURE()

    QVERIFY(controller.pairFromDeepLink(buildLink(params)));
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));

    controller.setAppLocked(true);
    QVERIFY(!controller.confirmPendingPair());

    QVERIFY(!controller.isPaired());
    QVERIFY(fake.receivedRequest().isEmpty());
    QVERIFY(!pairingStore.load().has_value());
}

// Refusing while locked must not be sticky -- the user re-opens the link after
// unlocking, exactly as on Android.
void PairingControllerTest::pairFromDeepLinkWorksAgainAfterUnlock()
{
    PAIRING_TEST_FIXTURE()

    controller.setAppLocked(true);
    QVERIFY(!controller.pairFromDeepLink(buildLink(params)));

    controller.setAppLocked(false);
    QVERIFY(controller.pairFromDeepLink(buildLink(params)));
    QCOMPARE(controller.pairingState(), QStringLiteral("confirm"));
    QCOMPARE(controller.pendingPairOrigin(), QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
}

// pendingPairHost() returned QUrl::host() only, so "http://evil.example:8443"
// and "https://evil.example" were indistinguishable in the one confirmation the
// user ever gets. The scheme is the difference between sending the pairing token
// and the real push device token over TLS or in cleartext.
void PairingControllerTest::pendingPairOriginDisclosesSchemeAndPort()
{
    PAIRING_TEST_FIXTURE()

    QVERIFY(controller.pairFromDeepLink(buildLink(params)));

    const QString origin = controller.pendingPairOrigin();
    QVERIFY2(origin.startsWith(QStringLiteral("http://")), qPrintable(origin));
    QVERIFY2(origin.contains(QString::number(fake.port())), qPrintable(origin));
    // Loopback http is the one legitimate cleartext case, and it must still be
    // announced as insecure rather than blending in with https.
    QVERIFY(controller.pendingPairInsecure());
}

QTEST_GUILESS_MAIN(PairingControllerTest)
#include "PairingControllerTest.moc"
