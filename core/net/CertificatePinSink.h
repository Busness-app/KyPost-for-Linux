#pragma once

#include "net/HttpClient.h"

#include <functional>
#include <vector>

class NetworkExecutor;

// The one place TOFU certificate-pin state is installed, so it cannot be
// installed in only some of the places that need it.
//
// WHY THIS EXISTS
//
// The pin lives inside HttpClient, and while Relay HTTP is migrating onto a
// worker thread there is more than one HttpClient in the process: the
// executor's, used by converted controllers, and the GUI-thread one still
// serving the rest (see docs/THREADING.md). Every pin mutation used to name
// an HttpClient directly, which meant the first conversion silently shipped
// an UNPINNED code path -- MfaController's requests went to the executor's
// client, which nobody had ever given a pin or a mismatch handler. The
// device secret went out over whatever certificate the CA chain accepted,
// and the "your mail server is being impersonated" banner could not fire
// because there was nothing to compare against.
//
// Writing "remember to set it on both" in a comment would have been the same
// bug waiting for the next conversion. This is the structural version: one
// sink, handed to everything that mutates pin state, fanning out to every
// client that must honour it. When the GUI-thread client is retired, one
// entry is removed from the fan-out and nothing else changes.
class CertificatePinSink
{
public:
    virtual ~CertificatePinSink() = default;

    // The current pin and the origin it is scoped to. Every underlying
    // client is kept identical by construction, so any of them answers.
    virtual HttpClient::CertificatePinState pinState() const = 0;

    virtual void setPin(const QByteArray& spkiSha256, const QUrl& origin) = 0;
    virtual void clearPin() = 0;
    virtual void restorePin(const HttpClient::CertificatePinState& state) = 0;

    // Installed on every client too, for the same reason: a mismatch
    // detected on a path whose client has no handler aborts the request and
    // tells the user nothing.
    //
    // The handler is invoked on whichever thread the request ran on, which
    // is NOT necessarily the GUI thread. Implementations of it must marshal
    // anything that touches a QObject.
    virtual void setMismatchHandler(HttpClient::CertificateMismatchHandler handler) = 0;
};

// Adapts one HttpClient owned by, and only ever touched from, the calling
// thread.
class HttpClientPinSink : public CertificatePinSink
{
public:
    explicit HttpClientPinSink(HttpClient& httpClient);

    HttpClient::CertificatePinState pinState() const override;
    void setPin(const QByteArray& spkiSha256, const QUrl& origin) override;
    void clearPin() override;
    void restorePin(const HttpClient::CertificatePinState& state) override;
    void setMismatchHandler(HttpClient::CertificateMismatchHandler handler) override;

private:
    HttpClient& m_httpClient;
};

// Adapts the HttpClient that lives on a NetworkExecutor's worker thread.
//
// Every operation goes through NetworkExecutor::configure(), which applies
// the change ON that thread and blocks until it has. Writing these fields
// from here directly would race a request reading them mid-handshake.
class NetworkExecutorPinSink : public CertificatePinSink
{
public:
    explicit NetworkExecutorPinSink(NetworkExecutor& executor);

    HttpClient::CertificatePinState pinState() const override;
    void setPin(const QByteArray& spkiSha256, const QUrl& origin) override;
    void clearPin() override;
    void restorePin(const HttpClient::CertificatePinState& state) override;
    void setMismatchHandler(HttpClient::CertificateMismatchHandler handler) override;

private:
    NetworkExecutor& m_executor;
};

// Applies every mutation to all of its targets, in order.
//
// pinState() reads the FIRST target: the whole point is that they never
// disagree, and reading one is how a caller such as
// DeviceRegistrationService's ScopedPinSuspension snapshots what to restore.
class FanOutCertificatePinSink : public CertificatePinSink
{
public:
    explicit FanOutCertificatePinSink(std::vector<CertificatePinSink*> targets);

    HttpClient::CertificatePinState pinState() const override;
    void setPin(const QByteArray& spkiSha256, const QUrl& origin) override;
    void clearPin() override;
    void restorePin(const HttpClient::CertificatePinState& state) override;
    void setMismatchHandler(HttpClient::CertificateMismatchHandler handler) override;

private:
    std::vector<CertificatePinSink*> m_targets;
};
