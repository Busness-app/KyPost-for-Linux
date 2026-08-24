#pragma once

#include <QAtomicInteger>
#include <QObject>
#include <QThread>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

class HttpClient;
class QNetworkAccessManager;

// Owns the thread that Relay HTTP actually happens on.
//
// WHY THIS EXISTS
//
// HttpClient::waitForReply drives a nested QEventLoop. On the GUI thread a
// nested event loop is worse than blocking: input keeps being delivered,
// timers keep firing and bindings keep re-evaluating, so any controller
// method that makes a network call can be re-entered from QML while it is
// suspended half-way through. Four separate fixes in this repo exist only to
// survive that -- ReentrancyGuard, PairingStore's lock epoch,
// DeviceRegistrationService's sealing-key snapshot, and main.cpp's
// QTimer::singleShot(0) to escape a half-finished unlock. Each is correct
// and none of them is the fix.
//
// Moving the loop to a thread with no QML on it removes the category. The
// GUI thread is then never inside a nested loop, so there is nothing to
// re-enter.
//
// WHAT THE CALLER GETS
//
// run() returns immediately. `work` executes on the executor thread and is
// handed the HttpClient; whatever it returns is delivered to `onDone` on the
// receiver's own thread, via the normal event queue. So a converted
// controller method looks like:
//
//     void SomeController::doThing(...)
//     {
//         setState("sending");
//         m_executor.run(this,
//             [=](HttpClient& http) { return doTheBlockingCall(http); },
//             [this](Result r) { applyResult(r); });
//     }
//
// -- no return value, no blocking, and the state machine the UI binds to is
// the only thing that moves.
//
// THREAD AFFINITY RULES, which are not optional
//
//  * The QNetworkAccessManager and HttpClient are CONSTRUCTED on the
//    executor thread and must only ever be touched there. That includes the
//    certificate pin: setCertificatePin() from the GUI thread would be a
//    plain data race against a request reading it. Use configure() below.
//  * `work` runs on the executor thread. It must not touch QSqlDatabase or
//    any DAO -- a QSqlDatabase connection may only be used from the thread
//    that opened it, and this repo opens it on the GUI thread. That
//    restriction is what splits the controllers into two migration tiers;
//    see docs/THREADING.md.
//  * `onDone` runs on the receiver's thread, so it may touch models, emit
//    signals and update QML-bound state freely.
//
// LIFETIME
//
// Receivers must outlive shutdown(); see run(). shutdown() waits for work that is already EXECUTING and discards work
// still queued behind it, so no callback can fire after it returns. The
// destructor calls it too, but main() should call it explicitly right after
// app.exec() returns: the executor has to be CONSTRUCTED before the
// controllers that take a reference to it, which means it would otherwise be
// DESTROYED after them.
class NetworkExecutor : public QObject
{
    Q_OBJECT

public:
    // transferTimeoutMs is forwarded to the HttpClient constructed on the
    // executor thread; exposed so tests need not wait out the real default.
    explicit NetworkExecutor(int transferTimeoutMs = 30000, QObject* parent = nullptr);
    ~NetworkExecutor() override;

    NetworkExecutor(const NetworkExecutor&) = delete;
    NetworkExecutor& operator=(const NetworkExecutor&) = delete;

