#include "update/UpdateCheckController.h"

#include "domain/PairingStore.h"
#include "net/HttpClient.h"
#include "net/NetworkExecutor.h"
#include "net/RelayAuth.h"
#include "version/VersionCompare.h"

namespace {
// Matches the server's own hourly release-check tick. Nothing is gained by
// asking more often: the server serves a cache that only moves hourly.
constexpr int kPollIntervalMs = 60 * 60 * 1000;

// The page a user is sent to. Not an API endpoint -- this is for a human.
constexpr auto kReleasesPage = "https://github.com/Busness-app/KyPost-for-Linux/releases";
} // namespace

QString UpdateCheckController::compiledInVersion()
{
    return QStringLiteral(KYPOST_VERSION);
}

QString UpdateCheckController::releaseUrl() const
{
    return QString::fromLatin1(kReleasesPage);
}

UpdateCheckController::UpdateCheckController(PairingStore& pairingStore, NetworkExecutor& executor,
                                             QObject* parent)
    : QObject(parent)
    , m_pairingStore(pairingStore)
    , m_executor(executor)
{
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &UpdateCheckController::checkNow);
    m_pollTimer.start();
}

void UpdateCheckController::checkNow()
{
    // load(), not loadChecked(). The three-state form exists for callers
    // making a security decision from "is this device paired", where an
    // unreadable keyring must not read as "not paired" (PairingStore.h:39-41).
    // This is not one of those: an unreadable store and an unpaired device
    // both mean "nobody to ask about updates right now", and the next hourly
    // tick retries either way.
    //
    // If this device is not paired there is nobody to ask, and there is no
    // fallback to GitHub by design.
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return;

    const QUrl serverBaseUrl(pairing->serverBaseUrl);
    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    m_executor.run(
        this,
        [serverBaseUrl, auth](HttpClient& http) {
            return ClientVersionClient(http).fetch(serverBaseUrl, auth);
        },
        [this](ClientVersionResult result) { applyResult(result); });
}

void UpdateCheckController::applyResult(const ClientVersionResult& result)
{
    // Failures and unsupported servers are dropped, not surfaced. This runs
    // unattended on a timer, an unreachable self-hosted server is routine,
    // and the next tick retries -- the same reasoning the server gives for
    // its own check. A server too old to have the endpoint is not a fault at
    // all. In both cases the last known-good values stay on screen.
    if (result.error.has_value() || !result.supported)
        return;

    const bool wasAvailable = m_updateAvailable;
    // A tag the server forwards but this client cannot parse (see
    // VersionCompare.h) must not be displayed at all: showing it would let
    // the "not newer" branch below read as "you are current" rather than
    // "unknown", which is exactly the conflation the About section forbids.
    m_latestVersion = VersionCompare::isValid(result.latestVersion) ? result.latestVersion : QString();
    m_checkedAt = result.checkedAt;
    m_updateAvailable = VersionCompare::isNewer(m_latestVersion, compiledInVersion());
    emit changed();
    if (m_updateAvailable && !wasAvailable)
        emit updateBecameAvailable();
}

void UpdateCheckController::pairingMayHaveChanged()
{
    checkNow();
}
