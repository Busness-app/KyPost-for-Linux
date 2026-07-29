#include "net/CertificatePinSink.h"

#include "net/NetworkExecutor.h"

HttpClientPinSink::HttpClientPinSink(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

HttpClient::CertificatePinState HttpClientPinSink::pinState() const
{
    return m_httpClient.certificatePinState();
}

void HttpClientPinSink::setPin(const QByteArray& spkiSha256, const QUrl& origin)
{
    m_httpClient.setCertificatePin(spkiSha256, origin);
}

void HttpClientPinSink::clearPin()
{
    m_httpClient.clearCertificatePin();
}

void HttpClientPinSink::restorePin(const HttpClient::CertificatePinState& state)
{
    m_httpClient.restoreCertificatePin(state);
}

void HttpClientPinSink::setMismatchHandler(HttpClient::CertificateMismatchHandler handler)
{
    m_httpClient.setCertificateMismatchHandler(std::move(handler));
}

NetworkExecutorPinSink::NetworkExecutorPinSink(NetworkExecutor& executor)
    : m_executor(executor)
{
}

HttpClient::CertificatePinState NetworkExecutorPinSink::pinState() const
{
    HttpClient::CertificatePinState state;
    m_executor.configure([&state](HttpClient& http) { state = http.certificatePinState(); });
    return state;
}

void NetworkExecutorPinSink::setPin(const QByteArray& spkiSha256, const QUrl& origin)
{
    m_executor.configure(
        [&spkiSha256, &origin](HttpClient& http) { http.setCertificatePin(spkiSha256, origin); });
}

void NetworkExecutorPinSink::clearPin()
{
    m_executor.configure([](HttpClient& http) { http.clearCertificatePin(); });
}

void NetworkExecutorPinSink::restorePin(const HttpClient::CertificatePinState& state)
{
    m_executor.configure([&state](HttpClient& http) { http.restoreCertificatePin(state); });
}

void NetworkExecutorPinSink::setMismatchHandler(HttpClient::CertificateMismatchHandler handler)
{
    m_executor.configure(
        [&handler](HttpClient& http) { http.setCertificateMismatchHandler(handler); });
}

FanOutCertificatePinSink::FanOutCertificatePinSink(std::vector<CertificatePinSink*> targets)
    : m_targets(std::move(targets))
{
    Q_ASSERT_X(!m_targets.empty(), "FanOutCertificatePinSink",
               "a fan-out with no targets silently disables pinning everywhere");
}

HttpClient::CertificatePinState FanOutCertificatePinSink::pinState() const
{
    // The first, not a merge: every mutation below reaches all of them, so
    // they cannot disagree. If they ever do, that is the bug, and returning
    // some reconciled value would hide it.
    return m_targets.front()->pinState();
}

void FanOutCertificatePinSink::setPin(const QByteArray& spkiSha256, const QUrl& origin)
{
    for (CertificatePinSink* target : m_targets)
        target->setPin(spkiSha256, origin);
}

void FanOutCertificatePinSink::clearPin()
{
    for (CertificatePinSink* target : m_targets)
        target->clearPin();
}

void FanOutCertificatePinSink::restorePin(const HttpClient::CertificatePinState& state)
{
    for (CertificatePinSink* target : m_targets)
        target->restorePin(state);
}

void FanOutCertificatePinSink::setMismatchHandler(HttpClient::CertificateMismatchHandler handler)
{
    for (CertificatePinSink* target : m_targets)
        target->setMismatchHandler(handler);
}
