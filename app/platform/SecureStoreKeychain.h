#pragma once

#include "stores/SecureStore.h"

#include <QString>

namespace QKeychain {
class Job;
}

// SecureStore backed by the platform Secret Service via QtKeychain
// (org.freedesktop.secrets over D-Bus). Lives in app/, not core/, because it
// talks to QtDBus-adjacent infrastructure that core/ must never link. Each
// call blocks on a local QEventLoop until the underlying QtKeychain job
// finishes, keeping the synchronous SecureStore contract.
class SecureStoreKeychain : public SecureStore
{
public:
    explicit SecureStoreKeychain(const QString& service);

    // The three-state read. QKeychain reports EntryNotFound as its own error
    // code, distinct from "no Secret Service provider is reachable" / "the
    // wallet is locked" / "D-Bus is not running", so this backend is the one
    // that can actually answer the question -- and is exactly the backend
    // where the answer matters, because it is what ships.
    ReadResult read(const QString& key) const override;

    bool set(const QString& key, const QString& value) override;
    std::optional<QString> get(const QString& key) const override;
    bool remove(const QString& key) override;
    bool contains(const QString& key) const override;

private:
    // Runs `job` synchronously to completion via a local QEventLoop tied to
    // QKeychain::Job::finished. Returns true iff job.error() == NoError;
    // callers needing textData() or a different error-code fallback (e.g.
    // remove()'s EntryNotFound-is-ok case) still inspect `job` themselves
    // afterward.
    static bool runBlocking(QKeychain::Job& job);

    QString m_service;
};
