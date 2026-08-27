#include "platform/SecureStoreKeychain.h"

#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <QtLogging>
#include <qt6keychain/keychain.h>

namespace {

// ONE worker thread for every SecureStoreKeychain in the process, and it is
// deliberately never joined or deleted.
//
// One per store was tried first and is worse than it looks. Each thread gets
// its own QDBusConnection, so on a machine whose Secret Service does not
// answer, every store pays Qt's full ~25 s D-Bus timeout independently instead
// of the first one paying it and the rest returning immediately -- measured at
// 83 s across one test run against 25 s shared. Startup builds several stores,
// so that is the case that matters.
//
// Never joined, because a job that timed out is still inside a blocked D-Bus
// call and will not come back until that timeout expires. Waiting for it would
// move the stall from startup to shutdown, which is not an improvement; and
// ~QThread on a running thread aborts, so the object has to outlive the
// process rather than be destroyed at exit. The cost is one thread in a
// session whose keyring is already broken.
QObject& keychainWorkerContext()
{
    static QObject* context = []() {
        auto* thread = new QThread; // never deleted, see above
        thread->setObjectName(QStringLiteral("kypost-keychain"));
        thread->start();
        // The receiver has to be an object that LIVES on that thread. A
        // QThread does NOT -- it belongs to whichever thread constructed it --
        // so dispatching through the QThread silently ran every job back on
        // the caller, which is the exact bug this indirection exists to fix.
        // It looked completely correct;
        // theCallingThreadIsReleasedByTheTimeoutNotByDBus is what caught it.
        auto* object = new QObject;
        object->moveToThread(thread);
        return object;
    }();
    return *context;
}

} // namespace

SecureStoreKeychain::SecureStoreKeychain(const QString& service, int timeoutMs,
                                         const QString& legacyService)
    : m_service(service)
    , m_timeoutMs(timeoutMs)
    , m_legacyService(legacyService)
    , m_legacyReachable(!legacyService.isEmpty())
{
}

SecureStoreKeychain::JobOutcome SecureStoreKeychain::runBlocking(const JobFactory& makeJob) const
{
    // Shared with the worker thread and outliving this frame on purpose. On
    // the timeout path we return while the job is still running, so anything
    // the worker may still write to cannot be a local -- and the mutex is not
    // ceremony either: "the caller gave up" and "the job finished" can happen
    // in the same instant on two threads.
    struct Payload
    {
        QMutex mutex;
        bool completed = false;
        int error = 0;
        QString textData;
    };
    const auto payload = std::make_shared<Payload>();

    QEventLoop loop;

    // THE WORKER TOUCHES NOTHING THIS FRAME OWNS. That is the rule here, and
    // it is why completion is polled rather than signalled.
    //
    // The obvious version -- connect the job's `finished` to `&loop` so it
    // quits directly -- was written first and crashes. The job does not exist
    // until the worker picks up the call below, so that connect() runs over
    // there, at a moment when this frame may already have timed out and
    // destroyed `loop`; with timeoutMs=0 it does so every single time. Qt can
    // sever a connection to a dead receiver but not one being made to it.
    //
    // Polling costs an extra wake-up every few milliseconds during a wait that
    // is normally milliseconds long and abnormally 25 seconds long. That is a
    // better trade than a lifetime rule which has to hold across two threads
    // and one event-loop teardown.
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [payload, &loop]() {
        const QMutexLocker locker(&payload->mutex);
        if (payload->completed)
            loop.quit();
    });
    poll.start();

    // The only exit that is guaranteed to happen. QKeychain does not emit
    // `finished` when its underlying D-Bus call gives up (measured -- see the
    // header), so waiting on completion alone left this loop with no way out.
    //
    // Unlike the version this replaced, the timer can actually be delivered:
    // the blocking D-Bus call is on the worker thread now, so this thread is
    // free to process its own events -- including this one, and including the
    // repaints that keep the window alive -- while it waits.
    QTimer::singleShot(m_timeoutMs, &loop, &QEventLoop::quit);

    QMetaObject::invokeMethod(
        &keychainWorkerContext(),
        [makeJob, payload]() {
            QKeychain::Job* job = makeJob();

            // DIRECT, because the receiver is the job itself and it lives on
            // this thread -- which is what makes it safe to dereference
            // `finished` here, and it has to be read here.
            //
            // QtKeychain leaves autoDelete on, so the job deleteLater()s
            // itself as soon as this signal has been delivered. Reading the
            // outcome from the caller's thread -- which the version before
            // this did -- meant reading it after the job was gone, and it
            // crashed in QKeychain::Job::error(). The comment it replaced
            // ("the job is guaranteed alive") had been true only while both
            // ends shared a thread.
            QObject::connect(job, &QKeychain::Job::finished, job,
                              [payload](QKeychain::Job* finished) {
                                  const QMutexLocker locker(&payload->mutex);
                                  payload->error = static_cast<int>(finished->error());
                                  if (auto* read = qobject_cast<QKeychain::ReadPasswordJob*>(finished))
                                      payload->textData = read->textData();
                                  // Written last: it is what the poll above
                                  // reads to decide the rest is populated.
                                  payload->completed = true;
                              });

            job->start();
        },
        Qt::QueuedConnection);

    loop.exec();

    const QMutexLocker locker(&payload->mutex);
    return JobOutcome{ payload->completed, payload->error, payload->textData };
}

