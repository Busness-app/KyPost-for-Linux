#include "domain/TrackedWipe.h"

#include "security/WipeTripwire.h"

TrackedWipe::TrackedWipe(WipeTripwire& tripwire, LocalDataWipe& wipe) : m_tripwire(tripwire), m_wipe(wipe)
{
}

TrackedWipe::Outcome TrackedWipe::wipeEverything()
{
    Outcome outcome;

    // Armed FIRST, before a single byte is erased. Arming afterwards would
    // leave the interruption this exists to catch -- power loss part-way
    // through -- recorded nowhere, because the code that would record it is
    // exactly the code that did not run.
    outcome.tripwireArmed = m_tripwire.arm();

    outcome.result = m_wipe.wipeEverything();

    // Disarmed only on a wipe that reported EVERY component erased. A
    // partial wipe keeps the marker, so the next launch tries again rather
    // than coming up clean-looking on top of whatever survived.
    if (outcome.result.complete())
        outcome.tripwireDisarmed = m_tripwire.disarm();
    else
        outcome.tripwireDisarmed = false;

    return outcome;
}

TrackedWipe::RecoveryOutcome TrackedWipe::recoverIfInterrupted()
{
    RecoveryOutcome outcome;
    if (!m_tripwire.isArmed())
        return outcome; // nothing was ever left half-done

    outcome.wasInterrupted = true;

    // Deliberately NOT re-armed first: the marker is already there, which is
    // the state arm() would produce anyway, and a failed re-arm would then
    // be indistinguishable from success.
    const LocalDataWipeResult result = m_wipe.wipeEverything();
    outcome.nowErased = result.complete();
    if (outcome.nowErased)
        m_tripwire.disarm();

    return outcome;
}
