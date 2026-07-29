#include "net/NetworkExecutor.h"

#include "net/HttpClient.h"

#include <QNetworkAccessManager>

#include <memory>

// Lives on the executor thread and owns everything that must never be
// touched from anywhere else.
//
// The QNetworkAccessManager and HttpClient are created LAZILY, on first use
// from the executor thread, rather than in the constructor. That is not
// convenience: a QObject constructed in NetworkExecutor's constructor would
// have GUI-thread affinity, and QNetworkAccessManager genuinely misbehaves
// when its affinity does not match the thread driving it. Creating them
// inside a task guarantees they are born on the thread that will use them.
class NetworkExecutor::Worker : public QObject
{
    Q_OBJECT

public:
    explicit Worker(int transferTimeoutMs)
        : m_transferTimeoutMs(transferTimeoutMs)
    {
    }

    HttpClient& httpClient()
    {
        Q_ASSERT_X(QThread::currentThread() == thread(), "NetworkExecutor::Worker::httpClient",
                   "the HttpClient may only be touched on the executor thread");
        if (!m_httpClient) {
            m_manager = std::make_unique<QNetworkAccessManager>();
            m_httpClient = std::make_unique<HttpClient>(*m_manager, m_transferTimeoutMs);
        }
        return *m_httpClient;
    }

private:
    int m_transferTimeoutMs;
    // Declaration order matters: m_httpClient holds a reference to
    // *m_manager, so the manager must outlive it. Members are destroyed in
    // reverse declaration order, so the client goes first.
    std::unique_ptr<QNetworkAccessManager> m_manager;
    std::unique_ptr<HttpClient> m_httpClient;
};

NetworkExecutor::NetworkExecutor(int transferTimeoutMs, QObject* parent)
    : QObject(parent)
    , m_worker(new Worker(transferTimeoutMs))
{
    m_thread.setObjectName(QStringLiteral("kypost-net"));
    m_worker->moveToThread(&m_thread);
    // Destroyed on its own thread, which is the only thread allowed to
    // destroy the QNetworkAccessManager it owns.
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

NetworkExecutor::~NetworkExecutor()
{
    shutdown();
}

void NetworkExecutor::shutdown()
{
    if (!m_thread.isRunning())
        return;
    // What this guarantees, precisely, because the difference matters at
    // teardown:
    //
    //   * work already EXECUTING runs to completion -- wait() does not
    //     return until the thread has left its event loop, and the loop
    //     cannot be left mid-task;
    //   * work still QUEUED and not yet started does NOT run;
    //   * therefore no callback can fire after this returns.
    //
    // Discarding queued work is the behaviour we want: the only caller is a
    // teardown path, and running more network work against stores and
    // controllers that are already being destroyed is strictly worse than
    // dropping a request whose answer nobody will see.
    //
    // The flag, not quit(), is what makes that deterministic. quit() is
    // QEventLoop::exit(), whose flag is tested once per loop iteration,
    // while a single processEvents() pass can deliver several posted events
    // -- so relying on quit() alone left "did the queued task run?" decided
    // by how Qt happened to batch that pass. Measured, not assumed: the test
    // for this failed intermittently until the flag was added.
    m_acceptingWork.storeRelease(false);
    m_thread.quit();
    m_thread.wait();
    m_worker = nullptr; // deleteLater above ran as the loop exited
}

void NetworkExecutor::postToExecutor(std::function<void()> task)
{
    if (!m_thread.isRunning()) {
        // After shutdown() there is nothing to run on. Dropping the task is
        // correct: the only caller is a controller being torn down, and the
        // alternative -- running network code on the GUI thread as a
        // fallback -- would silently reintroduce exactly what this class
        // exists to prevent.
        qWarning("NetworkExecutor: dropping work submitted after shutdown");
        return;
    }
    QMetaObject::invokeMethod(m_worker, std::move(task), Qt::QueuedConnection);
}

bool NetworkExecutor::isAcceptingWork() const
{
    return m_acceptingWork.loadAcquire();
}

HttpClient& NetworkExecutor::httpClientOnExecutorThread()
{
    return m_worker->httpClient();
}

void NetworkExecutor::configure(const std::function<void(HttpClient&)>& configuration)
{
    if (!m_thread.isRunning()) {
        qWarning("NetworkExecutor: ignoring configuration submitted after shutdown");
        return;
    }
    if (QThread::currentThread() == &m_thread) {
        // Already there. A BlockingQueuedConnection to our own thread would
        // deadlock outright.
        configuration(m_worker->httpClient());
        return;
    }
    QMetaObject::invokeMethod(
        m_worker, [this, &configuration]() { configuration(m_worker->httpClient()); },
        Qt::BlockingQueuedConnection);
}

#include "NetworkExecutor.moc"
