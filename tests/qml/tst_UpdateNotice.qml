import QtQuick 2.15
import QtTest 1.15
import com.kysecurity.mail 1.0
import "qrc:/qml/components" as Components

TestCase {
    id: testCase
    name: "UpdateNotice"
    when: windowShown
    visible: true
    width: 400
    height: 300

    // ---- UpdateNotice: the About section's three-state content ----------
    //
    // FakeUpdateCheck's properties are writable from QML, so each test
    // drives the UpdateCheck singleton directly and asserts against the
    // rendered Text elements -- found by objectName rather than
    // positionally, same reasoning as StatusBanner.qml's "statusBannerRow":
    // which wording is on screen IS the behaviour under test, and walking
    // anonymous children to work it out was too brittle there too.

    Component {
        id: noticeComponent
        Components.UpdateNotice {}
    }

    function findChild(item, objectName) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (child.objectName === objectName)
                return child
            const found = findChild(child, objectName)
            if (found !== null)
                return found
        }
        return null
    }

    // Item.visible reports EFFECTIVE (ancestor-cascaded) visibility, not
    // just the item's own binding -- so the TestCase root itself must be
    // `visible: true` (see above; StatusBannerCollapse.qml's TestCase sets
    // the same) or every descendant reads visible=false regardless of its
    // own binding, which would make the "hidden" assertions below pass for
    // the wrong reason. wait(0) after creation is the same belt-and-braces
    // margin tst_StatusBannerCollapse.qml gives its own first visible check.
    function createNotice() {
        const notice = createTemporaryObject(noticeComponent, testCase)
        verify(notice !== null)
        wait(0)
        return notice
    }

    function init() {
        UpdateCheck.updateAvailable = false
        UpdateCheck.latestVersion = ""
        UpdateCheck.checkedAt = ""
    }

    function cleanup() {
        UpdateCheck.updateAvailable = false
        UpdateCheck.latestVersion = ""
        UpdateCheck.checkedAt = ""
    }

    // State 1: an update exists.
    function test_updateAvailable_showsVersionAndOffersTheReleasesPage() {
        UpdateCheck.updateAvailable = true
        UpdateCheck.latestVersion = "1.4.0"

        const notice = createNotice()

        const status = findChild(notice, "updateNoticeStatus")
        verify(status !== null)
        verify(status.text.indexOf("1.4.0") >= 0,
               "status text must carry the new version, got: " + status.text)
        verify(status.text.indexOf("is available") >= 0,
               "must use the update-available wording, got: " + status.text)

        const button = findChild(notice, "updateNoticeReleasesButton")
        verify(button !== null)
        compare(button.visible, true, "the releases-page button must be offered")

        const hint = findChild(notice, "updateNoticeSelfUpdateHint")
        verify(hint !== null)
        compare(hint.visible, true, "the cannot-self-update hint must be offered")
    }

    // State 2: we are current. Must not be confused with state 1 (no "is
    // available" wording, no button, no hint).
    function test_current_saysNewestReleasedVersion() {
        UpdateCheck.updateAvailable = false
        UpdateCheck.latestVersion = "1.4.0"

        const notice = createNotice()

        const status = findChild(notice, "updateNoticeStatus")
        verify(status !== null)
        verify(status.text.indexOf("newest released version") >= 0,
               "must read as current, got: " + status.text)
        verify(status.text.indexOf("is available") === -1,
               "must not be confused with the update-available wording, got: " + status.text)

        const button = findChild(notice, "updateNoticeReleasesButton")
        compare(button.visible, false, "no button when there is nothing to install")

        const hint = findChild(notice, "updateNoticeSelfUpdateHint")
        compare(hint.visible, false, "no self-update hint when there is nothing to install")
    }

    // State 3: no information at all -- the state of every client paired to
    // a server too old to answer. Must read as "unknown", never as an
    // error, and must not collapse into either neighbouring state:
    // conflating "unknown" with "current" is the exact bug worth guarding.
    function test_noServerAnswer_readsAsUnknownNotAsCurrentOrError() {
        UpdateCheck.updateAvailable = false
        UpdateCheck.latestVersion = ""

        const notice = createNotice()

        const status = findChild(notice, "updateNoticeStatus")
        verify(status !== null)
        verify(status.text.indexOf("No update information") >= 0,
               "must read as unknown, got: " + status.text)
        verify(status.text.indexOf("newest released version") === -1,
               "must not be confused with 'you are current', got: " + status.text)
        verify(status.text.indexOf("is available") === -1,
               "must not be confused with 'an update exists', got: " + status.text)
        verify(status.text.toLowerCase().indexOf("error") === -1,
               "not knowing must never read as a failure, got: " + status.text)
        verify(status.text.toLowerCase().indexOf("fail") === -1,
               "not knowing must never read as a failure, got: " + status.text)

        const button = findChild(notice, "updateNoticeReleasesButton")
        compare(button.visible, false, "no button when there is nothing to install")

        const hint = findChild(notice, "updateNoticeSelfUpdateHint")
        compare(hint.visible, false, "no self-update hint when there is nothing to install")
    }

    // The "last checked" line is the only evidence a stale, silently-failed
    // check isn't masquerading as "you are current".
    function test_lastChecked_hiddenWhenEmptyVisibleWithValue() {
        UpdateCheck.checkedAt = ""

        const notice = createNotice()

        const line = findChild(notice, "updateNoticeLastChecked")
        verify(line !== null)
        compare(line.visible, false, "no timestamp, no line")

        UpdateCheck.checkedAt = "2026-08-25T10:00:00Z"
        wait(0)
        compare(line.visible, true, "a real timestamp must show the line")
        verify(line.text.indexOf("2026-08-25T10:00:00Z") >= 0,
               "the line must carry the value, got: " + line.text)
    }
}
