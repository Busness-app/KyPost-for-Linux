#pragma once

class QQmlApplicationEngine;

// Bring the app's main window to the front.
//
// This is what "the user launched KyPost while it was already running" has to
// mean. KDBusService(Unique) does not start a second app: the new process
// relays its argv to the running instance over D-Bus and exits immediately.
// If the running instance does nothing in response, the launcher silently
// does nothing -- which is exactly how this shipped, and it is worse than it
// sounds, because a closed window leaves the process alive with no way back
// to it (there is no tray icon on a desktop with no StatusNotifier host).
//
// Safe to call with a null engine, or before any QML has loaded:
// activateRequested can fire in the window between KDBusService's
// construction and the engine's, and a crash there would be a far worse bug
// than the one this fixes.
void activateMainWindow(QQmlApplicationEngine* engine);
