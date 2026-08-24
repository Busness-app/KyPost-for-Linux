#include "platform/SecureStoreKeychain.h"

#include <QEventLoop>
#include <QTimer>
#include <QtLogging>
#include <qt6keychain/keychain.h>

SecureStoreKeychain::SecureStoreKeychain(const QString& service, int timeoutMs,
                                         const QString& legacyService)
    : m_service(service)
    , m_timeoutMs(timeoutMs)
    , m_legacyService(legacyService)
    , m_legacyReachable(!legacyService.isEmpty())
{
}

SecureStoreKeychain::JobOutcome SecureStoreKeychain::runBlocking(QKeychain::Job* job) const
{
    JobOutcome outcome;
    QEventLoop loop;

    // `&loop` as the context object is load-bearing, not style. Both
    // connections are scoped to this stack frame, so that on the timeout path
    // -- where we return while the job is still running -- neither the lambda
    // below (which captures `outcome` and `loop` by reference) nor the timer
    // can fire into a dead frame afterwards.
    QObject::connect(job, &QKeychain::Job::finished, &loop, [&outcome, &loop](QKeychain::Job* finished) {
        outcome.completed = true;
        outcome.error = static_cast<int>(finished->error());
        // Read here, while the job is guaranteed alive: autoDelete is left on,
        // so the job reaps itself once this handler returns.
        if (auto* read = qobject_cast<QKeychain::ReadPasswordJob*>(finished))
            outcome.textData = read->textData();
        loop.quit();
    });
    // The only exit that is guaranteed to happen. QKeychain does not emit
    // `finished` when its underlying D-Bus call gives up (measured -- see the
    // header), so `finished` alone left this loop with no way out at all.
    //
    // Note what this timer cannot do: the first ~25 s are spent inside a
    // synchronous D-Bus call with the thread not processing events, so it
    // will not be delivered until that returns regardless of m_timeoutMs.
    // It bounds the wait; it does not shorten it.
    QTimer::singleShot(m_timeoutMs, &loop, &QEventLoop::quit);

    job->start();
    // A job that completed synchronously inside start() already quit a loop
    // that was not running yet; entering it now would block until the timeout
    // for no reason. The version this replaced had the same hazard and simply
    // never hit it.
    if (!outcome.completed)
        loop.exec();
    return outcome;
}

bool SecureStoreKeychain::set(const QString& key, const QString& value)
{
    return writeTo(m_service, key, value);
}

bool SecureStoreKeychain::writeTo(const QString& service, const QString& key,
                                  const QString& value) const
{
    auto* job = new QKeychain::WritePasswordJob(service);
    job->setKey(key);
    job->setTextData(value);

    const JobOutcome outcome = runBlocking(job);
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
    auto* job = new QKeychain::ReadPasswordJob(service);
    job->setKey(key);

    const JobOutcome outcome = runBlocking(job);

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
    if (m_legacyReachable && !removeFrom(m_legacyService, key)) {
        m_legacyReachable = false;
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
    auto* job = new QKeychain::DeletePasswordJob(service);
    job->setKey(key);

    const JobOutcome outcome = runBlocking(job);
    if (!outcome.completed)
        return false;
    return outcome.error == QKeychain::NoError || outcome.error == QKeychain::EntryNotFound;
}

bool SecureStoreKeychain::contains(const QString& key) const
{
    return get(key).has_value();
}
