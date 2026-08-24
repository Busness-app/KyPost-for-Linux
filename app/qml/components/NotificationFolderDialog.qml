import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.kysecurity.mail 1.0

// Asks which copy of a message the user meant, when a notification tap-through
// cannot tell.
//
// The UnifiedPush envelope carries no mailbox (PushPayloadParser.h), and a
// message id stopped being globally unique in migration 006 -- the relay serves
// the same id from every mailbox holding it. So an id cached in both INBOX and
// Archive has no tie-breaker, and MailRepository::findCachedEmail refuses to
// pick rather than open the wrong copy.
//
// Until 2026-08-23 that refusal rendered as a blank detail page: the user
// tapped View and got nothing, with no way to act on a question only they can
// answer. This is the way to act.
//
// Same overlay-Item shape as PickupFallbackDialog.qml / HyperlinkDialog.qml --
// there is no QtQuick.Controls.Dialog precedent in this codebase.
Item {
    id: root

    property bool isOpen: false
    property string messageId: ""
    // The mailboxes holding this id, as MailController delivered them. Already
    // sorted by the query, so the entries do not move between openings.
    property var folders: []

    signal chosen(string messageId, string folder)

    function open(id, folderList) {
        root.messageId = id
        root.folders = folderList
        root.isOpen = true
        // Escape below only works with active focus, and this overlay is
        // created rather than pushed onto a focus-managing stack.
        panel.forceActiveFocus()
    }

    function close() {
        root.isOpen = false
        root.messageId = ""
        root.folders = []
    }

    // Dismissing opens nothing. There is no safe default here: every option is
    // a real message and picking one for the user is exactly the wrong-message
    // bug this dialog exists to avoid.
    function cancel() {
        root.close()
    }

    function choose(folder) {
        const id = root.messageId
        root.close()
        root.chosen(id, folder)
    }

    visible: root.isOpen
    anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.4)

        TapHandler {
            onTapped: root.cancel()
        }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: 360
        implicitHeight: content.implicitHeight + 32
        radius: Theme.shapeSheet
        color: Theme.panel
        border.width: 1
        border.color: Theme.line

        Keys.onEscapePressed: function (event) {
            root.cancel()
            event.accepted = true
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: function (wheel) { wheel.accepted = true }
        }

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Text {
                objectName: "folderDialogTitle"
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: i18n("Which copy do you want?")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 16
                font.bold: true
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                // Says why rather than just asking. A notification that cannot
                // say which mailbox it meant is not something the user did
                // wrong, and "we can't tell" is the honest reason.
                text: i18n("This message is in more than one mailbox, and the notification didn't say which one it came from.")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: root.folders

                PrimaryButton {
                    objectName: "folderChoice"
                    Layout.fillWidth: true
                    // Bound as PlainText wherever it is rendered: this is a
                    // mailbox name off the wire, not a string this app chose.
                    text: modelData
                    onClicked: root.choose(modelData)
                }
            }

            GhostButton {
                Layout.alignment: Qt.AlignRight
                text: i18n("Cancel")
                onClicked: root.cancel()
            }
        }
    }
}
