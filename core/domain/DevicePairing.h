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

// Which account+registration a request was authorised by, and nothing else.
//
// A reply is written into a local cache that has no subscriber column: the
// emails, contacts, groups and push tables are per-DEVICE, so a reply that
// arrives after the device has been re-paired to a different account lands in
// that account's mailbox and is readable by whoever is now using it. Pairing
// a replacement account purges the caches (LocalDataWipe::wipeCachedAccountData),
// but a request already in flight completes AFTER the purge and writes its
// rows back in behind it.
//
// So every apply step compares the identity that authorised its request --
// captured before the request, carried across the thread hop -- against the
// identity the store holds now, and writes nothing if they differ.
//
// deviceId is part of it, not just subscriberId: re-registering the SAME
// account mints a fresh device registration, and a reply authorised by the
// previous one has no claim on the new one's cache either. deviceSecret is
// deliberately NOT part of it -- the credential gate re-saves the pairing on
// every lock/unlock, and keying on the secret would discard every legitimate
// reply that spanned one.
struct PairingIdentity
{
    QString subscriberId;
    QString deviceId;

    // An unpaired store's identity. Never equal to a real pairing's, so a
    // reply that outlives an unpair is discarded on the same comparison.
    bool isEmpty() const { return subscriberId.isEmpty() && deviceId.isEmpty(); }

    bool operator==(const PairingIdentity&) const = default;
};

inline PairingIdentity identityOf(const DevicePairing& pairing)
{
    return { pairing.subscriberId, pairing.deviceId };
}
