#pragma once

#include "stores/SecureStore.h"

#include <QString>

#include <functional>
#include <memory>

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
//     (25000 ms).
//   * When that call finally gives up, QKeychain::Job::finished is NEVER
//     EMITTED. The job simply never completes.
//
// That last point is the bug. The old code's exec() had exactly one exit --
// `finished` -- so once the D-Bus call gave up, the loop had nothing left to
// quit it and the application hung forever, with no window and nothing in the
// journal. The 300 s that SecureStoreKeychainTest appeared to take was
// QtTest's own watchdog killing it, not any timeout here.
//
// THE JOBS RUN ON THEIR OWN THREAD, added 2026-08-24, and that is what makes
// the timeout above mean something.
//
// Until then the job ran on the CALLING thread, which for every startup caller
// is the GUI thread -- so the ~25 s was spent inside a synchronous D-Bus call
// with that thread not processing events. No QEventLoop timer could be
// delivered until the call returned, so `timeoutMs` bounded the wait without
// ever shortening it, and the application was not merely slow to start: it was
// indistinguishable from dead, with no window, no repaint and nothing in the
// journal. The header said so and called it "a floor no amount of work in this
// file can get under", which was true only as long as the work stayed on that
// thread.
//
// It does not any more. A private worker thread owns the QKeychain jobs; the
// caller waits on a QEventLoop that is free to process its own events, so the
// timeout timer actually fires AND -- the part that matters to a user -- the
// GUI thread keeps painting. main() puts a window up before the first call for
// exactly that reason.
//
// The wait is still synchronous, deliberately. SecureStore's contract is
// synchronous and its callers (PairingStore, AppLockStore, DatabaseKeyStore)
// are read as plain accessors in dozens of places; making them asynchronous is
// a change to all of those, not to this file. What was worth fixing here is
// that the wait was UNINTERRUPTIBLE, not that it exists.
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
    // Per key: one key's legacy failure never skips the next key's attempt.
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

    // Builds one QKeychain job. Called ON THE WORKER THREAD, which is why the
    // job is created by a callback rather than passed in: a QObject belongs to
    // the thread that constructed it, and a job constructed here and started
    // over there would run its D-Bus work against the wrong thread's
    // connection.
    using JobFactory = std::function<QKeychain::Job*()>;

    // Runs the job `makeJob` builds to completion or to m_timeoutMs, whichever
    // comes first. Returns on the calling thread either way.
    //
    // The job is heap-allocated and ABANDONED on timeout rather than deleted,
    // and that is load-bearing. A job that has timed out is still live inside
    // a blocked D-Bus call; destroying it would free state that call still
    // uses. Abandoning it costs one leaked job in a session that is already
    // broken, and QtKeychain's autoDelete reaps it if the backend ever
    // answers.
    JobOutcome runBlocking(const JobFactory& makeJob) const;


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

    // read()'s guard ONLY -- remove() attempts the legacy service every time,
    // because skipping a removal reports a wipe that did not happen.
    //
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
