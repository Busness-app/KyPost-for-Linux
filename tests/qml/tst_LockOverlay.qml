import QtQuick 2.15
import QtTest 1.15
import com.urlxl.mail 1.0
import "qrc:/qml/components" as Components

// Regression coverage for the app-lock bypass.
//
// DesktopRoot creates genuine top-level Windows for popped-out email,
// compose and contact views (createObject(null, ...)). The unlock overlay
// was written out once, by hand, anchored to the main window's own root
// item -- so it never covered those windows. Popping an email out and then
// letting the app lock left the message on screen, readable and fully
// interactive, next to a dutifully-covered main window. That defeated
// "Require Unlock to Open" with a single click.
//
// The fix was to make the overlay a component every Window instantiates.
// These tests hold that line: the component must gate purely on
// AppLock.locked, and a separate Window must get its own working instance.
TestCase {
    id: testCase
    name: "LockOverlay"
    when: windowShown
    width: 200
    height: 200

    Component {
        id: overlayComponent
        Components.LockOverlay {}
    }

    // A stand-in for a pop-out: a real top-level Window with no QML parent,
    // exactly how DesktopRoot creates them.
    Component {
        id: popOutWindowComponent
        Window {
            width: 200
            height: 200
            property alias overlay: overlayInWindow
            Components.LockOverlay { id: overlayInWindow }
        }
    }

    function init() {
        AppLock.locked = false
    }

    function test_inactive_while_unlocked() {
        const overlay = createTemporaryObject(overlayComponent, testCase)
        verify(overlay !== null)
        verify(!overlay.active, "overlay must not exist while the app is unlocked")
        verify(!overlay.visible)
    }

    function test_activates_when_locked() {
        const overlay = createTemporaryObject(overlayComponent, testCase)
        verify(overlay !== null)

        AppLock.locked = true
        verify(overlay.active, "overlay must appear the moment the app locks")
        verify(overlay.item !== null, "the PIN screen itself must be loaded, not just the Loader")
        // Item.visible reads as effective visibility, so it depends on this
        // TestCase's own ancestry -- the meaningful check lives in
        // test_separate_window_gets_its_own_overlay below, against a real
        // shown Window.

        AppLock.locked = false
        verify(!overlay.active, "overlay must go away again on unlock")
        verify(overlay.item === null, "the PIN screen must be torn down, not merely hidden")
    }

    // The actual bypass: a separate top-level Window is not a child of the
    // main window, so an overlay anchored there cannot cover it. Each Window
    // needs its own.
    function test_separate_window_gets_its_own_overlay() {
        const win = createTemporaryObject(popOutWindowComponent, testCase)
        verify(win !== null)
        win.show()
        verify(waitForRendering(win.contentItem))

        verify(!win.overlay.active)

        AppLock.locked = true
        verify(win.overlay.active,
               "a popped-out window must be covered too -- this is the regression")
        verify(win.overlay.visible)
        verify(win.overlay.item !== null)

        win.close()
    }

    // It must sit above everything else in its window, including the Toast
    // (z 900) -- a notification rendering on top of the PIN screen would
    // leak exactly what the screen exists to hide.
    function test_paints_above_toasts() {
        const overlay = createTemporaryObject(overlayComponent, testCase)
        verify(overlay !== null)
        verify(overlay.z > 900, "overlay z must beat the Toast's 900")
    }
}
