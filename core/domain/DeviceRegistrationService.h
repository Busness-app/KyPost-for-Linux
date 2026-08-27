#pragma once

#include "net/HttpClient.h"
#include "net/NativeRegistrationClient.h"
#include "security/CredentialCipher.h"

#include <QByteArray>
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
    // Raw SHA-256 of the server's SubjectPublicKeyInfo, from the pairing
    // link's `pin` parameter. Empty means the link published none, which is
    // trust on first use -- what every pairing did before pins existed.
    //
    // When it is set, beginPair() arms it BEFORE the registration request
    // instead of clearing the pin, so the one call that discloses the pairing
    // token and the push credentials is already pinned. Capturing the key
    // afterwards (which is all TOFU can do) is too late: on a network with a
    // locally trusted CA the secrets have gone to the interceptor by then.
    //
    // Only the deep-link path fills this. Re-registration leaves it empty on
    // purpose: that is a deliberate tradeoff with a written reason, not an
    // omission -- see reregistrationParams().
    QByteArray spkiPin;
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

    // Phase 0 of the ASYNCHRONOUS re-registration, on the calling thread.
    //
    // reregisterIfPaired() above is reregistrationParams() + the synchronous
    // pair(), and it is what the tests drive. Callers that must not block --
    // which since 2026-08-24 is every caller in the running application, since
    // a distributor endpoint can rotate at any moment with the UI up -- take
    // these params, then beginPair()/sendRegistration()/finishPair() exactly
    // as PairingController does. Reading PairingStore is the part that cannot
    // leave this thread, which is why it is a separate call rather than
    // something the executor could do for itself.
    //
    // std::nullopt means there is no stored pairing: an ordinary state on a
    // device that has never paired, not a failure.
    std::optional<PairingParams> reregistrationParams() const;

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
    // the sealing key, and either arms `params.spkiPin` or suspends the
    // certificate pin for the registration.
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

    PairAttempt beginPair(const PairingParams& params);

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
