#pragma once

#include "net/ClientVersionClient.h"

#include <QObject>
#include <QString>
#include <QTimer>

class NetworkExecutor;
class PairingStore;

// Asks the paired server what the newest published Linux release is, and
// compares it against this build.
//
// SURFACES, NEVER ACTS. A Flatpak cannot update itself, so this produces a
// notice and a link and nothing else. Nothing here downloads, installs, or
// touches the host.
class UpdateCheckController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString installedVersion READ installedVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY changed)
    Q_PROPERTY(QString checkedAt READ checkedAt NOTIFY changed)
    Q_PROPERTY(QString releaseUrl READ releaseUrl CONSTANT)

public:
    UpdateCheckController(PairingStore& pairingStore, NetworkExecutor& executor,
                          QObject* parent = nullptr);

    // The build's own version, and the LEFT-HAND SIDE of every comparison.
    // Exposed as a static so a test can assert it tracks KYPOST_VERSION
    // rather than a second copy.
    static QString compiledInVersion();

    QString installedVersion() const { return compiledInVersion(); }
    QString latestVersion() const { return m_latestVersion; }
    bool updateAvailable() const { return m_updateAvailable; }
    QString checkedAt() const { return m_checkedAt; }
    QString releaseUrl() const;

    Q_INVOKABLE void checkNow();
    void pairingMayHaveChanged();

    // The pure state transition checkNow()'s network callback feeds into --
    // public so a test can drive it directly without a real HTTP round trip.
    // Not Q_INVOKABLE: this is a test seam, not something QML should call.
    void applyResult(const ClientVersionResult& result);

signals:
    void changed();
    // Raised on every false->true transition of updateAvailable, so a root
    // can show a toast without polling the property. NOT one-shot-per-run:
    // the poll timer keeps running for the life of the process, and a later
    // check can genuinely go back to false (e.g. a result whose version this
    // client cannot parse clears m_latestVersion) and then true again, which
    // re-raises this signal. Any "show it at most once per run" suppression
    // is the listener's job -- see UpdateToast.qml's `shown` latch -- not
    // this class's.
    void updateBecameAvailable();

private:
    PairingStore& m_pairingStore;
    NetworkExecutor& m_executor;
    QTimer m_pollTimer;
    QString m_latestVersion;
    QString m_checkedAt;
    bool m_updateAvailable = false;
};