    // Runs `work` on the executor thread; delivers its result to `onDone` on
    // `receiver`'s thread. Returns immediately.
    //
    // PRECONDITION, and it is the caller's to keep: `receiver` must still be
    // alive when shutdown() returns. In practice that means every receiver
    // is a controller owned by the composition root, and main() calls
    // shutdown() immediately after app.exec() returns, before anything is
    // destroyed. shutdown() waits for executing work and refuses queued
    // work, so once it has returned no handler can fire and the receiver may
    // be destroyed freely.
    //
    // This used to hold a QPointer and check it on the executor thread
    // before delivering, which LOOKED like it made a mid-flight destruction
    // safe and did not: QPointer is explicitly not thread-safe, so the check
    // races with ~QObject on the receiver's thread and can report "alive"
    // for an object already being torn down -- after which the delivery
    // dereferences freed memory. ThreadSanitizer flags it (QWeakPointer::
    // isNull). There is no cheap way to close that window from this side;
    // the check-and-post would have to happen under the same lock as the
    // destruction, which is precisely what Qt's own signal/slot connection
    // machinery does and what a raw pointer check cannot. Rather than keep a
    // guard that is wrong under exactly the conditions it claims to handle,
    // the requirement is stated and enforced by shutdown() ordering.
    template<typename Work, typename Handler>
    void run(QObject* receiver, Work work, Handler onDone)
    {
        postToExecutor(makeCopyable([this, receiver, work = std::move(work),
                                      onDone = std::move(onDone)]() mutable {
            if (!isAcceptingWork())
                return;
            auto result = work(httpClientOnExecutorThread());
            QMetaObject::invokeMethod(
                receiver,
                makeCopyable(
                    [onDone = std::move(onDone), result = std::move(result)]() mutable { onDone(std::move(result)); }),
                Qt::QueuedConnection);
        }));
    }

    // Runs `work` on the executor thread with nothing to deliver back.
    template<typename Work>
    void runDetached(Work work)
    {
        postToExecutor(makeCopyable([this, work = std::move(work)]() mutable {
            if (!isAcceptingWork())
                return;
            work(httpClientOnExecutorThread());
        }));
    }

    // Applies a configuration change to the HttpClient ON the executor
    // thread, and BLOCKS until it has been applied.
    //
    // Blocking is deliberate here and is not the thing this class exists to
    // avoid. These calls are pure in-memory field writes -- setting the
    // certificate pin, installing the mismatch handler -- so the wait is
    // microseconds, not a network round trip. The alternative, letting the
    // GUI thread write those fields directly, is a data race against a
    // request reading them mid-handshake.
    void configure(const std::function<void(HttpClient&)>& configuration);

    // Stops the thread and waits for in-flight work. Idempotent.
    void shutdown();

private:
    // Wraps a possibly MOVE-ONLY callable in a copyable one.
    //
    // Both hops here go through std::function -- ours, and Qt's own
    // QMetaObject::invokeMethod -- and std::function requires its target to
    // be copy-constructible. That is not an academic constraint: a handler
    // legitimately owns move-only state, and the first real caller to do so
    // was PairingController, whose completion handler carries a
    // DeviceRegistrationService::PairAttempt (move-only precisely because
    // copying it would double-restore the certificate pin).
    //
    // std::move_only_function would be the direct answer, but it is C++23
    // and this project is on C++20. A shared_ptr indirection costs one
    // allocation per dispatch, against a network round trip.
    template<typename F>
    static std::function<void()> makeCopyable(F&& callable)
    {
        auto shared = std::make_shared<std::decay_t<F>>(std::forward<F>(callable));
        return [shared]() { (*shared)(); };
    }

    void postToExecutor(std::function<void()> task);
    HttpClient& httpClientOnExecutorThread();
    // Checked by every task before it does anything. Cleared by shutdown()
    // BEFORE the thread is asked to quit.
    //
    // Without it, whether an already-queued task ran was a race:
    // QThread::quit() is QEventLoop::exit(), whose flag is tested per
    // iteration, while a single processEvents() pass can deliver several
    // posted events -- so a task queued behind a running one sometimes ran
    // after shutdown had been requested and sometimes did not. A teardown
    // path that is nondeterministic about whether it touches
    // being-destroyed state is not a teardown path.
    bool isAcceptingWork() const;

    QAtomicInteger<bool> m_acceptingWork{ true };
    QThread m_thread;
    // Lives on m_thread; owns the QNetworkAccessManager and HttpClient so
    // that both are constructed, used and destroyed there.
    class Worker;
    Worker* m_worker = nullptr;
};
