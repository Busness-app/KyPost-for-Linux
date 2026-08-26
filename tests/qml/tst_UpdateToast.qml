import QtQuick 2.15
import QtTest 1.15
import com.kysecurity.mail 1.0
import "qrc:/qml/components" as Components

// Regression coverage for the defect found in final review of the update
// notice: the toast sat at z: 940, under LockOverlay's z: 1000, and
// self-hid after 2.5s -- so a lock-enabled user whose startup update check
// landed while the PIN screen was up never saw it. "Told every launch"
// silently became "never told" for exactly the users the feature exists to
// protect, because the one-shot updateBecameAvailable() signal fired and
// played out entirely behind the overlay.
//
// UpdateToast.qml is the fix: defer the toast until unlock if locked at
// signal time, and never show it more than once per run. These tests drive
// it directly via FakeAppLock (writable `locked`) and FakeUpdateCheck
// (writable `updateAvailable`/`latestVersion`, and a real
// updateBecameAvailable signal) -- see tests/qml/FakeSingletons.h.
TestCase {
    id: testCase
    name: "UpdateToast"
    when: windowShown
    visible: true
    width: 400
    height: 300

    Component {
        id: toastComponent
        Components.UpdateToast {}
    }

    function init() {
        AppLock.locked = false
        UpdateCheck.updateAvailable = false
        UpdateCheck.latestVersion = ""
    }

    function cleanup() {
        AppLock.locked = false
        UpdateCheck.updateAvailable = false
        UpdateCheck.latestVersion = ""
    }

    function createToast() {
        const toast = createTemporaryObject(toastComponent, testCase)
        verify(toast !== null)
        wait(0)
        return toast
    }

    // Unlocked at signal time: today's behaviour, unchanged.
    function test_unlockedAtSignal_showsImmediately() {
        const toast = createToast()
        UpdateCheck.latestVersion = "1.5.0"

        UpdateCheck.updateBecameAvailable()

        compare(toast.visible, true, "must show immediately while unlocked")
        verify(toast.text.indexOf("1.5.0") >= 0,
               "toast text must carry the version, got: " + toast.text)
    }

    // The defect: locked at signal time must not waste the one-shot signal
    // behind the overlay. It must wait, then show on unlock.
    function test_lockedAtSignal_thenUnlock_showsToast() {
        AppLock.locked = true
        const toast = createToast()
        UpdateCheck.latestVersion = "1.5.0"

        UpdateCheck.updateBecameAvailable()
        compare(toast.visible, false, "must not show while still locked")

        AppLock.locked = false
        compare(toast.visible, true, "must show once the user unlocks")
        verify(toast.text.indexOf("1.5.0") >= 0,
               "toast text must carry the version, got: " + toast.text)
    }

    // At most once per run: a user who locks and unlocks repeatedly must
    // not be re-shown the same notice on every cycle.
    function test_secondUnlock_doesNotShowAgain() {
        AppLock.locked = true
        const toast = createToast()
        UpdateCheck.latestVersion = "1.5.0"
        UpdateCheck.updateBecameAvailable()

        AppLock.locked = false
        compare(toast.visible, true, "first unlock must show it")
        toast.visible = false // simulate the 2.5s auto-hide having already run

        AppLock.locked = true
        AppLock.locked = false
        compare(toast.visible, false, "a later unlock must stay silent")
    }

    // At most once per run, across a SECOND signal emission too, not just a
    // second lock/unlock cycle with no new signal. updateBecameAvailable()
    // is a transition signal and can genuinely fire more than once in one
    // run (e.g. a later poll goes unavailable -> available again), so the
    // once-per-run rule has to hold against a real second emission, not
    // just against `pending` being already spent.
    function test_secondEmission_afterAlreadyShown_doesNotShowAgain() {
        AppLock.locked = true
        const toast = createToast()
        UpdateCheck.latestVersion = "1.5.0"
        UpdateCheck.updateBecameAvailable()

        AppLock.locked = false
        compare(toast.visible, true, "first unlock must show it")
        toast.visible = false // simulate the 2.5s auto-hide having already run

        UpdateCheck.latestVersion = "1.6.0"
        UpdateCheck.updateBecameAvailable()
        compare(toast.visible, false, "a second emission this run must stay silent")
    }

    // No update pending: unlocking must show nothing at all.
    function test_unlockWithNoUpdatePending_showsNothing() {
        AppLock.locked = true
        const toast = createToast()

        AppLock.locked = false
        compare(toast.visible, false, "no update means nothing to show")
    }
}
