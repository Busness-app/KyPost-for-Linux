#include "platform/SecureStoreKeychain.h"

#include <QEventLoop>
#include <QTimer>
#include <qt6keychain/keychain.h>

SecureStoreKeychain::SecureStoreKeychain(const QString& service, int timeoutMs)
    : m_service(service)
    , m_timeoutMs(timeoutMs)
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
    auto* job = new QKeychain::WritePasswordJob(m_service);
    job->setKey(key);
    job->setTextData(value);

    const JobOutcome outcome = runBlocking(job);
    return outcome.completed && outcome.error == QKeychain::NoError;
}

SecureStore::ReadResult SecureStoreKeychain::read(const QString& key) const
{
    auto* job = new QKeychain::ReadPasswordJob(m_service);
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
    auto* job = new QKeychain::DeletePasswordJob(m_service);
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
