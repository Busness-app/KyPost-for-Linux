import QtQuick 2.15
import com.kysecurity.mail 1.0

// Compose autocomplete (ContactAutocomplete.md): a wrapping row of address
// "pill" tokens with a trailing plain-text sub-field, replacing Compose.qml's
// old bare ThemedTextField for To/Cc/Bcc. Pill visual shape is lifted
// verbatim from Compose.qml's own pre-existing attachment-chip block
// (Rectangle + Row + Text + "✕" + TapHandler) -- there's nothing
// conceptually new about the chip itself, just what triggers adding one.
//
// Judgment call: doesn't own its own AutocompleteDropdown instance -- Compose.qml
// owns one shared dropdown positioned under whichever TokenField last raised
// queryChanged, and hands it back here via the `dropdown` property so
// Up/Down/Enter/Tab can be forwarded to it. This is a small duck-typed
// contract (dropdown.moveSelectionUp()/moveSelectionDown()/currentSelection()/
// close()/visible), not a hard type dependency -- keeps this component usable
// without necessarily reaching back into a specific dropdown implementation.
Item {
    id: root

    // Public API.
    property var tokens: [] // [{email, valid}]
    property string placeholderText: ""
    property var dropdown: null // duck-typed: visible, moveSelectionUp()/moveSelectionDown()/currentSelection()/close()

    readonly property string joinedText: {
        const emails = []
        for (let i = 0; i < tokens.length; i++)
            emails.push(tokens[i].email)
        return emails.join(", ")
    }

    // "Good enough" RFC 5322 substitute (local@domain.tld shape), not a full
    // RFC 5322 grammar -- matches the source doc's "standard email regex"
    // wording, not a formal-grammar requirement.
    readonly property var emailPattern: /^[^\s@]+@[^\s@]+\.[^\s@]+$/

    signal duplicateRejected(string email)
    // Emitted on every keystroke with the current trimmed input text
    // (possibly empty) -- Compose.qml treats an empty query as "close the
    // dropdown", a non-empty one as "open/reposition it under this field".
    signal queryChanged(string query)

    function addToken(email) {
        const trimmed = email.trim()
        if (trimmed === "")
            return
        for (let i = 0; i < root.tokens.length; i++) {
            if (root.tokens[i].email.toLowerCase() === trimmed.toLowerCase()) {
                root.duplicateRejected(trimmed)
                return
            }
        }
        root.tokens = root.tokens.concat([{ email: trimmed, valid: root.emailPattern.test(trimmed) }])
    }

    function removeTokenAt(index) {
        const updated = root.tokens.slice()
        updated.splice(index, 1)
        root.tokens = updated
    }

    // Enter/Tab/comma with no dropdown selection active -- commits whatever
    // is currently typed, flagging it (not dropping it) if it fails the
    // email pattern.
    function commitInputAsToken() {
        const text = inputField.text.trim()
        if (text === "")
            return
        inputField.text = ""
        root.addToken(text)
    }

    implicitWidth: 260
    implicitHeight: Math.max(flow.implicitHeight, 40)

    Rectangle {
        anchors.fill: parent
        radius: Theme.shapeField
        color: Theme.panel
        border.width: 1
        border.color: inputField.activeFocus ? Theme.accent : Theme.line

        Behavior on border.color {
            ColorAnimation { duration: 120 }
        }
    }

    Flow {
        id: flow
        anchors.fill: parent
        anchors.margins: 6
        spacing: 6

        Repeater {
            model: root.tokens
            delegate: Rectangle {
                radius: Theme.shapeButton
                color: Theme.panel
                border.width: 1
                border.color: modelData.valid ? Theme.line : Theme.dangerColor
                implicitWidth: chipRow.implicitWidth + 18
                implicitHeight: chipRow.implicitHeight + 10

                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: 6

                    Text {
                        textFormat: Text.PlainText
                        text: modelData.email
                        color: modelData.valid ? Theme.inkStrong : Theme.dangerColor
                        font.family: Theme.fontUi
                        font.pixelSize: 12
                    }
                    Text {
                        text: "✕"
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: 12

                        TapHandler {
                            onTapped: root.removeTokenAt(index)
                        }
                    }
                }
            }
        }

        TextInput {
            id: inputField
            width: Math.max(80, flow.width - 20)
            height: 24
            color: Theme.inkStrong
            font.family: Theme.fontUi
            font.pixelSize: 14
            selectByMouse: true
            clip: true

            Text {
                text: root.placeholderText
                visible: inputField.text === "" && !inputField.activeFocus
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 14
            }

            onTextChanged: root.queryChanged(text.trim())

            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Up) {
                    if (root.dropdown && root.dropdown.visible)
                        root.dropdown.moveSelectionUp()
                    event.accepted = true
                } else if (event.key === Qt.Key_Down) {
                    if (root.dropdown && root.dropdown.visible)
                        root.dropdown.moveSelectionDown()
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    if (root.dropdown && root.dropdown.visible) {
                        root.dropdown.close()
                        event.accepted = true
                    }
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                           || event.key === Qt.Key_Tab || event.key === Qt.Key_Comma) {
                    const hadText = inputField.text !== ""
                    const selection = (root.dropdown && root.dropdown.visible)
                        ? root.dropdown.currentSelection() : null
                    if (selection) {
                        inputField.text = ""
                        root.addToken(selection.email)
                    } else {
                        root.commitInputAsToken()
                    }
                    if (root.dropdown)
                        root.dropdown.close()
                    // Tab's default focus-chain advance is only swallowed
                    // when there was actually something to commit/select --
                    // an empty field lets Tab move focus normally.
                    if (event.key === Qt.Key_Tab && (selection || hadText))
                        event.accepted = true
                    else if (event.key !== Qt.Key_Tab)
                        event.accepted = true
                }
            }
        }
    }
}