bool SecureStoreKeychain::set(const QString& key, const QString& value)
{
    return writeTo(m_service, key, value);
}

bool SecureStoreKeychain::writeTo(const QString& service, const QString& key,
                                  const QString& value) const
{
    const JobOutcome outcome = runBlocking([service, key, value]() -> QKeychain::Job* {
        auto* job = new QKeychain::WritePasswordJob(service);
        job->setKey(key);
        job->setTextData(value);
        return job;
    });
    return outcome.completed && outcome.error == QKeychain::NoError;
}

SecureStore::ReadResult SecureStoreKeychain::read(const QString& key) const
{
    const ReadResult result = readFrom(m_service, key);

    // Found, or the store could not be consulted at all: the legacy service
    // has nothing to add either way, and asking would only cost another
    // blocking call.
    if (result.status != ReadStatus::Absent || !m_legacyReachable)
        return result;

    const ReadResult legacy = readFrom(m_legacyService, key);
    if (legacy.failed()) {
        m_legacyReachable = false;
        // Fail closed, once. See the header: the primary answering Absent
        // means the daemon is up, so this is not the ordinary
        // no-legacy-profile path -- that one returns EntryNotFound/Absent.
        return ReadResult{ ReadStatus::Failed, QString() };
    }
    if (!legacy.found())
        return result;

    // Copy forward, best-effort. A failure here costs nothing but a repeat of
    // this fallback on the next read, so it is not worth failing the read the
    // caller actually asked for.
    if (!writeTo(m_service, key, legacy.value))
        qWarning("SecureStoreKeychain: could not copy '%s' forward from the pre-rename service; "
                 "it will be read from there again next launch",
                 qUtf8Printable(key));
    return legacy;
}

SecureStore::ReadResult SecureStoreKeychain::readFrom(const QString& service, const QString& key) const
{
    const JobOutcome outcome = runBlocking([service, key]() -> QKeychain::Job* {
        auto* job = new QKeychain::ReadPasswordJob(service);
        job->setKey(key);
        return job;
    });

    // Timed out: the store could not be consulted. Reported as Failed, never
    // as Absent -- see the header. This is the branch that used to be an
    // infinite wait.
    if (!outcome.completed)
        return ReadResult{ ReadStatus::Failed, QString() };

    if (outcome.error == QKeychain::NoError)
        return ReadResult{ ReadStatus::Found, outcome.textData };
    // EntryNotFound is the ONLY error that means "there is no such secret".
    // Everything else -- NoBackendAvailable, AccessDenied, AccessDeniedByUser,
    // OtherError from a D-Bus timeout -- means the store could not be
    // consulted, which is a different answer and must not be reported as
    // absence. See SecureStore::ReadStatus for what that conflation cost.
    if (outcome.error == QKeychain::EntryNotFound)
        return ReadResult{ ReadStatus::Absent, QString() };
    return ReadResult{ ReadStatus::Failed, QString() };
}

std::optional<QString> SecureStoreKeychain::get(const QString& key) const
{
    const ReadResult result = read(key);
    return result.found() ? std::optional<QString>(result.value) : std::nullopt;
}

bool SecureStoreKeychain::remove(const QString& key)
{
    const bool removed = removeFrom(m_service, key);

    // Clear the pre-rename copy too. This is not tidiness: PairingStore::
    // clear() and the ten-failure wipe are both implemented as remove() over
    // their keys, and read() above resurrects anything the legacy service
    // still holds. Without this line a wipe the user was told had happened
    // would be silently undone on the next launch.
    //
    // m_legacyReachable is deliberately NOT consulted here; it is read()'s
    // latency guard alone. Letting it short-circuit meant every key after the
    // first failure skipped the legacy service and still reported success, so
    // a wipe that half-failed came back clean. Each key gets its own attempt
    // and its own answer, even if that costs the D-Bus timeout per key.
    if (!m_legacyService.isEmpty() && !removeFrom(m_legacyService, key)) {
        qWarning("SecureStoreKeychain: could not clear '%s' from the pre-rename service; a copy "
                 "may remain in the keyring",
                 qUtf8Printable(key));
        // The caller asked for this key to be gone and it is not gone
        // everywhere. Reporting success here is the reporting-a-write-that-
        // never-landed failure mode, so it reports the removal as failed.
        return false;
    }

    return removed;
}

bool SecureStoreKeychain::removeFrom(const QString& service, const QString& key)
{
    const JobOutcome outcome = runBlocking([service, key]() -> QKeychain::Job* {
        auto* job = new QKeychain::DeletePasswordJob(service);
        job->setKey(key);
        return job;
    });
    if (!outcome.completed)
        return false;
    return outcome.error == QKeychain::NoError || outcome.error == QKeychain::EntryNotFound;
}

bool SecureStoreKeychain::contains(const QString& key) const
{
    return get(key).has_value();
}
