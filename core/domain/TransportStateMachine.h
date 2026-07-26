#pragma once

#include "models/PushNotification.h"

#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVector>

class NtfySubscriber;
class PushRepository;

enum class TransportTier { Distributor, EmbeddedSubscriber, Polling };

// Owns the three-tier push-delivery fallback decision and the
// timer/subscriber lifecycle behind it, per Linux_QT_Client_Plan.md's
// "Transport state machine (shared)" section (lines 218-224): "distributor
// present -> KUnifiedPush; else -> embedded ntfy subscriber while
// foregrounded; subscriber unreachable -> 90s polling." This class never
// links KUnifiedPush itself (core/ links only Qt6::Core/Network/Sql) --
// "distributor available" and "foregrounded" are both externally-reported
// state from app-layer code in a later phase, not detected here.
class TransportStateMachine : public QObject
{
    Q_OBJECT
public:
    // pollIntervalMs mirrors NtfySubscriber's own reconnectDelayMs pattern
    // (core/net/NtfySubscriber.h): a constructor-overridable knob, defaulted
    // to the plan doc's real 90s cadence, so tests don't have to wait out
    // 90 real seconds to exercise the polling tier's timer-driven fetch
    // path.
    TransportStateMachine(NtfySubscriber& subscriber, PushRepository& pushRepository,
                           QObject* parent = nullptr, int pollIntervalMs = 90000,
                           int subscriberRetryAfterMs = 5 * 60 * 1000);

    // Consecutive subscriber reconnect failures tolerated before demoting to
    // Polling. NtfySubscriber retries with its own exponential backoff in
    // between, so this is "the server has been unreachable for a while", not
    // "one packet was lost".
    static constexpr int kSubscriberFailureBudget = 3;

    // Called by app-layer code (a later phase) when the KUnifiedPush
    // distributor path is confirmed working (endpoint acquired, registered)
    // or lost (no distributor installed, or the connector reports
    // failure). true -> Distributor tier, stop the embedded subscriber and
    // polling timer if either is running. false -> falls through to
    // EmbeddedSubscriber (or Polling) per the current foreground state.
    void setDistributorAvailable(bool available);

    // Foreground/background is also app-layer-owned (window focus, or UT
    // suspend/resume) -- the embedded subscriber tier is foreground-only
    // per the plan doc, so a caller reports this too, independent of
    // distributor availability.
    void setForegrounded(bool foregrounded);

    TransportTier currentTier() const;

    // Re-runs the tier decision from the current inputs. Called internally
    // on the retry timer after a demotion to Polling; public so a host can
    // also force a re-evaluation (e.g. on a network-came-back signal).
    void reevaluateTier();

signals:
    void tierChanged(TransportTier tier);
    // Forwards NtfySubscriber::messageReceived while in the
    // EmbeddedSubscriber tier only; polling-tier arrivals surface through
    // PushRepository's own pull results instead, via pollTick.
    void notificationReceived(const QJsonObject& data);
    // Emitted after each polling-tier PushRepository::pullOnce() call
    // (whether or not it returned anything) -- a later phase's UI wires
    // this to a "last checked at" indicator.
    void pollTick(QVector<PushNotification> delivered);

private slots:
    void onSubscriberConnectionLost(const QString& reason, int consecutiveFailures);
    void onPollTimer();

private:
    void enterTier(TransportTier tier);
    // Re-runs the tier-selection decision from current
    // m_distributorAvailable/m_foregrounded state -- called from both
    // public setters even when the reported value didn't change, since
    // enterTier() re-attempting the target tier is exactly how the
    // "subscriber unreachable -> polling, then foregrounded reported again
    // -> retry the embedded subscriber" edge (no latched degraded flag)
    // works.
    void selectTier();

    NtfySubscriber& m_subscriber;
    PushRepository& m_pushRepository;
    QTimer m_pollTimer; // 90s interval, per the plan doc's stated cadence
    // Fires once after a subscriber-driven demotion to Polling, to try the
    // EmbeddedSubscriber tier again. Without it the demotion was a one-way
    // ratchet: selectTier() only ran from setDistributorAvailable/
    // setForegrounded, so a single dropped connection left a still-
    // foregrounded app on 90-second polling until the user alt-tabbed away
    // and back.
    QTimer m_subscriberRetryTimer;
    bool m_distributorAvailable = false;
    bool m_foregrounded = false;
    TransportTier m_tier = TransportTier::Polling;
};

// Both types cross a signal boundary above (tierChanged, pollTick) --
// QSignalSpy/queued connections need them registered as Qt metatypes to
// capture the arguments, which plain "enum class"/QVector<CustomStruct>
// don't get for free.
Q_DECLARE_METATYPE(TransportTier)
Q_DECLARE_METATYPE(QVector<PushNotification>)
