import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.kysecurity.mail 1.0

// Compose's rich-body "Insert Link" toolbar action: label + URL, with an
// optional "Style as button" checkbox that renders the same <a> with an
// inline PrimaryButton-like style (RichBodyEditor.qml's onLinkConfirmed
// handler builds the actual HTML/inline style; this dialog only collects
// and validates the three inputs). Same overlay-Item shape as
// AddressBookPickerDialog.qml -- no QtQuick.Controls.Dialog precedent
// anywhere in this codebase.
Item {
    id: root

    property bool isOpen: false
    property string labelText: ""
    property string urlText: ""
    property bool styleAsButton: false
    property string validationError: ""

    signal linkConfirmed(string label, string url, bool asButton)

    function open() {
        root.labelText = ""
        root.urlText = ""
        root.styleAsButton = false
        root.validationError = ""
        root.isOpen = true
    }

    function close() {
        root.isOpen = false
    }

    function isSafeUrl(url) {
        return /^(https?:|mailto:)/i.test(url)
    }

    function confirm() {
        if (root.labelText.trim() === "" || root.urlText.trim() === "") {
            root.validationError = i18n("Please fill in both fields")
            return
        }
        if (!root.isSafeUrl(root.urlText.trim())) {
            root.validationError = i18n("Link must start with http://, https://, or mailto:")
            return
        }
        root.linkConfirmed(root.labelText, root.urlText.trim(), root.styleAsButton)
        root.close()
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
        anchors.centerIn: parent
        width: 320
        implicitHeight: content.implicitHeight + 32
        radius: Theme.shapeSheet
        color: Theme.panel
        border.width: 1
        border.color: Theme.line

        TapHandler {} // swallow taps -- keeps them from reaching the scrim behind

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Text {
                text: i18n("Insert Link")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 16
                font.weight: Font.Medium
            }
            ThemedTextField {
                Layout.fillWidth: true
                placeholderText: i18n("Label")
                text: root.labelText
                onTextChanged: root.labelText = text
            }
            ThemedTextField {
                Layout.fillWidth: true
                placeholderText: i18n("URL")
                text: root.urlText
                onTextChanged: root.urlText = text
            }
            CheckBox {
                id: control
                Layout.fillWidth: true
                text: i18n("Style as button")
                checked: root.styleAsButton
                contentItem: Text {
                    text: control.text
                    color: Theme.inkStrong
                    font.family: Theme.fontUi
                    font.pixelSize: 14
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: control.indicator.width + control.spacing
                }
                onCheckedChanged: root.styleAsButton = checked
            }
            Text {
                Layout.fillWidth: true
                visible: root.validationError !== ""
                text: root.validationError
                color: Theme.dangerColor
                font.family: Theme.fontUi
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                GhostButton {
                    text: i18n("Cancel")
                    onClicked: root.close()
                }
                PrimaryButton {
                    text: i18n("Insert")
                    onClicked: root.confirm()
                }
            }
        }
    }
}
