#pragma once

// Shared real-QTcpServer test harness for core/net client tests. Originally
// hand-copied into HttpClientTest.cpp, NativeRegistrationClientTest.cpp, and
// PushNotificationClientTest.cpp (Tasks 13-14); lifted here verbatim as part
// of Task 15 so MfaResponseClientTest and any future net/ client test can
// share one copy instead of re-copying it again.

#include <QByteArray>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <memory>
#include <utility>

// Builds a raw HTTP/1.1 response with Content-Type/Content-Length/Connection
// headers set, for FakeRelayServer to hand back verbatim. contentType and
// extraHeaders default to the original JSON-only shape used by Tasks 13-17;
// Task 18's attachment-download test needs a raw (non-JSON) body plus a
// hand-written Content-Disposition header, hence the two added parameters
// rather than a second function -- existing 3-arg call sites are unaffected.
inline QByteArray httpResponse(int statusCode, const QByteArray& statusText, const QByteArray& body,
                                const QByteArray& contentType = "application/json",
                                const QList<QPair<QByteArray, QByteArray>>& extraHeaders = {})
{
    QByteArray response = "HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    for (const auto& header : extraHeaders)
        response += header.first + ": " + header.second + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;
    return response;
}

// Accepts connections on localhost, captures everything the client sends,
// and replies with a canned raw HTTP response once the full request (headers
// plus any Content-Length body) has arrived. Runs on the test's own event
// loop -- the same one HttpClient::get/post block on via their internal
// QEventLoop -- so plain signal/slot wiring is enough, no extra thread
// required.
class FakeRelayServer : public QObject
{
public:
    explicit FakeRelayServer(QByteArray response)
        : m_response(std::move(response))
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, &FakeRelayServer::onNewConnection);
    }

    quint16 port() const { return m_server.serverPort(); }
    const QByteArray& receivedRequest() const { return m_received; }

    // Changes what the NEXT connection is answered with. For tests that
    // drive two different endpoints through one controller -- refresh the
    // inbox, then act on a message -- which the async conversions made
    // necessary: the two calls can no longer be given a server each, because
    // the second one needs the state the first one produced.
    void setResponse(QByteArray response) { m_response = std::move(response); }

    // Answers requests whose path contains `needle` with `response`, ahead of
    // the default. For a flow that hits several endpoints in one asynchronous
    // run -- the client-encrypted send fetches an identity, resolves keys and
    // then posts -- setResponse() cannot work: the test thread has no moment
    // between the calls at which to change it.
    void setResponseForPath(const QByteArray& needle, QByteArray response)
    {
        m_byPath.append({ needle, std::move(response) });
    }

    // Every request received, in order, so a test can assert what a multi-step
    // flow actually asked for.
    const QList<QByteArray>& receivedRequests() const { return m_requests; }

    // How many TCP connections were accepted. Added for the async
    // controllers: "the second call while one was in flight was coalesced"
    // cannot be asserted from receivedRequest() alone, which only
    // accumulates bytes and would look identical whether one request or ten
    // arrived.
    int connectionCount() const { return m_connectionCount; }

    // Parses the JSON body out of the captured raw request.
    QJsonObject receivedJsonBody() const
    {
        const int headerEnd = m_received.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return {};
        const QByteArray body = m_received.mid(headerEnd + 4);
        return QJsonDocument::fromJson(body).object();
    }

private:
    void onNewConnection()
    {
        ++m_connectionCount;
        QTcpSocket* socket = m_server.nextPendingConnection();
        // Per-connection buffer, separate from the cumulative m_received that
        // tests read. "Has the whole request arrived?" must be judged from
        // THIS connection's bytes: asking it of the cumulative buffer finds
        // the first request's headers and Content-Length every time, so from
        // the second connection onwards it answers "yes" before anything has
        // actually been read -- and the reply goes out ahead of the POST body.
        auto request = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, request]() {
            const QByteArray chunk = socket->readAll();
            *request += chunk;
            m_received += chunk;
            if (!requestComplete(*request))
                return;
            m_requests.append(*request);
            socket->write(responseFor(*request));
            socket->flush();
            socket->disconnectFromHost();
        });
    }

    // First matching path wins; the default answers anything unmatched, so a
    // test only has to name the endpoints it cares about.
    QByteArray responseFor(const QByteArray& request) const
    {
        const int lineEnd = request.indexOf("\r\n");
        const QByteArray requestLine = lineEnd < 0 ? request : request.left(lineEnd);
        for (const auto& entry : m_byPath) {
            if (requestLine.contains(entry.first))
                return entry.second;
        }
        return m_response;
    }

    static bool requestComplete(const QByteArray& received)
    {
        const int headerEnd = received.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return false;
        const QByteArray headers = received.left(headerEnd);
        const int idx = headers.indexOf("Content-Length:");
        if (idx < 0)
            return true; // no body expected (e.g. GET)
        int lineEnd = headers.indexOf("\r\n", idx);
        if (lineEnd < 0)
            lineEnd = headers.size();
        bool ok = false;
        const int contentLength = headers.mid(idx + 15, lineEnd - idx - 15).trimmed().toInt(&ok);
        if (!ok)
            return true;
        return received.size() >= headerEnd + 4 + contentLength;
    }

    QTcpServer m_server;
    int m_connectionCount = 0;
    QList<QPair<QByteArray, QByteArray>> m_byPath;
    QList<QByteArray> m_requests;
    QByteArray m_response;
    QByteArray m_received;
};
