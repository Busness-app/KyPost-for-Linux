import QtQuick 2.15
import QtTest 1.15
import com.kysecurity.mail 1.0
import "qrc:/qml/components" as Components

// The warning strips must never become the reason a user cannot act.
//
// StatusBanner is an anchored overlay (z 950) that reserves no space in its
// host's layout, so every pixel it occupies is painted OVER the app. In
// DesktopRoot that host is a ColumnLayout whose first child is the 66px top
// bar -- the one carrying the Settings button. Two active conditions were
// enough to swallow it whole on a real window, which left the pairing strip
// telling the user to "pair this device again in Settings" while covering
// the only route to Settings. Seven conditions can be active at once; the
// component had no upper bound at all.
//
// So this file pins two properties that together keep the app reachable no
// matter how many warnings fire: the expanded stack is capped, and the user
// can always collapse it to a single line. Neither may cost the user the
// information -- a collapsed banner still says how many warnings are live,
// and nothing here can be dismissed outright.
TestCase {
    id: testCase
    name: "StatusBannerCollapse"
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
        AppLock.databaseUnencrypted = false
        AppLock.databaseMemoryOnly = false
        AppLock.dataDirectoryUnprotected = false
        Pairing.certificateMismatch = false
        Pairing.reregistrationRejected = false
    }

    // Every condition the component can carry, on at once.
    function activateEverything() {
        Pairing.certificateMismatch = true
        Pairing.reregistrationRejected = true
        AppLock.wipeIncomplete = true
        AppLock.databaseMemoryOnly = true
        AppLock.dataDirectoryUnprotected = true
        AppLock.databaseUnencrypted = true
        AppLock.credentialsUnavailable = true
    }

    // The stack's height comes from a ColumnLayout, and Qt Quick Layouts
    // resolve during a polish pass rather than synchronously. A single
    // wait(0) lands before that pass often enough to fail about half the
    // time -- measured, not guessed: the banner reads 33 (its summary line
    // alone, rows still 0-high) instead of its settled height. Every
    // assertion about a measured height therefore waits for the value to
    // settle rather than for a fixed number of event-loop turns.
    function settledHeight(banner) {
        tryVerify(() => banner.height > collapsedCeiling, 3000,
                  "the stack never finished laying out")
        return banner.height
    }

    // A collapsed banner is one line; anything taller is still a stack.
    readonly property int collapsedCeiling: 48

    function visibleRowCount(banner) {
        let n = 0
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.objectName === "statusBannerRow" && child.visible)
                    ++n
                walk(child)
            }
        }
        walk(banner)
        return n
    }

    function test_anOrdinaryLaunchStillShowsNothing() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null, "banner must instantiate")
        wait(0)
        compare(banner.visible, false, "no condition active must mean no strip at all")
        compare(banner.height, 0, "and no height reserved either")
    }

    // The bound that stops seven strips eating an entire window.
    function test_theExpandedStackIsCapped() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null)

        activateEverything()
        settledHeight(banner)

        verify(banner.visible, "seven live warnings must be on screen")
        verify(banner.maxExpandedHeight > 0, "the component must publish its own cap")
        verify(banner.maxExpandedHeight < testCase.height,
               "a cap as tall as the window is not a cap")
        verify(banner.height <= banner.maxExpandedHeight + 1,
               "the stack grew past its cap: " + banner.height
               + " > " + banner.maxExpandedHeight)
    }

    // The escape hatch: whatever is underneath must be reachable in one click.
    function test_collapsingClearsTheUiBeneath() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null)

        Pairing.certificateMismatch = true
        Pairing.reregistrationRejected = true
        const expandedHeight = settledHeight(banner)

        banner.collapsed = true
        tryVerify(() => banner.height <= testCase.collapsedCeiling, 3000,
                  "collapsing must actually give the space back")

        verify(banner.height < expandedHeight,
               "collapsed (" + banner.height + ") must be shorter than expanded ("
               + expandedHeight + ")")
        verify(banner.visible,
               "collapsed is not dismissed -- the user must still see something is wrong")
        compare(visibleRowCount(banner), 0, "no message strips while collapsed")
    }

    // Collapsing may free space; it may not cost information.
    function test_collapsedStillSaysHowManyWarnings() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null)

        Pairing.certificateMismatch = true
        Pairing.reregistrationRejected = true
        tryCompare(banner, "activeCount", 2, 3000, "both live conditions must be counted")

        banner.collapsed = true
        wait(0)

        verify(banner.summaryText.length > 0, "a collapsed banner must still say something")
        verify(banner.summaryText.indexOf("2") !== -1,
               "the collapsed line must carry the count, got: " + banner.summaryText)
    }

    function test_expandingBringsTheWarningsBack() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null)

        Pairing.certificateMismatch = true
        const expandedHeight = settledHeight(banner)

        banner.collapsed = true
        tryVerify(() => banner.height <= testCase.collapsedCeiling, 3000)
        banner.collapsed = false
        tryVerify(() => banner.height === expandedHeight, 3000,
                  "expanding must restore the stack exactly")
        compare(visibleRowCount(banner), 1, "and the message itself must be back")
    }

    // A security warning that can be turned off permanently is not a warning.
    function test_theBannerCannotBeDismissedOutright() {
        const banner = createTemporaryObject(bannerComponent, testCase)
        verify(banner !== null)

        Pairing.certificateMismatch = true
        banner.collapsed = true
        tryVerify(() => banner.visible, 3000)

        verify(banner.visible, "collapsed still shows the summary line")
        verify(banner.height > 0, "and still occupies its one line")
    }
}
