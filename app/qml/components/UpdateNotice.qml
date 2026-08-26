import QtQuick 2.15
import QtQuick.Layouts 1.15
import com.kysecurity.mail 1.0

// The About section's content: installed version, plus one of three
// states -- an update exists, we are current, or we do not know -- a "last
// checked" freshness line, and the cannot-self-update hint. Pulled out of
// Settings.qml into its own component so it can be instantiated and driven
// directly in tests (tst_UpdateNotice.qml) via the writable FakeUpdateCheck
// singleton, without dragging in the whole Settings page.
//
// Pure presentation: reads the UpdateCheck singleton directly rather than
// duplicating its state as properties here.
ColumnLayout {
    id: root

    spacing: 6

    Text {
        objectName: "updateNoticeInstalledVersion"
        textFormat: Text.PlainText
        text: i18n("KyPost %1", UpdateCheck.installedVersion)
        color: Theme.inkStrong
        font.family: Theme.fontUi
        font.pixelSize: 14
    }

    // Three distinct states, deliberately worded apart: an update exists, we
    // are current, or we do not know. "We do not know" is the state of every
    // client paired to a server too old to answer, and it must not read as a
    // failure.
    //
    // objectName lets the test find this element deterministically -- same
    // reasoning as StatusBanner.qml's "statusBannerRow": which of these
    // three wordings is showing IS the behaviour under test, so walking
    // anonymous children to work it out would be brittle.
    Text {
        objectName: "updateNoticeStatus"
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: UpdateCheck.updateAvailable ? Theme.accent : Theme.ink
        font.family: Theme.fontUi
        font.pixelSize: 12
        text: UpdateCheck.updateAvailable
              ? i18n("Version %1 is available.", UpdateCheck.latestVersion)
              : UpdateCheck.latestVersion.length > 0
                ? i18n("This is the newest released version.")
                : i18n("No update information available from this server.")
    }

    // When the check last succeeded. Without it, "This is the newest
    // released version" is indistinguishable from a check that has been
    // silently failing for a month -- failures are dropped rather than
    // surfaced, so this timestamp is the only evidence the user has that
    // the answer is fresh.
    Text {
        objectName: "updateNoticeLastChecked"
        textFormat: Text.PlainText
        visible: UpdateCheck.checkedAt.length > 0
        text: i18n("Last checked %1", UpdateCheck.checkedAt)
        color: Theme.ink
        font.family: Theme.fontUi
        font.pixelSize: 11
    }

    GhostButton {
        objectName: "updateNoticeReleasesButton"
        visible: UpdateCheck.updateAvailable
        text: i18n("Open the releases page…")
        onClicked: Qt.openUrlExternally(UpdateCheck.releaseUrl)
    }

    MutedHint {
        objectName: "updateNoticeSelfUpdateHint"
        Layout.fillWidth: true
        visible: UpdateCheck.updateAvailable
        text: i18n("KyPost cannot update itself. Download the new version from the "
                   + "releases page, or update through your package manager.")
    }
}
