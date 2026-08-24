#include "pgp/PgpEnrollmentController.h"

#include "domain/PairingStore.h"
#include "net/HttpClient.h"
#include "net/NetworkExecutor.h"
#include "net/PgpBootstrapClient.h"
#include "net/PgpDeviceEnrollmentClient.h"
#include "pgp/OpenPgpKeyImporter.h"

#include <KLocalizedString>

#include <QDateTime>

struct PgpEnrollmentController::StartResult
{
    enum Failure { None, Network, NotClientProtected, PublishFailed } failure = None;
    QString fingerprint;
};

namespace {
struct ImportResult
{
    PgpImportResult imported;
    bool enrollmentStateReported = false;
};
}

PgpEnrollmentController::PgpEnrollmentController(PairingStore& pairingStore, NetworkExecutor& executor,
                                                 QObject* parent)
    : QObject(parent), m_pairingStore(pairingStore), m_executor(executor)
{
    m_pollTimer.setInterval(3000);
    m_pollTimer.setSingleShot(true);
    connect(&m_pollTimer, &QTimer::timeout, this, &PgpEnrollmentController::poll);
    m_codeTimer.setInterval(1000);
    connect(&m_codeTimer, &QTimer::timeout, this, &PgpEnrollmentController::refreshCode);
}

void PgpEnrollmentController::setState(State state, const QString& status)
{
    m_state = state;
    m_status = status;
    emit changed();
}

void PgpEnrollmentController::start()
{
    if (busy())
        return;
    cancel();
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value() || pairing->deviceSecret.isEmpty()) {
        finishWithFailure(i18n("Pair and unlock KyPost before enrolling this device."));
        return;
    }
    if (!m_crypto.generate()) {
        finishWithFailure(i18n("Could not create this device's enrollment key."));
        return;
    }
    m_serverBaseUrl = QUrl(pairing->serverBaseUrl);
    m_auth = RelayAuth{pairing->deviceId, pairing->deviceSecret};
    m_identity = identityOf(*pairing);
    const QByteArray publicKey = m_crypto.publicKeyBase64();
    setState(Starting, i18n("Publishing this device's enrollment key…"));
    m_executor.run(
        this,
        [url = m_serverBaseUrl, auth = m_auth, publicKey](HttpClient& http) {
            StartResult out;
            const PgpBootstrapResult bootstrap = PgpBootstrapClient(http).fetch(url, auth);
            if (!bootstrap.ok) { out.failure = StartResult::Network; return out; }
            if (bootstrap.protection != QStringLiteral("client") || bootstrap.fingerprint.isEmpty()) {
                out.failure = StartResult::NotClientProtected;
                return out;
            }
            out.fingerprint = bootstrap.fingerprint;
            const DeviceEnrollmentCallResult published =
                PgpDeviceEnrollmentClient(http).publishKey(url, auth, publicKey);
            if (published.status != DeviceEnrollmentCallStatus::Ok)
                out.failure = StartResult::PublishFailed;
            return out;
        },
        [this](const StartResult& result) {
            if (!m_pairingStore.stillCurrent(m_identity)) {
                cancel();
                return;
            }
            if (result.failure != StartResult::None) {
                const QString message = result.failure == StartResult::NotClientProtected
                    ? i18n("This account does not use a client-protected OpenPGP key.")
                    : i18n("The enrollment key could not be published.");
                finishWithFailure(message);
                return;
            }
            m_fingerprint = result.fingerprint;
            m_pollWindow.start();
            refreshCode();
            m_codeTimer.start();
            setState(Waiting, i18n("In webmail, open Security → Devices, choose this device, and enter the code shown here."));
            poll();
        });
}

void PgpEnrollmentController::refreshCode()
{
    if (!m_crypto.isReady())
        return;
    const QString code = formatEnrollmentCode(m_crypto.verificationCode(
        m_auth.deviceId, QDateTime::currentSecsSinceEpoch() / 120));
    if (code != m_code) {
        m_code = code;
        emit changed();
    }
}

