#include "domain/TransportStateMachine.h"

#include "domain/PushRepository.h"
#include "net/NtfySubscriber.h"

TransportStateMachine::TransportStateMachine(NtfySubscriber& subscriber, PushRepository& pushRepository,
                                               QObject* parent, int pollIntervalMs, int subscriberRetryAfterMs)
    : QObject(parent)
    , m_subscriber(subscriber)
    , m_pushRepository(pushRepository)
{
    m_pollTimer.setInterval(pollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &TransportStateMachine::onPollTimer);

    // Single-shot: one promotion attempt per demotion. If the subscriber is
    // still unreachable, its own failures demote again and re-arm this,
    // which keeps the retry rate bounded rather than growing a second
    // unbounded loop next to NtfySubscriber's own backoff.
    m_subscriberRetryTimer.setSingleShot(true);
    m_subscriberRetryTimer.setInterval(subscriberRetryAfterMs);
    connect(&m_subscriberRetryTimer, &QTimer::timeout, this, &TransportStateMachine::reevaluateTier);

    connect(&m_subscriber, &NtfySubscriber::connectionLost, this,
            &TransportStateMachine::onSubscriberConnectionLost);
    connect(&m_subscriber, &NtfySubscriber::messageReceived, this, [this](const QJsonObject& data) {
        if (m_tier == TransportTier::EmbeddedSubscriber)
            emit notificationReceived(data);
    });

    // m_tier already defaults to Polling (see header); there is no
    // "previous" tier to leave, so start its timer directly rather than
    // going through enterTier(), which only acts on an actual transition.
    m_pollTimer.start();
}

void TransportStateMachine::setDistributorAvailable(bool available)
{
    m_distributorAvailable = available;
    selectTier();
}

void TransportStateMachine::setForegrounded(bool foregrounded)
{
    m_foregrounded = foregrounded;
    selectTier();
}

TransportTier TransportStateMachine::currentTier() const
{
    return m_tier;
}

void TransportStateMachine::reevaluateTier()
{
    selectTier();
}

void TransportStateMachine::selectTier()
{
    if (m_distributorAvailable)
        enterTier(TransportTier::Distributor);
    else if (m_foregrounded)
        enterTier(TransportTier::EmbeddedSubscriber);
    else
        enterTier(TransportTier::Polling);
}

void TransportStateMachine::onSubscriberConnectionLost(const QString&, int consecutiveFailures)
{
    if (m_tier != TransportTier::EmbeddedSubscriber)
        return;

    // Spend the failure budget before demoting. NtfySubscriber schedules its
    // own exponentially-backed-off reconnect after emitting this, so doing
    // nothing here IS the retry. Demoting on the very first drop -- the old
    // behaviour -- meant a two-second Wi-Fi blip or a suspend/resume cycle
    // permanently parked a foregrounded app on 90-second polling, which on a
    // machine with no UnifiedPush distributor is the whole reason this tier
    // exists.
    if (consecutiveFailures < kSubscriberFailureBudget)
        return;

    // Budget spent: the server is genuinely unreachable. Fall back to
    // polling (enterTier stops the subscriber, cancelling its pending
    // reconnect), but arm the retry timer so this is a temporary demotion
    // rather than a permanent one.
    enterTier(TransportTier::Polling);
    m_subscriberRetryTimer.start();
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

    // Any transition is a fresh decision -- drop a pending promotion attempt
    // so it cannot fire later and contradict it.
    if (tier != TransportTier::Polling)
        m_subscriberRetryTimer.stop();

    if (tier != TransportTier::EmbeddedSubscriber)
        m_subscriber.stop();
    if (tier != TransportTier::Polling)
        m_pollTimer.stop();

    if (tier == TransportTier::EmbeddedSubscriber)
        m_subscriber.start();
    else if (tier == TransportTier::Polling)
        m_pollTimer.start();

    emit tierChanged(tier);
}
