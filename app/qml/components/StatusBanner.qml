import QtQuick 2.15
import QtQuick.Layouts 1.15
import com.urlxl.mail 1.0

// Persistent warning strips for conditions the user has to act on and that
// no toast can carry, because they do not pass on their own.
//
// Both of the conditions below were previously invisible by construction:
// the re-registration 401 was discarded at its call site in main.cpp (a case
// AGENTS.md section 8 flags in bold), and a failed credential unseal simply
// made every request 401 with nothing on screen to distinguish it from the
// server being down. A user cannot act on a problem they are not told about.
//
// Anchored rather than laid out, so a root can drop one in without
// restructuring its own layout. z sits under the app-lock overlay (1000) --
// a locked app must not leak account state through a banner.
Item {
    id: root

    anchors.top: parent ? parent.top : undefined
    anchors.left: parent ? parent.left : undefined
    anchors.right: parent ? parent.right : undefined
    z: 950
    height: column.implicitHeight
    visible: height > 0

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 0

        Repeater {
            model: [
                {
                    // The relay's TLS certificate no longer matches the one
                    // pinned when this device paired, so every request is
                    // aborted before it is sent (core/net/HttpClient.cpp's
                    // trust-on-first-use check). Listed first because it
                    // stops literally everything, and because without it the
                    // app looks simply broken: the per-request error is a
                    // generic "Refresh failed" and there is no other clue.
                    // Re-pairing is the only recovery -- it is what captures
                    // a new pin.
                    active: Pairing.certificateMismatch,
                    message: i18n("KyPost stopped trusting this server: its security certificate "
                                   + "changed since this device was paired, so no requests are "
                                   + "being sent. If your server's certificate was renewed, remove "
                                   + "this pairing in Settings and pair again. If you did not "
                                   + "expect this, do not pair again until you know why it changed.")
                },
                {
                    active: Pairing.reregistrationRejected,
                    message: i18n("This device's pairing has expired, so new mail notifications "
                                   + "have stopped arriving. Open Settings and pair this device again.")
                },
                {
                    active: AppLock.credentialsUnavailable,
                    message: i18n("KyPost could not decrypt this device's stored credentials, so "
                                   + "the server will reject its requests. Check that your system "
                                   + "keyring is unlocked, then lock and unlock KyPost again.")
                }
            ]

            delegate: Rectangle {
                required property var modelData

                Layout.fillWidth: true
                visible: modelData.active
                implicitHeight: visible ? bannerLabel.implicitHeight + 16 : 0
                color: Theme.panel
                border.width: 1
                border.color: Theme.dangerColor

                Text {
                    id: bannerLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    textFormat: Text.PlainText
                    text: modelData.message
                    color: Theme.dangerColor
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
