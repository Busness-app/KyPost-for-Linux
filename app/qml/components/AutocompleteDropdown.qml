import QtQuick 2.15
import com.kysecurity.mail 1.0
import "../utils/format.js" as Format

// Compose autocomplete (ContactAutocomplete.md): a floating result list below
// the currently-active TokenField, driven by ContactsApp.searchContacts().
// One shared instance lives in Compose.qml, repositioned under whichever
// TokenField last raised queryChanged -- see TokenField.qml's `dropdown`
// duck-typed contract (visible, moveSelectionUp()/moveSelectionDown()/
// currentSelection()/close()) for how keyboard nav reaches this component.
//
// Judgment call: plain Item/Rectangle, not QtQuick.Controls' Popup -- every
// other component in this directory (EmptyState/StatusBadge/PillTab) is a
// plain themed Rectangle, and Popup's modal/overlay/focus-stealing semantics
// are more than a lightweight inline dropdown needs.
//
// Judgment call: doesn't reuse EmptyState.qml for the "no contacts found"
// row -- that component is a large (240x160, dashed-border) full-page
// placeholder per its own doc comment, the wrong weight class for a 1-line
// dropdown affordance.
Item {
    id: root

    property string query: ""
    property bool isOpen: false
    property int currentIndex: -1

    readonly property string trimmedQuery: query.trim()
    readonly property var results: trimmedQuery.length > 0 ? ContactsApp.searchContacts(trimmedQuery, 5) : []

    visible: root.isOpen && root.trimmedQuery.length > 0
    implicitWidth: 280
    implicitHeight: panel.implicitHeight

    signal itemChosen(string email)

    onResultsChanged: currentIndex = results.length > 0 ? 0 : -1
    onVisibleChanged: if (!visible) currentIndex = -1

    function open() { root.isOpen = true }
    function close() { root.isOpen = false }

    function moveSelectionUp() {
        if (root.results.length === 0)
            return
        root.currentIndex = root.currentIndex <= 0 ? root.results.length - 1 : root.currentIndex - 1
    }

    function moveSelectionDown() {
        if (root.results.length === 0)
            return
        root.currentIndex = (root.currentIndex + 1) % root.results.length
    }

    function currentSelection() {
        if (root.currentIndex < 0 || root.currentIndex >= root.results.length)
            return null
        return root.results[root.currentIndex]
    }

    // Wraps the first case-insensitive occurrence of needle in `text` with
    // <b>...</b> for a rich-text Text item -- a "good enough" highlight, not
    // a multi-occurrence highlighter. `text` is escaped first (Format.
    // escapeHtml(), same set EmailDetail.qml/Compose.qml use) so the markup
    // this function adds is the only markup that ever reaches Text.RichText.
    function highlighted(text, needle) {
        const escaped = Format.escapeHtml(text)
        if (needle.length === 0)
            return escaped
        const idx = escaped.toLowerCase().indexOf(needle.toLowerCase())
        if (idx < 0)
            return escaped
        return escaped.substring(0, idx) + "<b>" + escaped.substring(idx, idx + needle.length) + "</b>"
            + escaped.substring(idx + needle.length)
    }

    Rectangle {
        id: panel
        width: root.width
        implicitHeight: column.implicitHeight + 8
        radius: Theme.shapeField
        color: Theme.panel
        border.width: 1
        border.color: Theme.line

        Column {
            id: column
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 4

            Repeater {
                model: root.results
                delegate: Rectangle {
                    width: column.width
                    height: rowContent.implicitHeight + 12
                    radius: Theme.shapeButton
                    color: index === root.currentIndex ? Theme.accent : "transparent"

                    Column {
                        id: rowContent
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 8
                        spacing: 1

                        Text {
                            width: parent.width
                            text: root.highlighted(modelData.name !== "" ? modelData.name : modelData.email,
                                                     root.trimmedQuery)
                            textFormat: Text.RichText
                            color: index === root.currentIndex ? Theme.readableOnAccent : Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            visible: modelData.name !== ""
                            text: root.highlighted(modelData.email, root.trimmedQuery)
                            textFormat: Text.RichText
                            color: index === root.currentIndex ? Theme.readableOnAccent : Theme.ink
                            font.family: Theme.fontUi
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }

                    TapHandler {
                        onTapped: root.itemChosen(modelData.email)
                    }
                }
            }

            Text {
                width: column.width
                visible: root.results.length === 0
                text: i18n("No contacts found")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                topPadding: 8
                bottomPadding: 8
            }
        }
    }
}
