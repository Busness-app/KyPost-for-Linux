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

    // Why a three-state read exists alongside get().
    //
    // get() returns std::optional, which collapses "the store says there is
    // no such key" and "the store could not be consulted" into the same
    // std::nullopt. For most keys that is harmless. For the app lock it was
    // a silent bypass: AppLockStore::lockEnabled() reads its flag through
    // get(), so an unreachable keyring -- stop gnome-keyring, or move
    // ~/.local/share/keyrings aside -- read as "no PIN configured".
    // AppLockManager's constructor then started the process UNLOCKED, with
    // no PIN screen, no lock overlay and no credential gate, for a user who
    // had been shown a PIN dialog the day before. main.cpp knew: it printed
    // a qCritical and carried on.
    //
    // Anything making a security decision must call read() and treat Failed
    // as "assume the protective answer", never as Absent. get() stays for
    // the many callers where the distinction genuinely does not matter.
    enum class ReadStatus
    {
        Found,  // the key exists; `value` is it
        Absent, // the store answered, and there is no such key
        Failed, // the store could not be consulted at all
    };

    struct ReadResult
    {
        ReadStatus status = ReadStatus::Failed;
        QString value;

        bool found() const { return status == ReadStatus::Found; }
        bool failed() const { return status == ReadStatus::Failed; }
    };

    // Virtual with a default rather than pure: an in-memory or on-disk store
    // that cannot distinguish "absent" from "unreachable" has nothing more
    // truthful to say than this, and every such implementation would
    // otherwise be forced to write the same three lines. The backends that
    // CAN tell the difference -- SecureStoreKeychain, whose QKeychain job
    // reports EntryNotFound separately from every other error -- override it.
    virtual ReadResult read(const QString& key) const
    {
        const std::optional<QString> value = get(key);
        if (!value.has_value())
            return ReadResult{ ReadStatus::Absent, QString() };
        return ReadResult{ ReadStatus::Found, *value };
    }

    virtual bool set(const QString& key, const QString& value) = 0;
    virtual std::optional<QString> get(const QString& key) const = 0;
    virtual bool remove(const QString& key) = 0;
    virtual bool contains(const QString& key) const = 0;
};
