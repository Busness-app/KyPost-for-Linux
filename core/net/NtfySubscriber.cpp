#include "net/NtfySubscriber.h"

#include "net/HttpClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

NtfySubscriber::NtfySubscriber(QNetworkAccessManager& manager, const QString& baseUrl, const QString& topic,
                                QObject* parent, int reconnectDelayMs)
    : QObject(parent)
    , m_manager(manager)
    , m_baseUrl(baseUrl)
    , m_topic(topic)
    , m_reconnectDelayMs(reconnectDelayMs)
{
}

void NtfySubscriber::start(qint64 since)
{
    m_since = since;
    m_stopped = false;
    m_consecutiveFailures = 0;
    sendRequest();
}

void NtfySubscriber::setTopic(const QString& topic)
{
    if (m_topic == topic)
        return;
    m_topic = topic;
    m_consecutiveFailures = 0;
    // A rotated topic has no shared history with the old one, so resuming
    // from the previous `since` would be meaningless. Start fresh.
    m_since = 0;
    if (!m_stopped)
        sendRequest();
}

QString NtfySubscriber::topic() const
{
    return m_topic;
}

int NtfySubscriber::currentReconnectDelayMs() const
{
    // Doubling, capped. m_consecutiveFailures is already >= 1 when this is
    // read (incremented in onFinished before the retry is scheduled), so the
    // first retry waits exactly m_reconnectDelayMs.
    qint64 delay = m_reconnectDelayMs;
    for (int i = 1; i < m_consecutiveFailures && delay < kMaxReconnectDelayMs; ++i)
        delay *= 2;
    return static_cast<int>(qMin<qint64>(delay, kMaxReconnectDelayMs));
}

void NtfySubscriber::stop()
{
    m_stopped = true;
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void NtfySubscriber::sendRequest()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_lineBuffer.clear();

    QUrl url = joinUrlPath(QUrl(m_baseUrl), m_topic + QStringLiteral("/json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("since"), QString::number(m_since));
    url.setQuery(query);

    m_reply = m_manager.get(QNetworkRequest(url));
    connect(m_reply, &QNetworkReply::readyRead, this, &NtfySubscriber::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &NtfySubscriber::onFinished);
}

void NtfySubscriber::onReadyRead()
{
    if (!m_reply)
        return;

    m_lineBuffer += m_reply->readAll();

    int newlineIndex;
    while ((newlineIndex = m_lineBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_lineBuffer.left(newlineIndex);
        m_lineBuffer.remove(0, newlineIndex + 1);
        processLine(line);
    }
}

void NtfySubscriber::onFinished()
{
    if (!m_reply)
        return;

    QNetworkReply* reply = m_reply;
    m_reply = nullptr;

    const QString reason = reply->errorString();
    reply->deleteLater();

    if (m_stopped)
        return;

    ++m_consecutiveFailures;
    emit connectionLost(reason, m_consecutiveFailures);

    // A listener may have called stop() from the signal above (that is
    // exactly what TransportStateMachine does once the failure budget is
    // spent) -- re-check before scheduling, or a demoted tier would keep a
    // zombie reconnect running underneath the polling tier.
    if (m_stopped)
        return;

    // m_since was already advanced to the last processed message's `time`
    // by processLine(), so the retry below resumes from there rather than
    // from 0 -- no messages are lost across a reconnect.
    QTimer::singleShot(currentReconnectDelayMs(), this, [this]() {
        if (!m_stopped)
            sendRequest();
    });
}

void NtfySubscriber::processLine(const QByteArray& line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(trimmed);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    // "open"/"keepalive" lines exist only to hold the long-poll connection
    // open and carry no payload -- only "message" is a real notification
    // (docs.ntfy.sh/subscribe/api/#json-stream).
    if (obj.value(QStringLiteral("event")).toString() != QStringLiteral("message"))
        return;

    if (obj.contains(QStringLiteral("time")))
        m_since = static_cast<qint64>(obj.value(QStringLiteral("time")).toDouble());

    // A real message proves the connection works: reset the backoff so a
    // long-lived subscription that drops after hours retries promptly rather
    // than at whatever delay an earlier outage had escalated to.
    m_consecutiveFailures = 0;

    emit messageReceived(obj);
}
