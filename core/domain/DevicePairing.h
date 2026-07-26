#pragma once

#include <QString>

// The full set of pairing state PairingStore persists to SecureStore.
// subscriberId/deviceId reuse SecureStore.h's own doc-comment key names
// ("sub"/"deviceId"); the remaining five are prefixed "pairing." to avoid
// colliding with any future unrelated key.
struct DevicePairing
{
    QString subscriberId;    // SecureStore key "sub"
    QString serverBaseUrl;   // SecureStore key "pairing.serverBaseUrl"
    QString registrationUrl; // SecureStore key "pairing.registrationUrl"
    QString pairingToken;    // SecureStore key "pairing.pairingToken"
    QString deviceId;        // SecureStore key "deviceId"
    QString deviceName;      // SecureStore key "pairing.deviceName"
    // The per-device pairing secret, minted fresh on every successful
    // registration and returned only in that response -- never carried in
    // the pairing deep link/QR. Must be persisted unconditionally,
    // overwriting any prior value, since every successful register
    // invalidates the previous secret. May be empty for a pairing created
    // before this field existed (pre-migration), which is not an error --
    // see PairingController::removePairing()'s graceful-degradation path.
    QString deviceSecret; // SecureStore key "pairing.deviceSecret"

    // Base64 SHA-256 of the server's TLS SubjectPublicKeyInfo, captured on
    // the handshake that completed this device's registration (trust on
    // first use). Empty means "no pin recorded" -- for a pairing created
    // before this field existed, or one made over plain http in testing --
    // and enforcement is skipped rather than failing every request.
    QString certificateSpkiSha256; // SecureStore key "pairing.certificateSpkiSha256"

    bool operator==(const DevicePairing&) const = default;
};
