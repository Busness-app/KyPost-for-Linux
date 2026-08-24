#pragma once

// Restarts this process.
//
// Needed because Hostile Location Protection decides at startup whether the
// database opens ":memory:" or a real file, and main.cpp's composition root
// is one unbroken chain of stack locals from Database down through every
// DAO, repository, controller and QML singleton registration. There is no
// supported way to re-point that graph at a different database at runtime --
// C++ references cannot be reseated and qmlRegisterSingletonInstance binds a
// fixed pointer for the engine's lifetime. A relaunch is the honest
// mechanism, not a workaround.
//
// Also used by the app lock's wipe-after-10-failures path, so one mechanism
// serves both.
namespace AppRelauncher {

// Requests a relaunch after the event loop exits.
//
// Deliberately does NOT spawn the child immediately. KDBusService(Unique)
// holds the com.kysecurity.mail well-known name for this process's lifetime; a
// child started while the parent still owns it would either fail to claim it
// or -- worse -- be treated as a duplicate launch, activate the dying parent
// and exit. Sequencing the spawn after app.exec() returns removes that race
// by construction rather than papering over it with a sleep.
//
// Calling this sets a flag and quits the event loop; main() then calls
// performPendingRelaunch() as its last act.
void requestRelaunch();

bool relaunchPending();

// Spawns the detached replacement process, preserving argv[1..] EXCEPT any
// kypost:// deep link -- see the .cpp for why a live pairing token must not
// be replayed unattended into a freshly-wiped process. Call only after the
// event loop has returned and the D-Bus name has been released.
//
// Returns false when the spawn failed (and logs), true otherwise including
// the no-relaunch-requested case. main() reports a non-zero exit on false:
// silently vanishing right after wiping the user's data is the one outcome
// worse than the wipe itself.
bool performPendingRelaunch();

} // namespace AppRelauncher
