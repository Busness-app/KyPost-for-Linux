import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.kysecurity.mail 1.0

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
//
// BOUNDED AND COLLAPSIBLE, because anchoring means every pixel here is
// painted OVER the app rather than beside it. Seven of the rows below can be
// active at once and the stack used to grow without any limit: two were
// already enough to swallow DesktopRoot's 66px top bar, which carries the
// Settings button -- so the pairing strip sat there telling the user to
// "pair this device again in Settings" while covering the only way to reach
// Settings. A warning that blocks its own remedy is worse than no warning.
//
// Two properties keep the app reachable no matter how many fire:
//   - the expanded stack is capped at maxExpandedHeight and scrolls past it,
//     so it can never take more than its share of the window;
//   - the summary line collapses it to a single row on demand.
// Collapsed is NOT dismissed: the summary line stays, and it keeps saying
// how many warnings are live. Nothing here can be turned off outright --
// these are the conditions the user most needs to know about.
Item {
    id: root

    // Raised when the user asks to review a certificate change. The root
    // hosts CertificateChangeDialog and connects this: the dialog is
    // full-window and this banner is only as tall as its own text, so it
    // cannot host one itself.
    signal certificateReviewRequested()

    readonly property var rows: [
        {
            // The relay's TLS certificate no longer matches the one
            // pinned when this device paired, so every request is
            // aborted before it is sent (core/net/HttpClient.cpp's
            // trust-on-first-use check). Listed first because it
            // stops literally everything, and because without it the
            // app looks simply broken: the per-request error is a
            // generic "Refresh failed" and there is no other clue.
            // Re-pairing is NO LONGER the only recovery: the action
            // below re-anchors the pin without deregistering the
            // device or costing the user a fresh pairing link. The
            // decision itself still belongs in a dialog that names
            // both fingerprints -- see CertificateChangeDialog.qml.
            active: Pairing.certificateMismatch,
            message: i18n("KyPost stopped trusting this server: a different authority is "
                           + "vouching for its certificate than when this device was "
                           + "paired, so no requests are being sent. Review the change "
                           + "before deciding — if you did not expect it, do not "
                           + "reconnect until you know why."),
            actionLabel: i18n("Review certificate change…")
        },
        {
            // The relay verifies the pairing token before anything
            // else on the registration route and 401s without a valid
            // one (server_notifications.go), and this client has no
            // way to mint one. So unlike the certificate case there
            // is genuinely no in-app recovery -- say what stopped,
            // and what the user has to go and do, rather than leaving
            // them to infer it from "pair again".
            active: Pairing.reregistrationRejected,
            message: i18n("This device's pairing token is no longer accepted by the server, "
                           + "so new-mail notifications have stopped. Cached mail is "
                           + "unaffected and stays readable. To restore notifications you "
                           + "need a new pairing link: sign in to KyPost on the web, open "
                           + "the pairing screen there, then pair this device again in "
                           + "Settings."),
            actionLabel: ""
        },
        {
            // A wipe was started on this device and never reported
            // completing -- it failed part-way, or the process died
            // in the middle of it and the retry at startup could not
            // finish it either. Listed above the credential row
            // because it is the only condition here where the user
            // has been told something is gone that is not.
            //
            // No action button: there is nothing in this app that
            // can be clicked to fix a keyring that will not release
            // a credential or a disk that will not take a write.
            // Saying so plainly beats a button that fails again.
            actionLabel: "",
            active: AppLock.wipeIncomplete,
            message: i18n("KyPost tried to erase this device's data and could not finish. "
                           + "Some cached mail, contacts or sign-in credentials may still be "
                           + "stored here even though they were reported as erased. Check "
                           + "that your system keyring is running and that this device has "
                           + "free disk space, then restart KyPost to try again.")
        },
        {
            // Nothing this session caches is being written to this device at
            // all, because there is nowhere to keep the key that would
            // protect it. Said plainly, because the consequence is one the
            // user will otherwise discover by losing their cached mail on
            // the next restart.
            actionLabel: "",
            active: AppLock.databaseMemoryOnly,
            message: i18n("KyPost cannot reach a keyring, so it has nowhere to store the key that "
                           + "encrypts your mail on this computer. Nothing is being saved to this "
                           + "device: your mail is loaded fresh each time and disappears when KyPost "
                           + "closes. Start a keyring service (gnome-keyring or kwallet) and restart "
                           + "KyPost to keep an encrypted copy here.")
        },
        {
            // Memory-only like the row above, but the fix is a chmod rather
            // than a keyring -- so it gets its own sentence instead of
            // borrowing one that would send the user to the wrong place.
            actionLabel: "",
            active: AppLock.dataDirectoryUnprotected,
            message: i18n("The folder KyPost keeps your mail and contacts in can be read by other "
                           + "accounts on this computer, and KyPost could not lock it down. Nothing "
                           + "is being saved to this device this session. Make that folder private "
                           + "to you (chmod 700) and restart KyPost to keep a copy here.")
        },
        {
            // The database is on disk and readable by anyone with access to
            // this account's files. Not the same as the row above and not
            // interchangeable with it.
            actionLabel: "",
            active: AppLock.databaseUnencrypted,
            message: i18n("The mail and contacts KyPost has stored on this computer are not "
                           + "encrypted: anyone who can read your files can read them. KyPost will "
                           + "try again to encrypt them the next time it starts.")
        },
        {
            actionLabel: "",
            active: AppLock.credentialsUnavailable,
            message: i18n("KyPost could not decrypt this device's stored credentials, so "
                           + "the server will reject its requests. Check that your system "
                           + "keyring is unlocked, then lock and unlock KyPost again.")
        }
    ]

    // Hoisted so the summary line and the height below can both ask how many
    // warnings are live WITHOUT going through the laid-out height.
    readonly property int activeCount: root.rows.filter(row => row.active).length

    // Set by the user, never by a condition appearing: a new warning must not
    // silently expand a banner the user deliberately collapsed, and an
    // existing one must not be hidden by a condition clearing elsewhere.
    property bool collapsed: false

    // The share of the host window the expanded stack may take before it
    // starts scrolling instead of growing. The floor keeps it usable in a
    // short window (Plasma Mobile), where 40% of very little is nothing.
    readonly property real maxExpandedHeight: parent ? Math.max(96, parent.height * 0.4) : 0

    readonly property string summaryText:
        i18np("%1 warning", "%1 warnings", root.activeCount)

    anchors.top: parent ? parent.top : undefined
    anchors.left: parent ? parent.left : undefined
    anchors.right: parent ? parent.right : undefined
    z: 950

    // Asked of the model, NOT of the measured height, and that is the whole
    // point.
    //
    // This used to read `visible: height > 0`, with the delegate's own
    // height reading `visible ? ... : 0`. Item.visible reports EFFECTIVE
    // visibility, so a delegate inside an invisible root reports false no
    // matter what its own binding says -- which made it contribute 0 height,
    // which kept the root invisible, which kept the delegate reporting
    // false. A closed loop with a stable answer of "never show anything":
    // every warning this component exists to carry -- the certificate
    // mismatch, the rejected re-registration, the undecryptable credentials
    // -- was unreachable. Found by the first test that drove one of these
    // conditions from false to true and looked at the screen.
    visible: root.activeCount > 0

    height: !root.visible ? 0
            : root.collapsed ? summaryStrip.implicitHeight
            : Math.min(summaryStrip.implicitHeight + column.implicitHeight,
                       root.maxExpandedHeight)

    // Always present while anything is live -- it is what makes the collapse
    // reversible, and what keeps a collapsed banner honest about how much it
    // is hiding.
    Rectangle {
        id: summaryStrip
        objectName: "statusBannerSummary"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        implicitHeight: summaryRow.implicitHeight + 16
        color: Theme.panel
        border.width: 1
        border.color: Theme.dangerColor

        RowLayout {
            id: summaryRow
            anchors.fill: parent
            anchors.margins: 8
            spacing: 8

            Text {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: root.summaryText
                color: Theme.dangerColor
                font.family: Theme.fontUi
                font.pixelSize: 12
                font.weight: Font.Medium
                elide: Text.ElideRight
            }

            // Deliberately a plain text toggle rather than a GhostButton:
            // this strip is the height floor for the whole component, and a
            // GhostButton's padding alone is taller than the line it sits in.
            Text {
                objectName: "statusBannerToggle"
                text: root.collapsed ? i18n("Show") : i18n("Hide")
                color: Theme.dangerColor
                font.family: Theme.fontUi
                font.pixelSize: 12
                font.underline: toggleArea.containsMouse

                MouseArea {
                    id: toggleArea
                    anchors.fill: parent
                    anchors.margins: -6 // a 12px line is a small hit target
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.collapsed = !root.collapsed
                }
            }
        }
    }

    // Scrolls rather than clips: capping the height must not be able to hide
    // a warning outright, which is the failure this component exists to
    // prevent in the first place.
    Flickable {
        id: stack
        anchors.top: summaryStrip.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: !root.collapsed
        clip: true
        contentHeight: column.implicitHeight
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ThemedScrollBar {}

        ColumnLayout {
            id: column
            width: stack.width
            spacing: 0

            Repeater {
                model: root.rows

                delegate: Rectangle {
                    required property var modelData

                    // Named so a test can find the strips that are actually on
                    // screen. Which of these conditions is visible is the whole
                    // behaviour of this component, and walking anonymous
                    // children to work it out was brittle enough to assert the
                    // wrong thing.
                    objectName: "statusBannerRow"

                    Layout.fillWidth: true
                    visible: modelData.active
                    // modelData.active, not `visible`: see the root's `visible` above
                    // for what reading effective visibility here cost.
                    implicitHeight: modelData.active ? bannerLabel.implicitHeight + 16 : 0
                    color: Theme.panel
                    border.width: 1
                    border.color: Theme.dangerColor

                    RowLayout {
                        id: bannerLabel
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: modelData.message
                            color: Theme.dangerColor
                            font.family: Theme.fontUi
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                        }

                        GhostButton {
                            visible: modelData.actionLabel.length > 0
                            text: modelData.actionLabel
                            // Only the certificate row carries an action today.
                            onClicked: root.certificateReviewRequested()
                        }
                    }
                }
            }
        }
    }
}
