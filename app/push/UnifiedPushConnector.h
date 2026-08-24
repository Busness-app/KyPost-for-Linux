#pragma once

#include <QObject>
#include <QString>

#include <KUnifiedPush/Connector>

// Thin wrapper around KUnifiedPush::Connector (the system UnifiedPush client
// library, /usr/include/KUnifiedPush/kunifiedpush/connector.h). Lives in
// app/, never core/, because it links against a QtDBus-adjacent system
// library, consistent with the existing core/ boundary rule (see
// SecureStoreKeychain).
//
// Task 10 built this as throwaway proof-of-concept (logged the acquired
// endpoint URL and incoming push message bytes to stdout via qDebug() and
// nothing else). Task 41 turns it into a real emitting wrapper: the same
// three KUnifiedPush::Connector signals are now re-emitted as same-shaped
// signals on this class (see push/PushPayloadParser.h /
// push/NotificationDispatcher.h -- app/main.cpp's composition root connects
// these to the distributor-tier arrival and re-registration paths). The
// endpointChanged host-only qDebug() line from Task 10 is preserved in the
// new emitting lambda rather than dropped.
class UnifiedPushConnector : public QObject
{
    Q_OBJECT
public:
    // serviceName is the app-unique identifier registered with the local
    // UnifiedPush distributor over D-Bus, e.g. "com.kysecurity.mail".
    explicit UnifiedPushConnector(const QString& serviceName, QObject* parent = nullptr);

    // Subscribes with the local distributor. Persisted until explicitly
    // unregistered; safe to call on every startup.
    void registerClient(const QString& description);

    // Pass-through accessors onto the wrapped m_connector -- endpoint() is
    // needed by main.cpp to build the deviceToken for
    // DeviceRegistrationService::reregisterIfPaired() on endpointChanged.
    QString endpoint() const;
    KUnifiedPush::Connector::State state() const;

Q_SIGNALS:
    void endpointChanged(const QString& endpoint);
    void messageReceived(const QByteArray& message);
    void stateChanged(KUnifiedPush::Connector::State state);

private:
    KUnifiedPush::Connector* m_connector;
};
