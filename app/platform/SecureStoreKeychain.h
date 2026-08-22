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
    explicit SecureStoreKeychain(const QString& service, int timeoutMs = 30000);

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
    ReadResult read(const QString& key) const override;

    bool set(const QString& key, const QString& value) override;
    std::optional<QString> get(const QString& key) const override;
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

    QString m_service;
    int m_timeoutMs;
};
