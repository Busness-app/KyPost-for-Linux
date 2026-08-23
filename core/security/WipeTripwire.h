#pragma once

#include <QString>

// Records that a wipe was STARTED, so one that never finished is
// discoverable on the next launch.
//
// The wipe-after-ten-failed-PIN-attempts path names every individual failure
// in the journal and then relaunches regardless, which is the right call --
// leaving a window open on the pre-wipe view of the world in front of
// whoever just failed ten attempts would be worse. But the journal is the
// ONLY record. After the relaunch the app comes up looking ordinary, on top
// of whatever survived, and nobody is told.
//
// Worse, the journal cannot record the case it most needs to: a wipe
// interrupted part-way -- power loss, an OOM kill, a laptop lid closed on a
// dying battery -- writes nothing at all, because the code that would have
// written it never ran. The device is then left holding data the user
// believes was erased, with no trace that an erase was ever attempted.
//
// So the marker goes down BEFORE the first byte is erased and is removed
// only after a wipe that reported itself complete. Anything else -- a failed
// wipe, a crashed one, a killed one -- leaves it behind, and finding it at
// startup means "the last wipe did not finish; do it again".
//
// A plain file rather than a settings key or a database row: it has to
// survive the very wipe it is tracking, and both of those are things the
// wipe erases.
class WipeTripwire
{
public:
    explicit WipeTripwire(const QString& markerPath);

    // Call before erasing anything. False means the marker could not be
    // written, so an interruption from here on will NOT be detectable --
    // the caller should say so rather than assume it is covered. It must
    // still go on to wipe: erasing what it can beats erasing nothing
    // because the bookkeeping failed.
    bool arm();

    // Call only after a wipe that reported itself complete. False means the
    // marker is still there, which costs a redundant wipe on the next launch
    // -- harmless, and the safe direction to fail in.
    bool disarm();

    // True when a wipe was started and never reported completing.
    bool isArmed() const;

private:
    QString m_markerPath;
};
