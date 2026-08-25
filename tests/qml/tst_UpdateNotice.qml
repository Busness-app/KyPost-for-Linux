import QtQuick 2.15
import QtTest 1.15
import com.kysecurity.mail 1.0

TestCase {
    name: "UpdateNotice"
    when: windowShown

    // The version on screen must be the build's own, not a literal someone
    // has to remember to edit.
    function test_installedVersionIsPresent() {
        verify(UpdateCheck.installedVersion.length > 0)
    }

    // No update, no notice. A user who is current must not see an update row
    // at all -- a permanently-present "you might be behind" is noise.
    function test_noNoticeWhenCurrent() {
        compare(UpdateCheck.updateAvailable, false)
    }

    function test_releaseUrlPointsAtTheReleasesPage() {
        verify(UpdateCheck.releaseUrl.indexOf("KyPost-for-Linux/releases") >= 0)
    }
}
