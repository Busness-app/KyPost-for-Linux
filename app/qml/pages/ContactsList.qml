import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.urlxl.mail 1.0
import "../components"
import "../utils/format.js" as Format

// Task 36 -- plain reusable Item, deliberately NOT a Kirigami.Page (see
// Phase 6 global constraint 4): MobileRoot wraps this in a thin
// Kirigami.Page shell when it pushes it (Task 38); DesktopRoot embeds it
// directly inside its list-pane Item (Task 39). Navigation is signal-based,
// same "don't assume how the host navigates" shape as EmailDetail/Compose
// (Task 35) -- tapping a row or "Add" both just report a uid, they never
// push/pop anything themselves.
Item {
    id: root

    // uid === "" means "Add" (create mode) -- ContactDetail.qml's own `uid`
    // property already treats an empty string this way (see its doc
    // comment), so "Add" reuses this single signal with the "" sentinel
    // rather than adding a second addRequested() signal that would carry no
    // real behavioral difference for whichever root ends up handling it.
    signal contactSelected(string uid)
    // PGP QR key exchange: same "let the host decide how to navigate" shape
    // as contactSelected() -- the host pushes/opens PgpScanContactKey.qml
    // and, on a successful scan, creates a brand-new contact from it (see
    // MobileRoot.qml/DesktopRoot.qml's wiring).
    signal scanPgpKeyRequested()

    implicitWidth: 360
    implicitHeight: 640

    Component.onCompleted: ContactsApp.load()

    // Shown for 3s after a Sync tap, then hidden again -- ContactsApp.
    // statusMessage/lastError are ContactsApp's own persistent last-outcome
    // state (they don't clear themselves), so "briefly after" is
    // implemented as a local visibility flag here rather than this file
    // reaching in and blanking properties it doesn't own.
    property bool showSyncStatus: false

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        // Title + the screen's one primary action (Add), same "single
        // PrimaryButton" convention the rest of the app follows (e.g.
        // Compose in DesktopRoot's top bar) -- kept on its own row so it
        // never has to compete with the secondary actions below for width.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: i18n("Contacts")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            PrimaryButton {
                text: i18n("Add")
                onClicked: root.contactSelected("")
            }
        }

        // Secondary actions in a Flow (not a RowLayout): a fixed row can't
        // fit three text buttons in this column's ~300px width without
        // clipping or overlapping the row above it -- Flow wraps to as many
        // lines as needed instead, so nothing is ever cut off or occluded
        // regardless of window width.
        Flow {
            Layout.fillWidth: true
            spacing: 8

            GhostButton {
                text: i18n("Sync")
                enabled: !ContactsApp.isBusy
                onClicked: {
                    ContactsApp.sync()
                    root.showSyncStatus = true
                    syncStatusTimer.restart()
                }
            }
            GhostButton {
                text: i18n("Find Duplicates")
                enabled: !ContactsApp.isBusy
                onClicked: {
                    ContactsApp.dedupe()
                    root.showSyncStatus = true
                    syncStatusTimer.restart()
                }
            }
            GhostButton {
                text: i18n("Scan PGP Key")
                onClicked: root.scanPgpKeyRequested()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.showSyncStatus && (ContactsApp.lastError !== "" || ContactsApp.statusMessage !== "")
            textFormat: Text.PlainText
            text: ContactsApp.lastError !== "" ? ContactsApp.lastError : ContactsApp.statusMessage
            color: ContactsApp.lastError !== "" ? Theme.dangerColor : Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: listView.count === 0
            text: i18n("No contacts yet — sync or add one.")
        }

        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: count > 0
            clip: true
            spacing: 4
            model: ContactsApp.contactModel
            ScrollBar.vertical: ThemedScrollBar {}

            delegate: Rectangle {
                width: listView.width
                height: rowContent.implicitHeight + 20
                radius: Theme.shapeButton
                color: tapHandler.pressed ? Theme.panel : "transparent"

                Behavior on color {
                    ColorAnimation { duration: 120 }
                }

                RowLayout {
                    id: rowContent
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 12

                    Avatar {
                        initials: Format.initialsFromName(model.fn)
                        // extended-contact-fields Task 3: lazy fetch on
                        // list-row become-visible -- ListView only
                        // instantiates delegates for (roughly) visible
                        // rows, so this call naturally fires per row as it
                        // scrolls into view, not for the whole contact
                        // list up front. Guarded on model.photoRef so rows
                        // with no photo never make the ContactsApp call at
                        // all (photoPathFor() itself also short-circuits on
                        // an empty photoRef, but skipping the call here
                        // avoids it entirely for the common no-photo case).
                        photoSource: model.photoRef !== "" ? ContactsApp.photoPathFor(model.uid) : ""
                        size: 34
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            Layout.fillWidth: true
                            text: (model.fn && model.fn.length > 0 ? model.fn : i18n("Unnamed"))
                                + (model.isSelf ? " · " + i18n("You") : "")
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 15
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: model.primaryEmail !== ""
                            textFormat: Text.PlainText
                            text: model.primaryEmail
                            color: Theme.ink
                            font.family: Theme.fontUi
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                    }
                }

                TapHandler {
                    id: tapHandler
                    onTapped: root.contactSelected(model.uid)
                }
            }
        }
    }

    Timer {
        id: syncStatusTimer
        interval: 3000
        onTriggered: root.showSyncStatus = false
    }
}
