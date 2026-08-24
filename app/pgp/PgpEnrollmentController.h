#pragma once

#include "domain/DevicePairing.h"
#include "net/RelayAuth.h"
#include "pgp/DeviceEnrollmentCrypto.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QUrl>

class NetworkExecutor;
class PairingStore;

class PgpEnrollmentController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int state READ state NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString verificationCode READ verificationCode NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

public:
    enum State { Idle, Starting, Waiting, TimedOut, Importing, Enrolled, Failed };
    Q_ENUM(State)

    PgpEnrollmentController(PairingStore& pairingStore, NetworkExecutor& executor,
                            QObject* parent = nullptr);
    int state() const { return m_state; }
    QString status() const { return m_status; }
    QString verificationCode() const { return m_code; }
    bool busy() const { return m_state == Starting || m_state == Waiting || m_state == Importing; }

    Q_INVOKABLE void start();
    Q_INVOKABLE void checkAgain();
    Q_INVOKABLE void cancel();
    void pairingMayHaveChanged();

signals:
    void changed();
    void enrolled();

private:
    struct StartResult;
    void setState(State state, const QString& status);
    void refreshCode();
    void poll();
    void finishWithFailure(const QString& status);

    PairingStore& m_pairingStore;
    NetworkExecutor& m_executor;
    DeviceEnrollmentCrypto m_crypto;
    QTimer m_pollTimer;
    QTimer m_codeTimer;
    QElapsedTimer m_pollWindow;
    State m_state = Idle;
    QString m_status;
    QString m_code;
    QString m_fingerprint;
    QUrl m_serverBaseUrl;
    RelayAuth m_auth;
    PairingIdentity m_identity;
    static constexpr qint64 kPollWindowMs = 5 * 60 * 1000;
};
