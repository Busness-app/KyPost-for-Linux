#pragma once

#include "net/HttpClient.h"
#include "net/NativeRegistrationClient.h"
#include "security/CredentialCipher.h"

#include <QString>
#include <optional>

class PairingStore;
class SettingsStore;
class CertificatePinSink;

// Initial-pairing and re-registration params for DeviceRegistrationService::
// pair(). Mirrors DevicePairing's six persisted fields minus deviceId, which
// the server assigns on first pairing and DeviceRegistrationService supplies
// itself on re-registration (from the currently-stored DevicePairing).
struct PairingParams
{
    QString subscriberId;
    QString serverBaseUrl;
    QString registrationUrl;
    QString pairingToken;
    QString deviceName;
};

// Wraps NativeRegistrationClient with the "persist on success, leave
// untouched otherwise" policy from Linux_QT_Client_Plan.md's Phase 4 scope:
// PairingStore and SettingsStore's delivery fields only ever change together,
// and only on RegistrationOutcome::Success.
class DeviceRegistrationService
{
public:
    // `httpClient` is the same instance `client` was built on. Taken
    // directly so pair() can read the SPKI hash of the handshake that just
    // completed the registration -- that is the trust-on-first-use moment,
    // and there is no later point at which the pin can be captured
    // legitimately (by then an attacker's certificate would be pinned).
    DeviceRegistrationService(NativeRegistrationClient& client, PairingStore& pairingStore,
                               SettingsStore& settingsStore, CertificatePinSink& pinSink);

    NativeRegistrationResult pair(const PairingParams& params, const QString& deviceToken);

    std::optional<NativeRegistrationResult> reregisterIfPaired(const QString& deviceToken);

    // ---- three-phase form, for callers that must not block ---------------
    //
    // pair() above touches four things: PairingStore, SettingsStore, the
    // certificate pin, and the network. Only the LAST of those may leave the
    // calling thread -- PairingStore caches and is mutated by the credential
    // gate, SettingsStore is a QSettings file, and the pin fan-out reaches an
    // HttpClient that asserts its own thread affinity. So this is not "run
    // pair() over there"; it is a split with the store work pinned to the
    // caller's thread and only the HTTP request moved.
    //
    // That split is what the docs/THREADING.md tier list under-described:
    // the constraint is not only QSqlDatabase, it is any thread-confined
    // store in the chain, and PairingStore is one.

    // Phase 1, on the calling thread. Runs the pre-flight guards, snapshots
    // the sealing key, and suspends the certificate pin for the registration.
    //
    // Move-only, and it RESTORES the pin on destruction. That matters more
    // here than in the synchronous form: an attempt abandoned because the
    // executor shut down, or because the controller gave up, must not leave
    // pinning disabled for the rest of the process -- which is the exact
    // defect the ScopedPinSuspension guard was introduced to fix.
    class PairAttempt
    {
    public:
        ~PairAttempt();
        PairAttempt(PairAttempt&&) noexcept;
        PairAttempt& operator=(PairAttempt&&) noexcept;
        PairAttempt(const PairAttempt&) = delete;
        PairAttempt& operator=(const PairAttempt&) = delete;

        // Set when the guards refused before any network contact -- today
        // only RegistrationOutcome::CredentialsLocked. finish() must not be
        // called; report this outcome instead.
        std::optional<RegistrationOutcome> refusedOutcome() const { return m_refusedOutcome; }

    private:
        friend class DeviceRegistrationService;
        PairAttempt() = default;

        CertificatePinSink* m_pinSink = nullptr;
        HttpClient::CertificatePinState m_savedPin;
        CredentialCipher::SessionKey m_sealingKey;
        std::optional<RegistrationOutcome> m_refusedOutcome;
        bool m_restorePin = false;
    };

    PairAttempt beginPair();

    // Phase 2. The only part that may run on another thread: it touches
    // nothing but the HttpClient it is handed. Static for that reason --
    // there is no `this` to accidentally reach through.
    static NativeRegistrationResult sendRegistration(HttpClient& httpClient, const PairingParams& params,
                                                      const QString& deviceToken);

    // Phase 3, back on the calling thread. Persists, installs the new pin,
    // writes the delivery settings, and consumes the attempt.
    NativeRegistrationResult finishPair(PairAttempt attempt, const PairingParams& params,
                                         const NativeRegistrationResult& result);

private:
    NativeRegistrationClient& m_client;
    PairingStore& m_pairingStore;
    SettingsStore& m_settingsStore;
    CertificatePinSink& m_pinSink;
};
