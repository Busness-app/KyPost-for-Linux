import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.kysecurity.mail 1.0

// Compose autocomplete (ContactAutocomplete.md): a full address-book search
// panel with per-row To/Cc/Bcc buttons, launched from a directory icon next
// to Compose.qml's token fields. Plain overlay Item (scrim + centered
// panel), not QtQuick.Controls' Dialog -- no Controls.Dialog precedent
// exists anywhere in this codebase, and Compose.qml/ContactsList.qml's own
// "plain reusable Item" convention is the closer fit.
//
// Judgment call: the per-row "added" indicator is a live binding against
// whichever of toTokens/ccTokens/bccTokens the caller passes in (reverting
// automatically if a pill is later removed in Compose.qml), not a one-shot
// flash -- matches the source doc's "showing the contact **has been**
// added" (present-state) wording. Tapping an already-added row's button
// still forwards contactPicked() -- TokenField.addToken()'s own duplicate
// check (duplicateRejected -> Toast) is the single source of truth for
// "already there", so this dialog doesn't duplicate that logic.
Item {
    id: root

    property bool isOpen: false
    property string searchText: ""
    // Supplied by Compose.qml, bound live to each TokenField's own `tokens`.
    property var toTokens: []
    property var ccTokens: []
    property var bccTokens: []

    signal contactPicked(string email, string target)

    function open() {
        root.searchText = ""
        root.isOpen = true
    }

    function close() {
        root.isOpen = false
    }

    function containsEmail(tokenList, email) {
        const folded = email.toLowerCase()
        for (let i = 0; i < tokenList.length; i++) {
            if (tokenList[i].email.toLowerCase() === folded)
                return true
        }
        return false
    }

    visible: root.isOpen
    anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.4)

        TapHandler {
            onTapped: root.close()
        }
    }

    Rectangle {
        id: panel
        width: Math.min(420, root.width - 40)
        height: Math.min(520, root.height - 40)
        anchors.centerIn: parent
        radius: Theme.shapeSheet
        color: Theme.panel
        border.width: 1
        border.color: Theme.line

        // A pointer handler can share a gesture with items underneath. A
        // MouseArea is an exclusive input barrier for the modal panel's
        // otherwise-empty space; controls declared below remain clickable.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: function (wheel) { wheel.accepted = true }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true

                Text {
                    Layout.fillWidth: true
                    text: i18n("Address Book")
                    color: Theme.inkStrong
                    font.family: Theme.fontUi
                    font.pixelSize: 16
                    font.weight: Font.Bold
                }
                GhostButton {
                    text: i18n("Close")
                    onClicked: root.close()
                }
            }

            ThemedTextField {
                id: searchField
                Layout.fillWidth: true
                placeholderText: i18n("Search contacts…")
                text: root.searchText
                onTextChanged: {
                    root.searchText = text
                    searchDebounce.restart()
                }
            }

            EmptyState {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: resultsList.count === 0
                text: i18n("No contacts found")
            }

            ListView {
                id: resultsList
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: count > 0
                clip: true
                spacing: 2
                ScrollBar.vertical: ThemedScrollBar {}
                // Unbounded (limit 0) -- an empty search browses the whole
                // address book, not just an autocomplete-sized slice.
                model: root.isOpen ? ContactsApp.searchContacts(pickerQuery.debouncedText, 0) : []

                delegate: Rectangle {
                    width: resultsList.width
                    height: rowContent.implicitHeight + 16
                    radius: Theme.shapeButton
                    color: "transparent"

                    RowLayout {
                        id: rowContent
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Text {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: modelData.name !== "" ? modelData.name : modelData.email
                                color: Theme.inkStrong
                                font.family: Theme.fontUi
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: modelData.email
                                color: Theme.ink
                                font.family: Theme.fontUi
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: modelData.department !== ""
                                textFormat: Text.PlainText
                                text: modelData.department
                                color: Theme.ink
                                font.family: Theme.fontUi
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        GhostButton {
                            text: root.containsEmail(root.toTokens, modelData.email) ? i18n("✓ To") : i18n("To")
                            onClicked: root.contactPicked(modelData.email, "to")
                        }
                        GhostButton {
                            text: root.containsEmail(root.ccTokens, modelData.email) ? i18n("✓ Cc") : i18n("Cc")
                            onClicked: root.contactPicked(modelData.email, "cc")
                        }
                        GhostButton {
                            text: root.containsEmail(root.bccTokens, modelData.email) ? i18n("✓ Bcc") : i18n("Bcc")
                            onClicked: root.contactPicked(modelData.email, "bcc")
                        }
                    }
                }
            }
        }
    }

    // Debounces searchField's text into pickerQuery.debouncedText 150ms
    // after the last keystroke -- this repo's first keystroke-debounce
    // Timer (the only prior Timer precedent, ContactsList.qml's
    // syncStatusTimer, is an unrelated 3s self-hide, not a debounce).
    QtObject {
        id: pickerQuery
        property string debouncedText: ""
    }

    Timer {
        id: searchDebounce
        interval: 150
        onTriggered: pickerQuery.debouncedText = root.searchText
    }
}
