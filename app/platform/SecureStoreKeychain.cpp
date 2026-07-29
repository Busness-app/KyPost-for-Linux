#include "platform/SecureStoreKeychain.h"

#include <QEventLoop>
#include <qt6keychain/keychain.h>

SecureStoreKeychain::SecureStoreKeychain(const QString& service)
    : m_service(service)
{
}

// Shared connect-start-exec pattern every job below needs: runs `job`
// synchronously on a local QEventLoop, quitting it on QKeychain::Job's
// finished signal, and returns once the job has completed. Callers inspect
// job.error()/job.textData() themselves afterward -- this only owns the
// blocking-wait mechanics, not any per-job error interpretation.
bool SecureStoreKeychain::runBlocking(QKeychain::Job& job)
{
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    return job.error() == QKeychain::NoError;
}

bool SecureStoreKeychain::set(const QString& key, const QString& value)
{
    QKeychain::WritePasswordJob job(m_service);
    job.setAutoDelete(false);
    job.setKey(key);
    job.setTextData(value);

    return runBlocking(job);
}

SecureStore::ReadResult SecureStoreKeychain::read(const QString& key) const
{
    QKeychain::ReadPasswordJob job(m_service);
    job.setAutoDelete(false);
    job.setKey(key);

    if (runBlocking(job))
        return ReadResult{ ReadStatus::Found, job.textData() };
    // EntryNotFound is the ONLY error that means "there is no such secret".
    // Everything else -- NoBackendAvailable, AccessDenied, AccessDeniedByUser,
    // OtherError from a D-Bus timeout -- means the store could not be
    // consulted, which is a different answer and must not be reported as
    // absence. See SecureStore::ReadStatus for what that conflation cost.
    if (job.error() == QKeychain::EntryNotFound)
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
    QKeychain::DeletePasswordJob job(m_service);
    job.setAutoDelete(false);
    job.setKey(key);

    if (runBlocking(job))
        return true;
    return job.error() == QKeychain::EntryNotFound;
}

bool SecureStoreKeychain::contains(const QString& key) const
{
    return get(key).has_value();
}