void PgpEnrollmentController::poll()
{
    if (m_state != Waiting)
        return;
    if (m_pollWindow.elapsed() >= kPollWindowMs) {
        m_pollTimer.stop();
        m_codeTimer.stop();
        setState(TimedOut, i18n("No envelope arrived yet. Finish the webmail step, then check again."));
        return;
    }
    m_executor.run(
        this,
        [url = m_serverBaseUrl, auth = m_auth](HttpClient& http) {
            return PgpDeviceEnrollmentClient(http).fetchEnvelope(url, auth);
        },
        [this](const DeviceEnrollmentCallResult& result) {
            if (m_state != Waiting || !m_pairingStore.stillCurrent(m_identity)) {
                cancel();
                return;
            }
            if (result.status == DeviceEnrollmentCallStatus::NotFound
                || result.status == DeviceEnrollmentCallStatus::RateLimited
                || result.status == DeviceEnrollmentCallStatus::Failed) {
                m_pollTimer.start();
                return;
            }
            if (result.status != DeviceEnrollmentCallStatus::Ok) {
                finishWithFailure(i18n("This device is no longer authorised to fetch its envelope."));
                return;
            }
            SecureBytes privateKey = m_crypto.openEnvelope(result.envelope, m_auth.deviceId, m_fingerprint);
            m_crypto.clear();
            m_pollTimer.stop();
            m_codeTimer.stop();
            m_code.clear();
            if (privateKey.isEmpty()) {
                finishWithFailure(i18n("The envelope was not sealed for this device or this OpenPGP identity."));
                return;
            }
            setState(Importing, i18n("Importing the private key into GnuPG…"));
            m_executor.run(
                this,
                [key = std::move(privateKey), fingerprint = m_fingerprint,
                 url = m_serverBaseUrl, auth = m_auth](HttpClient& http) mutable {
                    ImportResult importResult;
                    importResult.imported = importPrivateKey(key, fingerprint);
                    key.clear();
                    if (importResult.imported.status == PgpImportStatus::Imported
                        || importResult.imported.status == PgpImportStatus::Unchanged) {
                        importResult.enrollmentStateReported =
                            PgpDeviceEnrollmentClient(http).reportState(url, auth, true).status
                            == DeviceEnrollmentCallStatus::Ok;
                    }
                    return importResult;
                },
                [this](const ImportResult& importResult) {
                    if (!m_pairingStore.stillCurrent(m_identity)) {
                        cancel();
                        return;
                    }
                    if (importResult.imported.status == PgpImportStatus::Imported
                        || importResult.imported.status == PgpImportStatus::Unchanged) {
                        setState(Enrolled, importResult.enrollmentStateReported
                            ? i18n("This device can now decrypt and sign with your OpenPGP key.")
                            : i18n("The key was imported, but KyPost could not update the device status on the server."));
                        emit enrolled();
                    } else {
                        finishWithFailure(importResult.imported.status == PgpImportStatus::EngineUnavailable
                            ? i18n("GnuPG is not available on this computer.")
                            : i18n("GnuPG rejected the private key or its fingerprint did not match."));
                    }
                });
        });
}

void PgpEnrollmentController::checkAgain()
{
    if (m_state != TimedOut || !m_crypto.isReady())
        return;
    m_pollWindow.restart();
    refreshCode();
    m_codeTimer.start();
    setState(Waiting, i18n("Checking for the envelope sealed by webmail…"));
    poll();
}

void PgpEnrollmentController::finishWithFailure(const QString& status)
{
    m_pollTimer.stop();
    m_codeTimer.stop();
    m_crypto.clear();
    m_code.clear();
    setState(Failed, status);
}

void PgpEnrollmentController::cancel()
{
    m_pollTimer.stop();
    m_codeTimer.stop();
    m_crypto.clear();
    m_code.clear();
    m_fingerprint.clear();
    m_identity = {};
    m_state = Idle;
    m_status.clear();
    emit changed();
}

void PgpEnrollmentController::pairingMayHaveChanged()
{
    if (m_state != Idle && !m_pairingStore.stillCurrent(m_identity))
        cancel();
}
