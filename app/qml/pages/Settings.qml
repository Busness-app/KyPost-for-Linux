import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.urlxl.mail 1.0
import "../components"
// Qualified import of this same directory, aliased to PairingPage -- the
// bare name "Pairing" is ambiguous in this file: the automatic IMPLICIT
// same-directory import (every .qml file in pages/ is implicitly visible by
// filename to every other file in pages/, no import statement needed) would
// resolve "Pairing" to Pairing.qml, but the "com.urlxl.mail 1.0"
// import above explicitly registers a QML singleton ALSO named "Pairing"
// (the PairingController instance, see main.cpp) -- and per QML's import
// precedence rules, an explicit import always wins over the implicit
// per-directory one. The bare name resolved to the singleton at runtime
// ("Pairing: Element is not creatable" -- a singleton can't be instantiated
// with curly-brace syntax), confirmed during this task's manual
// verification. A qualified EXPLICIT import of "." (this same directory,
// re-imported under an alias) outranks the implicit import the same way
// MobileRoot.qml's own explicit `import "pages"` does from outside this
// directory, sidestepping the collision entirely.
import "." as PagesDir

// Task 39 -- plain reusable Item, deliberately NOT a Kirigami.Page (same
// "parent-agnostic" shape as Tasks 35-37's page components, per Phase 6
// global constraint 4): MobileRoot wraps this in a thin Kirigami.Page shell
// when it pushes it from the globalDrawer's "Settings" action (previously a
// stub, see MobileRoot.qml); DesktopRoot hosts it inside a
// Kirigami.OverlaySheet instead of a second ApplicationWindow -- Global
// Constraint 4 leaves that choice open, and OverlaySheet was picked because
// it needs no second top-level window/event-loop lifetime to manage, unlike
// a real separate ApplicationWindow.
//
// 5 panes selected via a PillTab strip, not QtQuick.Controls
// TabBar/TabButton -- keeps this screen themed via the same "PillTab as a
// segmented selector" convention MobileRoot.qml's keyword pill row already
// established, rather than introducing a second, unthemed tab-chrome
// component into the app.
Item {
    id: root

    // Emitted from the header's "Done" button -- same "don't assume
    // push-navigation vs. a pane/sheet" shape as every other page component
    // this phase (EmailDetail/Compose/ContactDetail/Pairing): whichever
    // root hosts this decides what "close" means (pageStack.pop() for
    // Mobile, sheet.close() for Desktop).
    signal closed()
    // PGP QR key exchange: same "let the host decide how to navigate" shape
    // as closed() -- MobileRoot pushes PgpMyQrCode.qml via pageStack,
    // DesktopRoot opens it in a Kirigami.OverlaySheet (same choice it
    // already made for this Settings screen itself).
    signal myPgpQrCodeRequested()

    implicitWidth: 480
    implicitHeight: 560

    property int currentPane: 0 // 0 Connection, 1 Appearance, 2 Keywords, 3 Contacts, 4 Notifications, 5 General, 6 Security
    readonly property var paneNames: [i18n("Connection"), i18n("Appearance"), i18n("Keywords"), i18n("Contacts"), i18n("Notifications"), i18n("General"), i18n("Security")]

    // MailApp.allKeywordSettings() is a Q_INVOKABLE snapshot, not a
    // NOTIFY-bound property (see MailController.h's doc comment on why) --
    // cached here as a plain JS array and re-pulled on load and after every
    // toggle so the Keywords pane's row list stays in sync with whatever
    // setKeywordVisible() just wrote.
    property var keywordSettings: []

    // Which erase-after value the PIN prompt is about to apply. Held here
    // rather than passed through securityPrompt.begin() because that helper
    // takes only a mode and a message, and widening it for one caller would
    // touch every other prompt in this file. AppLockManager clamps and
    // re-checks whatever arrives, so this is a convenience, not a control.
    property int eraseThresholdChoice: 0

    function refreshKeywordSettings() {
        root.keywordSettings = MailApp.allKeywordSettings()
    }

    Component.onCompleted: refreshKeywordSettings()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // ---- header ------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: i18n("Settings")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            GhostButton {
                text: i18n("Done")
                onClicked: root.closed()
            }
        }

        // ---- pane selector -------------------------------------------------
        Flickable {
            Layout.fillWidth: true
            // +10 reserves dedicated space below the pills for the
            // horizontal scrollbar thumb, so it doesn't sit on top of the
            // pane-selector pills themselves.
            implicitHeight: paneTabRow.implicitHeight + 10
            contentWidth: paneTabRow.implicitWidth
            contentHeight: paneTabRow.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalFlick
            ScrollBar.horizontal: ThemedScrollBar {}

            Row {
                id: paneTabRow
                spacing: 8

                Repeater {
                    model: root.paneNames
                    delegate: PillTab {
                        // No textFormat here: PillTab is not a Text, and its
                        // own label already pins Text.PlainText. Assigning it
                        // from outside made the whole component fail to load,
                        // which took DesktopRoot down with it.
                        text: modelData
                        selected: root.currentPane === index
                        onClicked: root.currentPane = index
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.currentPane

            // ---- 1. Connection ----------------------------------------
            // Wrapped in a Flickable (same reasoning as EmailDetail.qml/
            // ContactDetail.qml's own Flickable wraps): this pane's content
            // can exceed the sheet's available height on a short window, and
            // a plain ColumnLayout doesn't clip or scroll its own overflow.
            Flickable {
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: width
                contentHeight: connectionColumn.implicitHeight
                ScrollBar.vertical: ThemedScrollBar {}

                ColumnLayout {
                    id: connectionColumn
                    width: parent.width
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        StatusBadge {
                            active: Pairing.isPaired
                            text: Pairing.isPaired ? i18n("Paired") : i18n("Not paired")
                        }
                        Item { Layout.fillWidth: true }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: Pairing.isPaired
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            SectionLabel { Layout.preferredWidth: 70; text: i18n("Server") }
                            Text {
                                Layout.fillWidth: true
                                text: Pairing.pairedServerHost
                                color: Theme.inkStrong
                                font.family: Theme.fontMono
                                font.pixelSize: 14
                                wrapMode: Text.WordWrap
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            SectionLabel { Layout.preferredWidth: 70; text: i18n("Device") }
                            Text {
                                Layout.fillWidth: true
                                text: Pairing.deviceId
                                color: Theme.inkStrong
                                font.family: Theme.fontMono
                                font.pixelSize: 14
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        // No "Test Connection" / desktop-session pairing here --
                        // both explicitly out of scope, see Phase 6 global
                        // constraint 6 (this client family only ever does
                        // sub/hash native pairing, no separate desktop-session
                        // flow) and the task-39 brief's Connection pane spec.
                        PrimaryButton {
                            text: i18n("Pair This Device…")
                            visible: !Pairing.isPaired
                            onClicked: pairingPopup.open()
                        }
                        DangerButton {
                            text: i18n("Remove Pairing")
                            visible: Pairing.isPaired
                            onClicked: Pairing.removePairing()
                        }
                    }
                }
            }

            // ---- 2. Appearance ------------------------------------------
            // Name-only list + checkmark, not per-theme color swatches:
            // ThemeController only ever exposes the CURRENTLY ACTIVE
            // theme's palette as live QColor properties (see its own doc
            // comment) -- there is no QML-reachable way to peek another
            // theme's bg/panel/accent today without adding new core-to-QML
            // plumbing (an AppTheme::palette(name) bridge) purely for this
            // one swatch row. The task-39 brief explicitly sanctions this
            // exact fallback rather than blocking on that plumbing.
            ListView {
                id: themeListView
                clip: true
                spacing: 2
                model: Theme.themeNames
                ScrollBar.vertical: ThemedScrollBar {}

                delegate: Rectangle {
                    id: themeDelegate
                    width: themeListView.width
                    height: themeRow.implicitHeight + 16
                    radius: Theme.shapeButton
                    color: modelData === Theme.themeName ? Theme.panel
                        : (themeHover.hovered ? Theme.bg : "transparent")

                    Behavior on color {
                        ColorAnimation { duration: 120 }
                    }

                    RowLayout {
                        id: themeRow
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8

                        Text {
                            Layout.fillWidth: true
                            // modelData is a theme's proper name (e.g. "Dark
                            // Matter", "Cyber Punk" -- see core/theme/
                            // AppTheme.cpp), NOT wrapped in i18n(): these are
                            // brand-style palette names (same "don't
                            // translate the product name" reasoning as
                            // "KyPost" itself), AND they live in core/,
                            // which the Phase 8 global-constraints boundary
                            // (item 3) forbids linking KI18n into -- they're
                            // also the literal identifier Theme.setTheme()
                            // stores/compares, not just display text.
                            textFormat: Text.PlainText
                            text: modelData
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                        }
                        Text {
                            visible: modelData === Theme.themeName
                            text: "✓"
                            color: Theme.accent
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                            font.weight: Font.Bold
                        }
                    }

                    HoverHandler {
                        id: themeHover
                    }

                    TapHandler {
                        onTapped: Theme.setTheme(modelData)
                    }
                }
            }

            // ---- 3. Keywords ---------------------------------------------
            Item {
                EmptyState {
                    anchors.fill: parent
                    visible: keywordListView.count === 0
                    text: i18n("No keywords yet.")
                }

                ListView {
                    id: keywordListView
                    anchors.fill: parent
                    visible: count > 0
                    clip: true
                    spacing: 4
                    model: root.keywordSettings
                    ScrollBar.vertical: ThemedScrollBar {}

                    delegate: Rectangle {
                        width: keywordListView.width
                        height: keywordRowContent.implicitHeight + 16
                        radius: Theme.shapeButton
                        color: keywordHover.hovered ? Theme.panel : "transparent"

                        Behavior on color {
                            ColorAnimation { duration: 120 }
                        }

                        RowLayout {
                            id: keywordRowContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 10
                            spacing: 8

                            Text {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: modelData.keyword
                                color: Theme.inkStrong
                                font.family: Theme.fontUi
                                font.pixelSize: 14
                            }
                            PillTab {
                                text: modelData.visible ? i18n("Visible") : i18n("Hidden")
                                selected: modelData.visible
                                onClicked: {
                                    MailApp.setKeywordVisible(modelData.keyword, !modelData.visible)
                                    root.refreshKeywordSettings()
                                }
                            }
                        }

                        HoverHandler {
                            id: keywordHover
                        }
                    }
                }
            }

            // ---- 4. Contacts ------------------------------------------------
            // No "Sync to system contacts" toggle: that's the Mac/Android
            // apps' OS-level Contacts-app export integration, and this repo
            // has no Linux equivalent anywhere in core/ or app/ (nothing
            // this plan has built through Phase 6 talks to a system address
            // book) -- a toggle here would do nothing when flipped, which
            // this task's brief explicitly forbids. This pane shows real
            // sync status/action instead (reusing ContactsApp exactly as
            // ContactsList.qml's own header does).
            Flickable {
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: width
                contentHeight: contactsColumn.implicitHeight
                ScrollBar.vertical: ThemedScrollBar {}

                ColumnLayout {
                    id: contactsColumn
                    width: parent.width
                    spacing: 12

                    Text {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: ContactsApp.lastError !== "" ? ContactsApp.lastError
                            : (ContactsApp.statusMessage !== "" ? ContactsApp.statusMessage : i18n("No sync yet."))
                        color: ContactsApp.lastError !== "" ? Theme.dangerColor : Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        PrimaryButton {
                            text: i18n("Sync Now")
                            enabled: !ContactsApp.isBusy
                            onClicked: ContactsApp.sync()
                        }
                        GhostButton {
                            text: i18n("My PGP QR Code")
                            onClicked: root.myPgpQrCodeRequested()
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // ---- 5. Notifications -------------------------------------------
            // Read-only display only -- no editable push-server-URL field.
            // Global constraint 6's deviceToken gap means live re-
            // registration isn't wired this phase anyway (see
            // PairingController.h's known-gap comment), so an editable
            // field here would look functional while doing nothing; a
            // Phase 7 follow-up once real registration lands end-to-end.
            Flickable {
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: width
                contentHeight: notificationsColumn.implicitHeight
                ScrollBar.vertical: ThemedScrollBar {}

                ColumnLayout {
                    id: notificationsColumn
                    width: parent.width
                    spacing: 10

                    Text {
                        Layout.fillWidth: true
                        visible: Pairing.deliveryMode === "" && Pairing.transport === ""
                        text: i18n("Not yet registered")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: 13
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: Pairing.deliveryMode !== "" || Pairing.transport !== ""
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            SectionLabel { Layout.preferredWidth: 100; text: i18n("Delivery Mode") }
                            Text {
                                Layout.fillWidth: true
                                text: Pairing.deliveryMode
                                color: Theme.inkStrong
                                font.family: Theme.fontMono
                                font.pixelSize: 14
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            SectionLabel { Layout.preferredWidth: 100; text: i18n("Transport") }
                            Text {
                                Layout.fillWidth: true
                                text: Pairing.transport
                                color: Theme.inkStrong
                                font.family: Theme.fontMono
                                font.pixelSize: 14
                            }
                        }
                    }

                    // The "Push Server" row that used to sit here displayed the
                    // embedded ntfy subscriber's base URL. Both it and the
                    // subscriber were removed on 2026-07-26 -- see
                    // core/domain/TransportStateMachine.h. What replaces it is
                    // the honest description of where notifications now come
                    // from: a system UnifiedPush distributor if one is
                    // installed, otherwise polling the relay directly, which
                    // involves no third party at all.
                    MutedHint {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        wrapMode: Text.WordWrap
                        text: i18n("KyPost receives notifications through your system's UnifiedPush "
                                    + "distributor. Sender names and subject lines pass through the "
                                    + "push server that distributor is configured to use -- change it "
                                    + "there, not here. With no distributor installed, KyPost instead "
                                    + "checks the relay directly every 90 seconds and nothing passes "
                                    + "through a third party.")
                    }
                }
            }

            // ---- 6. General ---------------------------------------------
            // Wrapped in a Flickable, same as the panes above -- this is the
            // tallest pane (Interface Mode + hint + conditionally-visible
            // System Tray section with 2 more rows), the most likely to
            // exceed the sheet's available height on a short/shrunk window.
            Flickable {
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: width
                contentHeight: generalColumn.implicitHeight
                ScrollBar.vertical: ThemedScrollBar {}

                ColumnLayout {
                    id: generalColumn
                    width: parent.width
                    spacing: 16

                    SectionLabel { text: i18n("Interface Mode") }

                    Row {
                        Layout.fillWidth: true
                        spacing: 8

                        PillTab {
                            text: i18n("Auto")
                            selected: General.preferredMode === "auto"
                            onClicked: General.setPreferredMode("auto")
                        }
                        PillTab {
                            text: i18n("Desktop")
                            selected: General.preferredMode === "desktop"
                            onClicked: General.setPreferredMode("desktop")
                        }
                        PillTab {
                            text: i18n("Mobile")
                            selected: General.preferredMode === "mobile"
                            onClicked: General.setPreferredMode("mobile")
                        }
                    }

                    MutedHint {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18n("Restart KyPost for interface mode changes to take effect.")
                    }

                    // Desktop-only: gated on the mode THIS process actually
                    // resolved to at startup, not the pending preference above --
                    // never shown mid-session in a Mobile launch even if the
                    // user just picked "Desktop" for next time.
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: General.isDesktopMode
                        spacing: 16

                        SectionLabel { text: i18n("System Tray") }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: i18n("Enable Tray Icon")
                                color: Theme.inkStrong
                                font.family: Theme.fontUi
                                font.pixelSize: 14
                            }
                            PillTab {
                                text: General.trayIconEnabled ? i18n("On") : i18n("Off")
                                selected: General.trayIconEnabled
                                onClicked: General.setTrayIconEnabled(!General.trayIconEnabled)
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            enabled: General.trayIconEnabled
                            opacity: enabled ? 1.0 : 0.5
                            Text {
                                Layout.fillWidth: true
                                text: i18n("Minimize to Tray on Close")
                                color: Theme.inkStrong
                                font.family: Theme.fontUi
                                font.pixelSize: 14
                            }
                            PillTab {
                                text: General.minimizeToTrayOnClose ? i18n("On") : i18n("Off")
                                selected: General.minimizeToTrayOnClose
                                onClicked: General.setMinimizeToTrayOnClose(!General.minimizeToTrayOnClose)
                            }
                        }
                    }
                }
            }

            // ---- 7. Security --------------------------------------------
            // Every toggle here needs the current PIN, including turning
            // things OFF: a lock that can be removed by whoever already has
            // the window open protects nothing.
            Flickable {
                contentWidth: width
                contentHeight: securityColumn.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThemedScrollBar {}

                ColumnLayout {
                    id: securityColumn
                    width: parent.width
                    spacing: 16

                    SectionLabel { text: i18n("App Lock") }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: i18n("Require Unlock to Open")
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                        }
                        PillTab {
                            text: AppLock.lockEnabled ? i18n("On") : i18n("Off")
                            selected: AppLock.lockEnabled
                            onClicked: {
                                if (AppLock.lockEnabled)
                                    securityPrompt.begin("disableLock", i18n("Enter your PIN to turn off the lock"))
                                else
                                    securityPrompt.begin("setPin", i18n("Choose a PIN"))
                            }
                        }
                    }

                    MutedHint {
                        Layout.fillWidth: true
                        text: i18n("A PIN is required every time KyPost starts, and whenever the window is hidden or minimised. After 10 failed attempts all local data is erased.")
                    }

                    // Says plainly what the PIN does not do.
                    //
                    // Cached mail and contacts live in an unencrypted SQLite
                    // file, so anyone who can read this account's files can
                    // read them without ever meeting this PIN. The lock
                    // guards the running app; "Keep nothing on this device"
                    // below is the setting that guards the disk. Leaving
                    // that unsaid let a PIN prompt imply at-rest protection
                    // it has never provided.
                    MutedHint {
                        Layout.fillWidth: true
                        text: i18n("The PIN protects this app while it is running. It does not encrypt the cached mail and contacts stored on this computer — for that, turn on Keep nothing on this device below.")
                    }

                    GhostButton {
                        visible: AppLock.lockEnabled
                        text: i18n("Change PIN…")
                        onClicked: securityPrompt.begin("changePin", i18n("Enter your current PIN, then a new one"))
                    }

                    Item { Layout.preferredHeight: 4 }

                    SectionLabel { text: i18n("Hostile Location Protection") }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        // Same dependency as the credential gate: only
                        // meaningful alongside a PIN.
                        enabled: AppLock.lockEnabled
                        opacity: enabled ? 1.0 : 0.5
                        Text {
                            Layout.fillWidth: true
                            text: i18n("Keep nothing on this device")
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                        }
                        PillTab {
                            text: AppLock.hostileLocationEnabled ? i18n("On") : i18n("Off")
                            selected: AppLock.hostileLocationEnabled
                            onClicked: securityPrompt.begin(
                                AppLock.hostileLocationEnabled ? "hlpOff" : "hlpOn",
                                AppLock.hostileLocationEnabled
                                    ? i18n("Enter your PIN to turn this off. KyPost will restart.")
                                    : i18n("Enter your PIN. KyPost will erase its local data and restart."))
                        }
                    }

                    MutedHint {
                        Layout.fillWidth: true
                        text: i18n("Mail, contacts and folders are held in memory only and are gone when KyPost closes. Turning this on erases what is already stored and restarts the app. Attachments open in a temporary location instead of being saved to Downloads.")
                    }

                    Item { Layout.preferredHeight: 4 }

                    SectionLabel { text: i18n("Erase after failed attempts") }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        // Nothing to count failures against without a PIN.
                        enabled: AppLock.lockEnabled
                        opacity: enabled ? 1.0 : 0.5
                        Text {
                            Layout.fillWidth: true
                            text: i18n("Erase this device after")
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                        }
                        Repeater {
                            model: [0, 5, 10, 20]
                            delegate: PillTab {
                                required property int modelData
                                text: modelData === 0
                                    ? i18n("Never")
                                    : i18np("%1 attempt", "%1 attempts", modelData)
                                selected: AppLock.wipeAfterAttempts === modelData
                                onClicked: {
                                    if (AppLock.wipeAfterAttempts === modelData)
                                        return
                                    eraseThresholdChoice = modelData
                                    securityPrompt.begin(
                                        "eraseAfter",
                                        modelData === 0
                                            ? i18n("Enter your PIN to stop KyPost erasing this device after "
                                                   + "repeated failed attempts.")
                                            : i18np("Enter your PIN to erase this device after %1 failed attempt.",
                                                    "Enter your PIN to erase this device after %1 failed attempts.",
                                                    modelData))
                                }
                            }
                        }
                    }

                    // Says what "Never" does and does not switch off. It is
                    // the erase that stops, not the rate limiting -- and
                    // being explicit here is the difference between an
                    // informed choice and a surprise.
                    MutedHint {
                        Layout.fillWidth: true
                        text: AppLock.wipeAfterAttempts === 0
                            ? i18n("KyPost will not erase itself, however many attempts fail. Repeated failures are still slowed down and eventually refused until you restart the app. Anyone who takes this computer can keep guessing.")
                            : i18n("Cached mail, contacts and this device's pairing are erased after this many consecutive failed attempts. Your mail on the server is not affected.")
                    }

                    Item { Layout.preferredHeight: 4 }

                    SectionLabel { text: i18n("Push and MFA") }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        // Meaningless without a PIN to gate on, so the whole
                        // row is inert until the lock is enabled.
                        enabled: AppLock.lockEnabled
                        opacity: enabled ? 1.0 : 0.5
                        Text {
                            Layout.fillWidth: true
                            text: i18n("Require unlock to receive push and MFA")
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                        }
                        PillTab {
                            text: AppLock.credentialPinGateEnabled ? i18n("On") : i18n("Off")
                            selected: AppLock.credentialPinGateEnabled
                            onClicked: securityPrompt.begin(
                                AppLock.credentialPinGateEnabled ? "gateOff" : "gateOn",
                                i18n("Enter your PIN"))
                        }
                    }

                    MutedHint {
                        Layout.fillWidth: true
                        text: i18n("When on, the pairing credential is encrypted with your PIN. Mail, push and MFA stop working until you unlock the app — that is the point, and it is a real tradeoff.")
                    }

                    // Permanently visible, not a one-time dialog: this is
                    // true whatever the settings above are set to, and a
                    // warning the user has dismissed is a warning they no
                    // longer have.
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: pushWarningText.implicitHeight + 24
                        radius: Theme.shapePanel
                        color: Theme.dangerFillColor
                        border.width: 1
                        border.color: Theme.dangerBorderColor

                        Text {
                            id: pushWarningText
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 12
                            text: i18n("Push notifications send the sender and subject through a relay server (UnifiedPush/ntfy), in the clear, even with the settings above enabled. For zero leakage, ask your server admin to switch this device to Pull mode.")
                            color: Theme.ink
                            font.family: Theme.fontUi
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }

    // ---- Security PIN prompt ---------------------------------------------
    // One prompt for all five flows (set / change / disable / gate on / gate
    // off). They differ only in which fields are shown and which AppLock
    // call runs, so a single popup with a mode is far less to get wrong than
    // five near-identical dialogs.
    Popup {
        id: securityPrompt

        property string mode: ""
        property string headingText: ""
        property string errorText: ""
        // Only the change-PIN flow needs both fields; every other flow
        // needs exactly one.
        readonly property bool needsCurrent: mode === "changePin" || mode === "disableLock"
                                              || mode === "gateOn" || mode === "gateOff"
                                              || mode === "hlpOn" || mode === "hlpOff"
        readonly property bool needsNew: mode === "setPin" || mode === "changePin"

        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(360, root.width - 32)
        padding: 20

        background: Rectangle {
            color: Theme.panel
            radius: Theme.shapeSheet
            border.width: 1
            border.color: Theme.line
        }

        function begin(newMode, heading) {
            securityPrompt.mode = newMode
            securityPrompt.headingText = heading
            securityPrompt.errorText = ""
            currentPinField.text = ""
            newPinField.text = ""
            confirmPinField.text = ""
            securityPrompt.open()
        }

        // Live PinPolicy verdict for whatever is currently typed, from the
        // same C++ rule AppLock.setPin() enforces -- so the dialog explains
        // a rejection before the user commits instead of just refusing.
        readonly property string newPinProblem:
            !securityPrompt.needsNew || newPinField.text.length === 0
                ? ""
                : AppLock.pinRejectionReason(newPinField.text)
        readonly property bool confirmMismatch:
            securityPrompt.needsNew && confirmPinField.text.length > 0
            && confirmPinField.text !== newPinField.text

        onOpened: {
            if (securityPrompt.needsCurrent)
                currentPinField.inputField.forceActiveFocus()
            else
                newPinField.inputField.forceActiveFocus()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: securityPrompt.headingText
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 14
                font.bold: true
                wrapMode: Text.WordWrap
            }

            ThemedTextField {
                id: currentPinField
                Layout.fillWidth: true
                visible: securityPrompt.needsCurrent
                placeholderText: i18n("Current PIN")
                Component.onCompleted: {
                    inputField.echoMode = TextInput.Password
                    inputField.inputMethodHints = Qt.ImhDigitsOnly
                }
            }

            ThemedTextField {
                id: newPinField
                Layout.fillWidth: true
                visible: securityPrompt.needsNew
                placeholderText: i18np("New PIN (at least %1 digit)",
                                       "New PIN (at least %1 digits)", AppLock.minimumPinLength)
                Component.onCompleted: {
                    inputField.echoMode = TextInput.Password
                    inputField.inputMethodHints = Qt.ImhDigitsOnly
                }
            }

            // Second entry, because the PIN is unrecoverable by design: get
            // it wrong once and the only way back in is ten failed attempts
            // and a full local wipe. A single masked field with no
            // confirmation was one typo away from that.
            ThemedTextField {
                id: confirmPinField
                Layout.fillWidth: true
                visible: securityPrompt.needsNew
                placeholderText: i18n("Confirm new PIN")
                Component.onCompleted: {
                    inputField.echoMode = TextInput.Password
                    inputField.inputMethodHints = Qt.ImhDigitsOnly
                }
            }

            MutedHint {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: securityPrompt.newPinProblem !== "" || securityPrompt.confirmMismatch
                color: Theme.dangerColor
                text: securityPrompt.confirmMismatch
                          ? i18n("The two PINs do not match.")
                          : securityPrompt.newPinProblem
            }

            Text {
                Layout.fillWidth: true
                visible: securityPrompt.errorText !== ""
                text: securityPrompt.errorText
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
                    onClicked: securityPrompt.close()
                }
                PrimaryButton {
                    text: i18n("Confirm")
                    enabled: (!securityPrompt.needsCurrent || currentPinField.text.length > 0)
                             && (!securityPrompt.needsNew
                                 || (securityPrompt.newPinProblem === ""
                                     && newPinField.text.length > 0
                                     && confirmPinField.text === newPinField.text))
                    onClicked: securityPrompt.submit()
                }
            }
        }

        function submit() {
            const current = currentPinField.text
            const fresh = newPinField.text
            let ok = false

            if (securityPrompt.mode === "setPin" || securityPrompt.mode === "changePin")
                ok = AppLock.setPin(current, fresh)
            else if (securityPrompt.mode === "disableLock")
                ok = AppLock.disableLock(current)
            else if (securityPrompt.mode === "gateOn")
                ok = AppLock.setCredentialPinGateEnabled(true, current)
            else if (securityPrompt.mode === "gateOff")
                ok = AppLock.setCredentialPinGateEnabled(false, current)
            else if (securityPrompt.mode === "hlpOn")
                ok = AppLock.setHostileLocationEnabled(true, current)
            else if (securityPrompt.mode === "hlpOff")
                ok = AppLock.setHostileLocationEnabled(false, current)
            else if (securityPrompt.mode === "eraseAfter")
                ok = AppLock.setWipeAfterAttempts(eraseThresholdChoice, current)

            if (ok) {
                securityPrompt.close()
                return
            }
            // Stay open so the user can retry without re-navigating.
            //
            // These calls now also refuse when the device secret could not
            // be re-wrapped under the new PIN (or unwrapped from the old
            // one), which is a different situation from a mistyped PIN and
            // has a different fix -- so the wording covers both rather than
            // implying the user got the PIN wrong. AppLockManager
            // deliberately fails closed there: leaving the lock on is
            // recoverable, stranding the pairing behind a destroyed key is
            // not.
            securityPrompt.errorText = AppLock.credentialPinGateEnabled
                ? i18n("Incorrect PIN, or the stored credentials could not be re-encrypted. If the "
                       + "PIN is right, check that your system keyring is unlocked and try again.")
                : i18n("Incorrect PIN, or the change could not be saved.")
            currentPinField.text = ""
        }
    }

    // ---- "Pair This Device…" overlay -------------------------------------
    // Settings.qml manages its own nested popup rather than asking the host
    // to navigate anywhere -- keeps this file host-agnostic (same reasoning
    // as the signal-based navigation on EmailDetail/ContactsList/etc.)
    // instead of assuming a pageStack that DesktopRoot doesn't have.
    Popup {
        id: pairingPopup
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(380, root.width - 32)
        height: Math.min(560, root.height - 32)
        padding: 0

        background: Rectangle {
            color: Theme.panel
            radius: Theme.shapeSheet
            border.width: 1
            border.color: Theme.line
        }

        contentItem: PagesDir.Pairing {
            anchors.fill: parent
            onClosed: pairingPopup.close()
        }
    }
}
