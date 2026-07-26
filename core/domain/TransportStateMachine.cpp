#include "domain/TransportStateMachine.h"

#include "domain/PushRepository.h"

TransportStateMachine::TransportStateMachine(PushRepository& pushRepository, QObject* parent,
                                               int pollIntervalMs)
    : QObject(parent)
    , m_pushRepository(pushRepository)
{
    m_pollTimer.setInterval(pollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &TransportStateMachine::onPollTimer);

    // m_tier already defaults to Polling (see header); there is no "previous"
    // tier to leave, so start its timer directly rather than going through
    // enterTier(), which only acts on an actual transition.
    m_pollTimer.start();
}

void TransportStateMachine::setDistributorAvailable(bool available)
{
    m_distributorAvailable = available;
    enterTier(available ? TransportTier::Distributor : TransportTier::Polling);
}

TransportTier TransportStateMachine::currentTier() const
{
    return m_tier;
}

void TransportStateMachine::onPollTimer()
{
    const QVector<PushNotification> delivered = m_pushRepository.pullOnce();
    emit pollTick(delivered);
}

void TransportStateMachine::enterTier(TransportTier tier)
{
    if (tier == m_tier)
        return;
    m_tier = tier;

    if (tier == TransportTier::Polling)
        m_pollTimer.start();
    else
        m_pollTimer.stop();

    emit tierChanged(tier);
}
