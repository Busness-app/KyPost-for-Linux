#pragma once

#include "domain/LocalDataWipe.h"

class WipeTripwire;

// LocalDataWipe plus the bookkeeping that makes an interrupted wipe
// survivable, in a class rather than in main().
//
// The sequence is the point, and it is short enough to get wrong in a
// lambda: arm the tripwire, wipe, and disarm ONLY if the wipe reported every
// component erased. main() is where the previous version of this lived, and
// main() has no seam a test can reach -- which is how the pre-rename
// database stayed invisible to both wipe handlers for months.
class TrackedWipe
{
public:
    TrackedWipe(WipeTripwire& tripwire, LocalDataWipe& wipe);

    struct Outcome
    {
        LocalDataWipeResult result;
        // False when the marker could not be written. The wipe still ran --
        // erasing what we can beats erasing nothing because the bookkeeping
        // failed -- but an interruption from that point would not have been
        // detectable, and the caller is expected to say so out loud rather
        // than let it pass as a clean run.
        bool tripwireArmed = true;
        // False when the wipe completed but the marker could not be removed.
        // Costs a redundant wipe next launch; harmless, and the safe
        // direction.
        bool tripwireDisarmed = true;

        // "Everything this device was asked to erase is gone." The tripwire
        // flags are deliberately NOT part of this: a leftover marker is a
        // bookkeeping problem, not surviving data.
        bool erased() const { return result.complete(); }
    };

    // The wipe-after-ten-failed-PIN-attempts path.
    Outcome wipeEverything();

    // Called at startup, before anything can put cached data on screen.
    //
    // A marker still on disk means the last wipe never reported completing:
    // it failed, or the process died in the middle of it. Either way the
    // right move is to do it again, and to keep the marker if it fails
    // again. Retrying is safe because every step of LocalDataWipe is
    // idempotent -- deleting rows that are already gone and removing files
    // that no longer exist both succeed.
    struct RecoveryOutcome
    {
        bool wasInterrupted = false;  // a marker was found
        bool nowErased = false;       // the retry (if any) reported complete
        // The retry's per-step detail, default-constructed when there was no
        // retry. Carried because this caller -- unlike the two that relaunch
        // -- goes on to run a full session, and it has to know how
        // LocalDataWipe reopened the database it just unlinked
        // (`databaseReopenedAs`) before anything writes to it.
        LocalDataWipeResult result;
    };
    RecoveryOutcome recoverIfInterrupted();

private:
    WipeTripwire& m_tripwire;
    LocalDataWipe& m_wipe;
};
