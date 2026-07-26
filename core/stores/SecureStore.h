#pragma once

#include <QString>
#include <optional>

// Backend-agnostic interface for storing secrets. Implementations persist
// each key/value pair however suits the platform (flat files, OS keychain,
// ...). Keys this store is expected to hold, so a future phase's
// DeviceRegistrationService knows the contract without re-deriving it: `sub`
// (subscriberId), `pairing.deviceSecret` (the per-device pairing secret),
// `deviceId`, and pairing credentials. (An `ntfy-topic` bearer secret was
// also in this set until 2026-07-26, when the embedded ntfy subscriber tier
// that owned it was removed -- see core/domain/TransportStateMachine.h. Any
// file a previous version left behind is simply never read again.)
class SecureStore
{
public:
    virtual ~SecureStore() = default;

    virtual bool set(const QString& key, const QString& value) = 0;
    virtual std::optional<QString> get(const QString& key) const = 0;
    virtual bool remove(const QString& key) = 0;
    virtual bool contains(const QString& key) const = 0;
};
