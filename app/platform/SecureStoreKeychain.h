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
// finishes OR the timeout below expires, keeping the synchronous SecureStore
// contract.
//
// THE TIMEOUT IS THE POINT, not a refinement. This class used to wait on
// QKeychain::Job::finished and nothing else, and main() drives it at startup
// before any window exists.
//
// Measured against a live gnome-keyring with a locked collection, rather than
// reasoned about (`start()` and `QEventLoop::exec()` timed separately, read
// and write jobs both):
//
//   * job->start() returns in 0 ms. Nothing blocks there.
//   * The first exec() blocks for ~25.0 s -- Qt's DEFAULT D-BUS CALL TIMEOUT
//     (25000 ms). The thread is inside a synchronous D-Bus call and is not
//     processing events, so no QEventLoop timer can interrupt it. This is a
//     floor no amount of work in this file can get under.
//   * When that call finally gives up, QKeychain::Job::finished is NEVER
//     EMITTED. The job simply never completes.
//
// That last point is the bug. The old code's exec() had exactly one exit --
// `finished` -- so once the D-Bus call gave up, the loop had nothing left to
// quit it and the application hung forever, with no window and nothing in the
// journal. The 300 s that SecureStoreKeychainTest appeared to take was
// QtTest's own watchdog killing it, not any timeout here.
//
// So the timer below is what guarantees termination. It does NOT make this
// fast: in the failure mode above the ~25 s D-Bus floor dominates, and a
// timeout shorter than that changes nothing. Callers that run before the UI
// exists should therefore avoid making more of these calls than they need --
// see main()'s canary, which stops at the first failure for this reason.
class SecureStoreKeychain : public SecureStore
{
public:
    // The per-job default, named so callers overriding only `legacyService`
    // below do not have to restate 30000 and go stale when it changes.
    static constexpr int kDefaultTimeoutMs = 30000;

    // `timeoutMs` bounds each individual job, measured from start(). Chosen
    // just above the ~25 s D-Bus floor described above, for two reasons:
    // anything below it is inert (the blocking call returns at ~25 s and the
    // timer is processed immediately afterwards either way), and when a
    // prompter IS running the thing being waited on is a human reading a
    // dialog and typing a passphrase -- cutting that short would report
    // "store unavailable" for a keyring that was about to open perfectly
    // well, which fails closed into a lock screen the owner cannot pass.
    //
    // Exposed as a parameter (rather than hardcoded) so tests can force the
    // timeout path instead of needing a wedged Secret Service, matching
    // HttpClient's transferTimeoutMs.
    // `legacyService` is the read-only fallback used by the app-id rename
    // (com.urlxl.mail -> com.kysecurity.mail, 2026-08-23). Secrets reached
    // through --talk-name=org.freedesktop.secrets are keyed by the service
    // string alone, not by the Flatpak app id, so the old entries survive the
    // rename and are recoverable -- but only if something looks for them.
    //
    // The fallback lives HERE rather than in a one-shot migration pass over a
    // list of key names, because such a list is a copy of what PairingStore,
    // AppLockStore and DatabaseKeyStore each define privately, and a key that
    // drifts out of the copy is a credential silently lost with nothing
    // failing. read() below is key-agnostic, so it cannot drift.
    //
    // Empty (the default) disables the fallback entirely: tests and any
    // future caller get exactly the old single-service behavior.
    explicit SecureStoreKeychain(const QString& service, int timeoutMs = kDefaultTimeoutMs,
                                 const QString& legacyService = QString());

    // The three-state read. QKeychain reports EntryNotFound as its own error
    // code, distinct from "no Secret Service provider is reachable" / "the
    // wallet is locked" / "D-Bus is not running", so this backend is the one
    // that can actually answer the question -- and is exactly the backend
    // where the answer matters, because it is what ships.
    //
    // A timeout maps to Failed, never Absent. It is the strongest available
    // evidence that the store could not be consulted, and reporting it as
    // "there is no such secret" is precisely the conflation SecureStore::
    // ReadStatus exists to prevent: AppLockStore would read a timed-out
    // keyring as "no PIN configured" and unlock the app.
    //
    // On Absent under the primary service, retries under `legacyService` and,
    // on a hit, copies the value forward so the next read is a single call.
    // Copies rather than moves, matching how the Llama Mail -> KyPost profile
    // migration was fixed (docs/RENAME_NOTES.md): a bad migration stays
    // recoverable by hand. remove() still clears BOTH services, so a wipe is
    // not quietly undone by a legacy copy the next read would resurrect.
    //
    // Absent under the primary service while the LEGACY read reports Failed
    // is reported as Failed, not Absent. The primary answering Absent proves
    // the Secret Service is reachable, so a Failed legacy read against the
    // same daemon is anomalous, and reporting it as absence is the exact
    // conflation ReadStatus exists to prevent -- AppLockStore would read a
    // legacy applock.enabled it could not consult as "no PIN configured".
    ReadResult read(const QString& key) const override;

    bool set(const QString& key, const QString& value) override;
    std::optional<QString> get(const QString& key) const override;

    // Removes from BOTH services, and reports false if the legacy service
    // still holds a copy. A half-removal that reported success would let
    // read()'s copy-forward resurrect a wiped credential on the next launch.
    bool remove(const QString& key) override;
    bool contains(const QString& key) const override;

    int timeoutMs() const { return m_timeoutMs; }

private:
    // What a job reported, captured while it was still alive.
    struct JobOutcome
    {
        bool completed = false; // false == timed out; the fields below are then meaningless
        int error = 0;          // QKeychain::Error, as int to keep it out of this header
        QString textData;
    };

    // Runs `job` to completion or to m_timeoutMs, whichever comes first.
    //
    // TAKES OWNERSHIP of a heap-allocated job, and that is load-bearing. A
    // job that has timed out is still live inside a blocked D-Bus call;
    // destroying it -- which is what the stack-allocated jobs this replaced
    // did on the way out of each method -- would free state that call still
    // uses. Abandoning it costs one leaked job in a session that is already
    // broken, and QtKeychain's autoDelete reaps it if the backend ever
    // answers.
    JobOutcome runBlocking(QKeychain::Job* job) const;

    // The service-parameterised primitives read()/remove() drive for both the
    // primary and the legacy service.
    ReadResult readFrom(const QString& service, const QString& key) const;
    // const so read()'s copy-forward can use it without a const_cast; it
    // mutates the Secret Service, not this object.
    bool writeTo(const QString& service, const QString& key, const QString& value) const;
    bool removeFrom(const QString& service, const QString& key);

    QString m_service;
    int m_timeoutMs;
    QString m_legacyService;

    // Latches false the first time the legacy service reports Failed, for the
    // lifetime of this store. Without it, an unreachable legacy service costs
    // another ~25 s D-Bus block (see the measurements above) on EVERY missing
    // key -- 19 of them across the three stores at startup, before any window
    // exists. One failure is enough evidence; retrying it 18 more times buys
    // nothing but an eight-minute launch.
    //
    // Deliberately not reset anywhere: a session that could not reach the
    // legacy store keeps its own store working, and the migration retries on
    // the next launch.
    mutable bool m_legacyReachable;
};
