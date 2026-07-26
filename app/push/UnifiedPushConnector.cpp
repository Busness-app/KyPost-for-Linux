#include "push/UnifiedPushConnector.h"

#include <KUnifiedPush/Connector>

#include <QDebug>
#include <QUrl>

UnifiedPushConnector::UnifiedPushConnector(const QString& serviceName, QObject* parent)
    : QObject(parent)
    , m_connector(new KUnifiedPush::Connector(serviceName, this))
{
    // connector.h documents serviceName as "the application identifier, same
    // as used for registration on D-Bus and for D-Bus activation" -- the
    // distributor delivers NewEndpoint/Message calls to this well-known bus
    // name (observed via `busctl --user monitor`: it calls
    // Destination=<serviceName> Path=/org/unifiedpush/Connector
    // Interface=org.unifiedpush.Connector2). KUnifiedPush::Connector does
    // not claim this name itself -- but this process's KDBusService
    // (constructed in main.cpp before this class) already has, and it's the
    // same D-Bus connection, so no registration call belongs here. See the
    // comment at the KDBusService construction site in main.cpp.

    connect(m_connector, &KUnifiedPush::Connector::endpointChanged, this, [this](const QString& endpoint) {
        // The endpoint URL's path/query (e.g. https://unifiedpush.kde.org/upezZkYjE4N2E2?up=1)
        // is a bearer secret: anyone who obtains it can push arbitrary notifications to
        // this app instance. Log only the host (not secret -- distributor hostnames like
        // unifiedpush.kde.org are public) so distributor choice is still debuggable without
        // writing the credential to the journal.
        qDebug() << "UnifiedPushConnector: endpoint changed, host:" << QUrl(endpoint).host();
        emit endpointChanged(endpoint);
    });

    connect(m_connector, &KUnifiedPush::Connector::messageReceived, this, [this](const QByteArray& msg) {
        emit messageReceived(msg);
    });

    connect(m_connector, &KUnifiedPush::Connector::stateChanged, this, [this](KUnifiedPush::Connector::State state) {
        emit stateChanged(state);
    });
}

void UnifiedPushConnector::registerClient(const QString& description)
{
    // Same reasoning as the endpointChanged handler above: the endpoint is a bearer
    // secret and must not be logged in full, so only its host is reported here.
    qDebug() << "UnifiedPushConnector: registering client, current state:" << m_connector->state()
              << "current endpoint host:" << QUrl(m_connector->endpoint()).host();
    m_connector->registerClient(description);
}

QString UnifiedPushConnector::endpoint() const
{
    return m_connector->endpoint();
}

KUnifiedPush::Connector::State UnifiedPushConnector::state() const
{
    return m_connector->state();
}
