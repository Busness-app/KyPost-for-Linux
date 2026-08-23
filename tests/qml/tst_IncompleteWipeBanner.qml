import QtQuick 2.15
import QtTest 1.15
import com.urlxl.mail 1.0
import "qrc:/qml/components" as Components

// The one thing on screen that stops a user believing their data is gone
// when it is not.
//
// A wipe that failed part-way, or one killed mid-run by a power loss, used
// to leave nothing but a qCritical line in the journal -- the next launch
// looked entirely ordinary on top of whatever survived. The C++ side now
// detects it (core/domain/TrackedWipe) and the banner is how the person
// holding the machine finds out.
//
// Tested in QML because the condition is a binding, and a binding that
// silently never becomes true is indistinguishable from a fixed bug.
TestCase {
    id: testCase
    name: "IncompleteWipeBanner"
    when: windowShown
    visible: true
    width: 800
    height: 400

    Component {
        id: bannerComponent
        Components.StatusBanner {}
    }

    function cleanup() {
        AppLock.wipeIncomplete = false
        AppLock.credentialsUnavailable = false
        Pairing.certificateMismatch = false
        Pairing.reregistrationRejected = false
    }

    // The messages actually on screen, in model order.
    function visibleMessages(banner) {
        let found = []
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.objectName === "statusBannerRow" && child.visible)
                    found.push(child.modelData.message)
                walk(child)
            }
        }
        walk(banner)
        return found
    }

    function test_nothingIsShownWhenNoWipeWasInterrupted() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null, "banner must instantiate")
        compare(banner.visible, false, "an ordinary launch must show no warning strip at all")
    }

    function test_theIncompleteWipeIsAnnounced() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null, "banner must instantiate")

        AppLock.wipeIncomplete = true
        // The strip heights come from a ColumnLayout, so the root's
        // `visible: height > 0` only becomes true after a layout pass.
        wait(0)

        verify(banner.visible, "an incomplete wipe must be visible on screen")
        const messages = visibleMessages(banner)
        compare(messages.length, 1, "exactly one strip should be showing")
        // The specific claim that matters: it must say the data may STILL BE
        // HERE. A generic "something went wrong" would leave the user with
        // the same false belief the banner exists to correct.
        verify(messages[0].indexOf("may still be") >= 0,
               "the warning must say the data may still be stored on this device")
    }

    // The distinction matters for describing what was broken: a condition
    // that is already true when the component is built took a different path
    // through the old circular binding than one that turns true afterwards.
    function test_aConditionAlreadyTrueAtCreationIsShown() {
        AppLock.wipeIncomplete = true
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null, "banner must instantiate")
        wait(0)
        compare(visibleMessages(banner).length, 1)
    }

    function test_itIsIndependentOfEveryOtherCondition() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null, "banner must instantiate")

        // A certificate problem must not mask it, and clearing that must not
        // clear this: they are separate facts about the device.
        Pairing.certificateMismatch = true
        AppLock.wipeIncomplete = true
        wait(0)
        compare(visibleMessages(banner).length, 2, "both conditions should be reported")

        Pairing.certificateMismatch = false
        wait(0)
        const remaining = visibleMessages(banner)
        compare(remaining.length, 1, "the wipe warning must survive an unrelated condition clearing")
        verify(remaining[0].indexOf("may still be") >= 0)
    }
}
