import QtQuick 2.15
import com.kysecurity.mail 1.0

// The "an update exists" toast, plus the timing rule review found missing:
// UpdateCheckController.updateBecameAvailable() is a one-shot false->true
// signal, fired once per app run, at whatever moment the startup check
// lands. When the app lock is enabled that moment is very often WHILE the
// PIN screen (LockOverlay, z: 1000) is covering the window -- this toast
// sits at z: 940, under it, and Toast.show() self-hides after 2.5s. The
// toast played out and vanished entirely behind the overlay, and a user who
// locks on every launch was never told at all: "told every launch" silently
// became "never told", the exact failure the feature exists to prevent.
//
// Extends Toast rather than wrapping it, so a root drops this in exactly
// where the plain Toast used to sit (same anchors/z), with no extra
// Connections block of its own.
//
// Pulled out of DesktopRoot/MobileRoot into its own leaf component, not
// because either root couldn't hold the extra Connections block, but
// because neither root is reachable by the QML test harness (see
// tests/CMakeLists.txt: QmlTests cannot load DesktopRoot or MobileRoot --
// they need every real C++ singleton) while a leaf component under
// app/qml/components/ is, via the same FakeAppLock/FakeUpdateCheck
// singletons tst_LockOverlay.qml and tst_UpdateNotice.qml already use.
Toast {
    id: root

    // True once an update is known but has not been shown yet -- i.e. it
    // arrived while locked and is waiting for the next unlock.
    property bool pending: false
    // Latched the first time the toast is shown, by whichever path shows
    // it. Never cleared: the requirement is "at most once per app run", not
    // once per lock/unlock cycle, so a second, third, fourth unlock in the
    // same session must stay silent even though pending would otherwise be
    // false and eligible again.
    property bool shown: false

    function present() {
        shown = true
        show(i18n("KyPost %1 is available.", UpdateCheck.latestVersion))
    }

    Connections {
        target: UpdateCheck
        function onUpdateBecameAvailable() {
            if (root.shown)
                return
            if (AppLock.locked)
                root.pending = true
            else
                root.present()
        }
    }

    Connections {
        target: AppLock
        function onLockedChanged() {
            if (AppLock.locked || !root.pending || root.shown)
                return
            root.pending = false
            root.present()
        }
    }
}
