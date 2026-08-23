import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import com.urlxl.mail 1.0
import "components"
import "pages"
import "utils/format.js" as Format

// Task 39 -- top-level desktop navigation shell, replacing the Task 1 stub.
// Per Phase 6 global constraint 4, this is a persistent 3-column layout
// (sidebar | list | detail) -- NOT pageStack push-navigation like
// MobileRoot.qml. Selection state (currentSection/currentFolder/
// selectedMessageId/selectedContactUid/detailMode) drives which content each
// pane shows; Tasks 35-37's plain-Item page components (EmailDetail/
// Compose/ContactsList/ContactDetail) are embedded directly here, never
// wrapped in a Kirigami.Page (that wrapping is MobileRoot-specific).
//
// Judgment call: a plain RowLayout with a fixed-width sidebar, a
// fill-width list column, and a fixed-width collapsible detail column --
// NOT a resizable SplitView. Global constraint 4 explicitly allows either;
// a RowLayout was chosen to avoid taking on SplitView's less-travelled
// hidden-item-reflow behavior in this codebase (nothing here uses SplitView
// yet) for a nice-to-have (user-resizable columns) the brief doesn't
// require. Documented here per that constraint's "your call, document it."
Kirigami.ApplicationWindow {
    id: root
    width: 1024
    height: 768
    // Explicit background: the 3-column RowLayout below (sidebar 200 + list
    // 340 + detail up to 420 = 960 max) doesn't necessarily sum to the
    // window's actual width -- none of the three columns has an
    // unconditional Layout.fillWidth, so any leftover width (960 already
    // being less than this window's own default 1024, before any resize)
    // shows raw unpainted window background instead of a themed one. That
    // showed up as a persistent gray block on the right edge. Without this,
    // the window's own background defaults to Kirigami/Qt's own choice, not
    // Theme.bg.
    color: Theme.bg
    // 900 = sidebar's own 200 + list's own 340 + detail's real content floor
    // 360 (EmailDetail/ContactDetail/Compose's own implicitWidth, not the
    // detail column's 420 preferred width) -- see the matching
    // Layout.minimumWidth values on the three columns below, which this
    // floor is derived from and reinforces rather than fights.
    minimumWidth: 900
    minimumHeight: 480
    visible: true
    title: "KyPost" // product name -- not translated, same as MobileRoot.qml's window title

    // Minimize to tray instead of quitting, when both are enabled (System
    // Tray settings pane) -- General.trayIconEnabled is only ever true here
    // since TrayController only exists in Desktop mode (see main.cpp).
    onClosing: (close) => {
        if (General.trayIconEnabled && General.minimizeToTrayOnClose) {
            close.accepted = false
            root.visible = false
            // Hiding to tray is the desktop equivalent of Android's onStop:
            // the user has walked away from a window that is still running.
            //
            // lockAfterGrace(), not lockNow(): the grace period defaults to
            // 0, in which case this locks synchronously exactly as before.
            // Only a user who has deliberately chosen a delay gets one.
            AppLock.lockAfterGrace()
        }
    }

    // Re-lock whenever the window is hidden or minimised by any route, not
    // only the minimise-to-tray path above -- otherwise a plain minimise
    // would leave the mail on screen the moment it is restored.
    onVisibleChanged: {
        if (!root.visible)
            AppLock.lockAfterGrace()
        else
            AppLock.cancelPendingLock()
    }
    onWindowStateChanged: {
        if (root.windowState === Qt.WindowMinimized)
            AppLock.lockAfterGrace()
        else if (root.visible)
            AppLock.cancelPendingLock()
    }

    // ---- selection state --------------------------------------------
    property string currentSection: "mail" // "mail" | "contacts"
    property string currentFolder: "INBOX" // wire folder name; meaningful when currentSection === "mail"
    property string selectedMessageId: ""
    property string selectedEmailFolder: ""
    property string selectedContactUid: ""
    // "empty" | "email" | "contact" | "compose" -- which component (if any)
    // the detail pane currently shows. Kept independent of
    // selectedMessageId/selectedContactUid (rather than inferring the mode
    // from which one is non-empty) so an in-progress Compose isn't
    // accidentally clobbered by whatever selection state happens to be
    // sitting in the other two.
    property string detailMode: "empty"
    property var composeSeed: ({ to: "", subject: "", body: "" })

    // Detail-pane collapse: in-memory only, not persisted via SettingsStore
    // -- this is a per-session view preference, not different from
    // countless other transient UI states in this app that don't survive a
    // restart (e.g. MobileRoot's activeTab). Adding a SettingsStore field
    // for one boolean felt like more plumbing than a "your call" judgment
    // call this size warrants; revisit if user feedback wants it to stick.
    property bool detailCollapsed: false

    function folderDisplayName(wireName) {
        return Format.folderDisplayName(MailApp.mailFolders(), wireName)
    }

    function selectFolder(wireName) {
        root.currentSection = "mail"
        root.currentFolder = wireName
        root.selectedMessageId = ""
        root.detailMode = "empty"
        MailApp.selectFolder(wireName)
    }

    // ---- folder management -------------------------------------------
    // One reusable prompt for create/rename and one confirm for delete,
    // rather than three near-identical Dialogs. `folderPromptMode` decides
    // which MailApp call the accept button makes.
    function promptCreateFolder(parentWireName) {
        folderPrompt.mode = "create"
        folderPrompt.targetFolder = parentWireName
        folderPrompt.headingText = i18n("New folder in %1", root.folderDisplayName(parentWireName))
        folderPrompt.nameText = ""
        folderPrompt.open()
    }

    function promptRenameFolder(wireName, displayName) {
        folderPrompt.mode = "rename"
        folderPrompt.targetFolder = wireName
        folderPrompt.headingText = i18n("Rename folder")
        folderPrompt.nameText = displayName
        folderPrompt.open()
    }

    function promptDeleteFolder(wireName, displayName) {
        folderDeleteConfirm.targetFolder = wireName
        folderDeleteConfirm.displayName = displayName
        folderDeleteConfirm.open()
    }

    function selectContactsSection() {
        root.currentSection = "contacts"
        root.selectedContactUid = ""
        root.detailMode = "empty"
    }

    // Detail pane collapsed means the embedded pane is width-0 (see
    // detailCollapsed below) -- setting selection state into it would be
    // invisible, so pop out a standalone window instead (same mechanism as
    // the detail pane's own pop-out button, see popOutEmail() further down).
    function selectEmail(messageId, folder) {
        if (root.detailCollapsed) {
            root.popOutEmail(messageId, folder)
            return
        }
        root.selectedMessageId = messageId
        root.selectedEmailFolder = folder
        root.detailMode = "email"
    }

    // Same detailCollapsed branch as selectEmail() above -- the pane is
    // width-0 when collapsed, so setting selection state into it would be
    // invisible; pop out a standalone window instead.
    function selectContact(uid) {
        if (root.detailCollapsed) {
            root.popOutContact(uid)
            return
        }
        root.selectedContactUid = uid
        root.detailMode = "contact"
    }

    // Seeds composeSeed BEFORE flipping detailMode, since the compose
    // Loader below only reads composeSeed at the moment it instantiates a
    // fresh Compose (see the Loader's own comment for why it's a Loader,
    // not a persistently-embedded instance like EmailDetail/ContactDetail).
    // Same detailCollapsed branch as selectEmail() above -- the pane is
    // width-0 when collapsed, so setting composeSeed/detailMode into it
    // would be invisible; pop out a standalone window instead.
    function openCompose(to, subject, body) {
        if (root.detailCollapsed) {
            root.popOutCompose(to || "", subject || "", body || "")
            return
        }
        root.composeSeed = { to: to || "", subject: subject || "", body: body || "" }
        root.detailMode = "compose"
    }

    function closeDetail() {
        root.detailMode = "empty"
        root.selectedMessageId = ""
        root.selectedContactUid = ""
    }

    Component.onCompleted: {
        MailApp.refresh()
        // Populates the sidebar's Archive subfolders. Cheap and quiet: a
        // failure leaves the six standard mailboxes showing (see
        // MailController::refreshFolders).
        MailApp.refreshFolders()
    }

    // ---- notification tap-through ---------------------------------------
    // MailController::openEmailRequested (Task 42) is forwarded from
    // NotificationDispatcher::openRequested (Task 40) via a direct
    // signal-to-signal connect in main.cpp -- see that connect's own
    // comment. Reuses selectEmail() unchanged (the exact function the mail
    // list's own TapHandler below already calls) rather than a second,
    // parallel way to set detail-pane selection state; currentSection is
    // set to "mail" first since selectEmail() alone only ever fires today
    // from within the mail list (visible only when currentSection ===
    // "mail" already), so a tap-through arriving while the Contacts section
    // is showing needs that switch made explicit here. messageId is bare
    // (no folder), so this hydrates the full cached email via
    // MailApp.findByMessageId() the same way EmailDetail.qml itself already
    // does, purely to recover the folder -- an empty folder (message not
    // yet locally cached) is not an error, see EmailDetail.qml's own
    // reload(), which already handles a miss gracefully.
    //
    // raise()/requestActivate() run unconditionally here because this
    // handler only ever fires from a genuine user click on the
    // notification's "View" action -- never from NotificationDispatcher.
    // notify() itself, which only builds/sends the KNotification and
    // touches no window state (confirmed by reading
    // NotificationDispatcher.h/.cpp and main.cpp's Task 41 wiring).
    Connections {
        target: MailApp
        function onOpenEmailRequested(messageId) {
            // Handed to C++ rather than hydrated from the cache here: a push
            // is generated the instant mail arrives and this client may not
            // have synced it yet, which used to select a message that was not
            // there and show an empty detail pane. openFromNotification()
            // refreshes first and answers on onNotificationEmailResolved.
            root.currentSection = "mail"
            MailApp.openFromNotification(messageId)
            root.raise()
            root.requestActivate()
        }
        function onNotificationEmailResolved(messageId, folder) {
            // Empty folder: not found even after a refresh. MailController
            // has set lastError, and selecting nothing leaves that visible
            // instead of covering it with an empty pane.
            if (folder.length > 0)
                root.selectEmail(messageId, folder)
        }

        // The mail list's context-menu Delete dispatches and returns, so the
        // "was it the message the detail pane is showing?" test has to live
        // somewhere that outlives the row -- the optimistic removal destroys
        // that delegate before the reply lands.
        //
        // Only the embedded pane is closed here. The popped-out Windows
        // handle their own EmailDetail's actionCompleted (see
        // emailWindowComponent), which is why this checks
        // root.selectedMessageId rather than acting on any deleted id.
        function onActionCompleted(action, messageIds, ok) {
            if (ok && action === "delete" && root.detailMode === "email"
                && messageIds.indexOf(root.selectedMessageId) !== -1) {
                root.closeDetail()
            }
        }
    }

    // ---- keyboard shortcuts ----------------------------------------------
    // Small, non-exhaustive set per the task-39 brief ("don't feel obligated
    // to replicate Mac's full shortcut set") -- refresh-current-section and
    // new-compose only. Ctrl+Shift+C is Mac's own Compose shortcut, added
    // alongside the more standard Ctrl+N rather than instead of it.
    Shortcut {
        sequence: "Ctrl+R"
        onActivated: {
            if (root.currentSection === "mail")
                MailApp.refresh()
            else
                ContactsApp.sync()
        }
    }
    Shortcut {
        sequence: "Ctrl+N"
        onActivated: root.openCompose("", "", "")
    }
    Shortcut {
        sequence: "Ctrl+Shift+C"
        onActivated: root.openCompose("", "", "")
    }

    // ---- Settings modal ----------------------------------------------
    // Kirigami.OverlaySheet, not a second ApplicationWindow -- Global
    // Constraint 4 allows either; OverlaySheet needs no second top-level
    // window/event-loop lifetime to manage, and Kirigami already renders it
    // centered over this window's content, which reads close enough to
    // Mac's separate Preferences window for this task's purposes. Settings
    // itself is a plain Item (same parent-agnostic shape as the other page
    // components) with its own "Done" button wired to close this sheet;
    // MobileRoot.qml's globalDrawer "Settings" action pushes the exact same
    // component wrapped in a Kirigami.Page instead.
    Kirigami.OverlaySheet {
        id: settingsSheet
        onOpened: settingsPane.refreshKeywordSettings()

        Settings {
            id: settingsPane
            implicitWidth: Math.min(480, root.width - 64)
            implicitHeight: Math.min(560, root.height - 64)
            onClosed: settingsSheet.close()
            onMyPgpQrCodeRequested: {
                settingsSheet.close()
                pgpMyQrCodeSheet.open()
            }
        }
    }

    // ---- pairing confirmation ------------------------------------------
    // VibeSec fix: this app is registered as the OS-wide handler for the
    // kypost:// scheme (packaging/flatpak/com.urlxl.mail.desktop's
    // MimeType), so a kypost://native-pair link clicked anywhere on
    // the system reaches PairingController without any of this app's own
    // UI ever having been on screen -- including here, where (unlike
    // MobileRoot.qml's pageStack) there's no page to auto-push. This is a
    // small dedicated Popup rather than reusing the full Pairing.qml/
    // Settings flow (Settings.qml's own pairingPopup is nested inside
    // settingsSheet, which isn't open by default): it only needs to show
    // which server is asking and let the user accept or reject before
    // Pairing.confirmPendingPair() makes any network call. See
    // PairingController::pairFromDeepLink's doc comment for the full gate.
    // Create/rename prompt. Deliberately one Popup with a mode flag: the two
    // flows differ only in the heading and which MailApp call runs.
    Popup {
        id: folderPrompt
        property string mode: "create"
        property string targetFolder: ""
        property string headingText: ""
        property alias nameText: folderPromptField.text

        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(380, root.width - 32)
        padding: 20

        background: Rectangle {
            color: Theme.panel
            radius: Theme.shapeSheet
            border.width: 1
            border.color: Theme.line
        }

        // ThemedTextField is a Rectangle wrapper, not a TextField -- focus
        // and the Return key both belong to its inner `inputField`.
        onOpened: folderPromptField.inputField.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: folderPrompt.headingText
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 14
                font.bold: true
                wrapMode: Text.WordWrap
            }

            ThemedTextField {
                id: folderPromptField
                Layout.fillWidth: true
                placeholderText: i18n("Folder name")
            }

            // ThemedTextField exposes only text/placeholderText/inputField --
            // it has no `accepted` signal of its own, so Return has to be
            // taken from the inner TextField it wraps.
            Connections {
                target: folderPromptField.inputField
                function onAccepted() { folderPrompt.submit() }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                GhostButton {
                    text: i18n("Cancel")
                    onClicked: folderPrompt.close()
                }
                PrimaryButton {
                    text: folderPrompt.mode === "create" ? i18n("Create") : i18n("Rename")
                    enabled: folderPromptField.text.trim().length > 0 && !MailApp.isBusy
                    onClicked: folderPrompt.submit()
                }
            }
        }

        function submit() {
            const name = folderPromptField.text.trim()
            if (name.length === 0)
                return
            if (folderPrompt.mode === "create")
                MailApp.createFolder(folderPrompt.targetFolder, name)
            else
                MailApp.renameFolder(folderPrompt.targetFolder, name)
        }

        // Stay open on failure so the typed name isn't lost; MailApp has
        // already put the reason in lastError, which the sidebar's error Text
        // below binds to. Only reached while this popup is open, because
        // nothing else can start a folder mutation: both dialogs are modal.
        Connections {
            target: MailApp
            enabled: folderPrompt.opened
            function onFolderMutationCompleted(ok) {
                if (ok)
                    folderPrompt.close()
            }
        }
    }

    Popup {
        id: folderDeleteConfirm
        property string targetFolder: ""
        property string displayName: ""

        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(380, root.width - 32)
        padding: 20

        background: Rectangle {
            color: Theme.panel
            radius: Theme.shapeSheet
            border.width: 1
            border.color: Theme.line
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: i18n("Delete “%1”?", folderDeleteConfirm.displayName)
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 14
                font.bold: true
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                text: i18n("The folder and any messages in it are removed from the server. This cannot be undone.")
                color: Theme.ink
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
                    onClicked: folderDeleteConfirm.close()
                }
                DangerButton {
                    text: i18n("Delete")
                    enabled: !MailApp.isBusy
                    onClicked: MailApp.deleteFolder(folderDeleteConfirm.targetFolder)
                }
            }
        }

        // Same shape as folderPrompt's above -- stay open on failure so the
        // reason in lastError is read next to the thing that failed.
        Connections {
            target: MailApp
            enabled: folderDeleteConfirm.opened
            function onFolderMutationCompleted(ok) {
                if (ok)
                    folderDeleteConfirm.close()
            }
        }
    }

    Popup {
        id: pairingConfirmPopup
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(380, root.width - 32)
        padding: 20

        background: Rectangle {
            color: Theme.panel
            radius: Theme.shapeSheet
            border.width: 1
            border.color: Theme.line
        }

        // Escape (or any other close not routed through the buttons below)
        // must fail closed -- discard the pending request rather than
        // leaving it sitting around for a later, unrelated
        // confirmPendingPair() call to unexpectedly complete. The state
        // check avoids double-cancelling when this popup is closed *because*
        // confirmPendingPair()/the Cancel button already moved
        // pairingState away from "confirm" themselves.
        onClosed: if (Pairing.pairingState === "confirm") Pairing.cancelPendingPair()

        ColumnLayout {
            width: parent.width
            spacing: 16

            Text {
                Layout.fillWidth: true
                text: i18n("Pair this device?")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 18
                font.weight: Font.Bold
            }
            Text {
                Layout.fillWidth: true
                text: i18n("A pairing request wants to connect this device to:")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 14
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: Pairing.pendingPairOrigin
                color: Theme.inkStrong
                font.family: Theme.fontMono
                font.pixelSize: 15
                font.weight: Font.Bold
                wrapMode: Text.WordWrap
            }
            // Only reachable for loopback, but cleartext must never look the
            // same as TLS in the one confirmation the user gets -- the pairing
            // token and the real push device token both travel over this.
            Text {
                Layout.fillWidth: true
                visible: Pairing.pendingPairInsecure
                text: i18n("This connection is not encrypted (http).")
                color: Theme.dangerColor
                font.family: Theme.fontUi
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Only confirm this if you expected it. Once paired, this server can deliver mail notifications to this device.")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Item { Layout.fillWidth: true }
                GhostButton {
                    text: i18n("Cancel")
                    onClicked: Pairing.cancelPendingPair()
                }
                PrimaryButton {
                    text: i18n("Pair")
                    onClicked: Pairing.confirmPendingPair()
                }
            }
        }
    }

    Connections {
        target: Pairing
        function onPairingStateChanged() {
            if (Pairing.pairingState === "confirm") {
                // Same raise()+requestActivate() as onOpenEmailRequested
                // above -- a kypost://native-pair link can arrive via
                // KDBusService while this window is minimized/behind
                // others, and Popup.open() alone doesn't bring the window
                // forward, so the confirm prompt would open invisibly and
                // the pairing attempt would silently stall on this step
                // forever.
                root.raise()
                root.requestActivate()
                pairingConfirmPopup.open()
            } else {
                pairingConfirmPopup.close()
            }
        }
    }

    // ---- PGP QR key exchange: two more OverlaySheets, same choice as
    // settingsSheet above (no second top-level window/event-loop to manage).
    Kirigami.OverlaySheet {
        id: pgpMyQrCodeSheet

        PgpMyQrCode {
            implicitWidth: 360
            implicitHeight: 480
            onClosed: pgpMyQrCodeSheet.close()
        }
    }

    Kirigami.OverlaySheet {
        id: pgpScanContactKeySheet
        // Set right before open() by whichever entry point below opened
        // this sheet -- null means "create a brand-new contact from the
        // scan" (the contacts-list entry point); non-null means "write
        // into the persistently-embedded ContactDetail pane instead" (the
        // ContactDetail pane's own entry point, unlike MobileRoot there is
        // only ever one ContactDetail instance here, not one per push, so
        // this can point at it directly rather than capturing a per-push
        // reference).
        property var targetContactDetail: null

        // Loader, not a direct child -- PgpScanContactKey's Camera defaults
        // to active as soon as it's constructed (see its own "active:
        // PgpQr.scannedFingerprint === ''" binding), so instantiating it
        // directly here would turn the camera on at DesktopRoot construction
        // time, before this sheet is ever opened. MobileRoot.qml avoids this
        // naturally since its equivalent lives inside a Component{} only
        // instantiated by pageStack.push(); OverlaySheet has no push-style
        // API, so a Loader gated on this sheet's own opened()/closed()
        // signals is the equivalent here -- also releases the camera again
        // once the sheet closes, rather than leaving it running.
        onOpened: pgpScanContactKeyLoader.active = true
        onClosed: pgpScanContactKeyLoader.active = false

        Loader {
            id: pgpScanContactKeyLoader
            active: false
            sourceComponent: PgpScanContactKey {
                implicitWidth: 360
                implicitHeight: 640
                onClosed: pgpScanContactKeySheet.close()
                onKeyScanned: function (name, publicKey) {
                    if (pgpScanContactKeySheet.targetContactDetail) {
                        pgpScanContactKeySheet.targetContactDetail.applyScannedKey(name, publicKey)
                    } else {
                        // Fold in whatever contact-card details the scanned
                        // person shared (see PgpQr.scannedContactCardFields's
                        // doc comment) -- fn/pgpKey always come from this
                        // signal's own out-of-band-confirmed values, never
                        // from the card, so they're set last and win.
                        const fields = PgpQr.scannedContactCardFields()
                        fields.fn = name
                        fields.pgpKey = publicKey
                        ContactsApp.createContact(fields)
                    }
                    pgpScanContactKeySheet.close()
                }
            }
        }
    }

    // ---- compose: Loader, not a persistent embed -----------------------
    // Unlike EmailDetail/ContactDetail below (which rebind messageId/uid as
    // the selection changes and reload() in response -- see those files'
    // own doc comments confirming DesktopRoot is expected to do this),
    // Compose.qml only seeds its fields once, in Component.onCompleted (it
    // has no onInitialToChanged-style rebind hook -- MobileRoot doesn't
    // need one either, since pageStack.push() always creates a fresh
    // instance). A persistently-embedded Compose would only ever read
    // initialTo/initialSubject/initialBody on this file's own startup, so a
    // second Reply/Forward while Compose is already the detail pane's
    // content would silently keep showing the FIRST draft's prefill. A
    // Loader with active bound to detailMode fixes this cheaply: toggling
    // active off then back on destroys and recreates the item (per Qt's
    // documented Loader sizing/lifecycle rules), which re-runs
    // Component.onCompleted every time, matching pageStack.push()'s own
    // fresh-instance semantics.
    Component {
        id: composePaneComponent
        Compose {
            initialTo: root.composeSeed.to
            initialSubject: root.composeSeed.subject
            initialBody: root.composeSeed.body
            onSendSucceeded: root.closeDetail()
            onPopOutRequested: function (to, subject, body) { root.popOutCompose(to, subject, body) }
        }
    }

    // ---- pop-out windows -------------------------------------------------
    // Email/Compose can be detached into their own top-level Window here
    // (Desktop mode only -- both components gate their own pop-out
    // IconButton on General.isDesktopMode, so these functions are only ever
    // reachable from that mode already). Popping out CLOSES the embedded
    // copy in the 3-column view (root.closeDetail() / detailMode reset)
    // rather than leaving a duplicate behind -- a message is read-only so a
    // second view of it wouldn't cause data loss, but Compose is editable,
    // and this app has exactly one way to "seed" either component's initial
    // content (the same composeSeed/messageId+folder properties used
    // in-pane), so keeping both variants of the same pop-out behavior
    // consistent was simpler than special-casing one of them.
    //
    // createObject(null, ...) makes each window a genuine top-level window
    // with no QML parent (Window itself doesn't need one); onClosing calls
    // destroy() so closing the window also frees the dynamically-created
    // object instead of leaking it.
    Component {
        id: emailWindowComponent
        Window {
            id: emailWindow
            width: 640
            height: 720
            color: Theme.bg

            property string popMessageId: ""
            property string popFolder: ""

            // Window.title is drawn by the compositor, so LockOverlay cannot
            // cover it: the subject stayed legible in the titlebar, taskbar and
            // Alt-Tab switcher while the PIN screen was up. Same reasoning as
            // NotificationDispatcher::setContentHidden(), which redacts push
            // content for exactly this reason.
            title: AppLock.locked
                ? i18n("Email")
                : ((poppedEmail.email && poppedEmail.email.subject) ? poppedEmail.email.subject : i18n("Email"))

            EmailDetail {
                id: poppedEmail
                anchors.fill: parent
                messageId: emailWindow.popMessageId
                folder: emailWindow.popFolder
                isPoppedOut: true
                onComposeRequested: function (to, subject, body) { root.openCompose(to, subject, body) }
                onActionCompleted: emailWindow.close()
            }

            // Same background as the main window, so with some window
            // managers/themes the two are hard to tell apart when they
            // overlap -- an app-drawn outline stays visible regardless of
            // decoration theme. Declared last so it paints on top of
            // EmailDetail's content instead of underneath it.
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 2
                border.color: Theme.line
            }

            // A popped-out window is a separate top-level Window, so the
            // main window's overlay does not cover it. Without this one, mail
            // stayed readable here while the app was locked.
            LockOverlay {}

            onClosing: emailWindow.destroy()
        }
    }

    Component {
        id: composeWindowComponent
        Window {
            id: composeWindow
            width: 640
            height: 720
            color: Theme.bg
            title: i18n("Compose")

            property string popInitialTo: ""
            property string popInitialSubject: ""
            property string popInitialBody: ""

            Compose {
                anchors.fill: parent
                initialTo: composeWindow.popInitialTo
                initialSubject: composeWindow.popInitialSubject
                initialBody: composeWindow.popInitialBody
                initialBodyIsHtml: true
                isPoppedOut: true
                onSendSucceeded: composeWindow.close()
                // No MailApp.sendWarning sink here: this Window closes on
                // sendSucceeded(), so any notice parented to it would be
                // destroyed in the same turn it appeared. The main window owns
                // that sink -- see the "send warnings" block near the bottom of
                // this file for the full reasoning and its one limitation.
            }

            // See the matching Rectangle in emailWindowComponent above --
            // same pop-out-blends-into-main-window issue, same fix.
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 2
                border.color: Theme.line
            }

            // A popped-out window is a separate top-level Window, so the
            // main window's overlay does not cover it. Without this one, mail
            // stayed readable here while the app was locked.
            LockOverlay {}

            onClosing: composeWindow.destroy()
        }
    }

    Component {
        id: contactWindowComponent
        Window {
            id: contactWindow
            width: 640
            height: 720
            color: Theme.bg

            property string popUid: ""

            title: AppLock.locked
                ? i18n("Contact")
                : ((poppedContact.contact && poppedContact.contact.fn) ? poppedContact.contact.fn : i18n("Contact"))

            ContactDetail {
                id: poppedContact
                anchors.fill: parent
                uid: contactWindow.popUid
                isPoppedOut: true
                onClosed: contactWindow.close()
                onSaved: function (uid) { ContactsApp.load() }
                onScanPgpKeyRequested: {
                    pgpScanContactKeySheet.targetContactDetail = poppedContact
                    pgpScanContactKeySheet.open()
                }
            }

            // Same background as the main window -- see the matching
            // Rectangle in emailWindowComponent above for why this outline
            // is needed.
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 2
                border.color: Theme.line
            }

            // A popped-out window is a separate top-level Window, so the
            // main window's overlay does not cover it. Without this one, mail
            // stayed readable here while the app was locked.
            LockOverlay {}

            onClosing: contactWindow.destroy()
        }
    }

    function popOutEmail(messageId, folder) {
        const win = emailWindowComponent.createObject(null, { popMessageId: messageId, popFolder: folder })
        win.show()
        // Same raise()+requestActivate() pairing as the tray restore
        // (TrayController.cpp) and notification tap-through (Connections
        // block above) -- without it, a freshly created top-level window
        // can end up behind root and never take focus, so show() alone
        // silently produces a window the user never sees.
        win.raise()
        win.requestActivate()
        root.closeDetail()
    }

    function popOutCompose(to, subject, body) {
        const win = composeWindowComponent.createObject(
            null, { popInitialTo: to, popInitialSubject: subject, popInitialBody: body })
        win.show()
        win.raise()
        win.requestActivate()
        root.detailMode = "empty"
    }

    function popOutContact(uid) {
        const win = contactWindowComponent.createObject(null, { popUid: uid })
        win.show()
        win.raise()
        win.requestActivate()
        root.closeDetail()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- top bar -----------------------------------------------------
        // A single bottom-edge hairline (not a full 4-sided border, which
        // reads flat/boxy against the OS window frame on the other 3 edges)
        // plus a barely-there vertical gradient give this a subtle "raised
        // bar" separation from the 3-column body below.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52

            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.panel }
                GradientStop { position: 1.0; color: Qt.darker(Theme.panel, 1.03) }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.line
            }

            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    text: "KyPost" // product name -- not translated, see ApplicationWindow.title above
                    color: Theme.inkStrong
                    font.family: Theme.fontUi
                    font.pixelSize: 16
                    font.weight: Font.Bold
                }
                GhostButton {
                    text: root.detailCollapsed ? i18n("Show Detail") : i18n("Hide Detail")
                    onClicked: root.detailCollapsed = !root.detailCollapsed
                }
                PrimaryButton {
                    text: i18n("Compose")
                    onClicked: root.openCompose("", "", "")
                }
                GhostButton {
                    text: i18n("Settings")
                    onClicked: settingsSheet.open()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ---- sidebar ------------------------------------------------
            // Mac's exact order: Inbox, then the 4 fixed interior folders
            // (Drafts, Junk, Sent, Trash), then Archive last, under a "Mail"
            // section; Contacts under a "People" section below it.
            // MailApp.mailFolders() already returns exactly this order (see
            // MailController::mailFolders()'s kFolders array), so this just
            // renders it as-is -- no reordering needed here.
            //
            // Server subfolders are nested under their parent by
            // mailFolders() and indented here by `depth`; this comment
            // previously said no folder listing was wired up, which stopped
            // being true when FolderRepository landed.
            Rectangle {
                Layout.preferredWidth: 200
                Layout.minimumWidth: 200
                Layout.fillHeight: true
                color: Theme.panel

                // Single hairline at the seam with the list column, not a
                // full 4-sided border (which would double up against that
                // column's own left edge).
                Rectangle {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 1
                    color: Theme.line
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                    SectionLabel { text: i18n("Mail") }

                    Repeater {
                        id: folderRepeater
                        // Server subfolders (Archive/...) are nested under
                        // their parent by mailFolders(); re-read whenever the
                        // folder set changes.
                        model: MailApp.mailFolders()

                        Connections {
                            target: MailApp
                            function onFoldersChanged() {
                                folderRepeater.model = MailApp.mailFolders()
                            }
                        }

                        delegate: Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: folderLabel.implicitHeight + 16
                            radius: Theme.shapeButton
                            readonly property bool isCurrent: root.currentSection === "mail"
                                && root.currentFolder === modelData.wireName
                            color: isCurrent ? Theme.accentSoft
                                : (folderTap.pressed ? Theme.bg : (folderHover.hovered ? Theme.panel : "transparent"))

                            Behavior on color {
                                ColorAnimation { duration: 120 }
                            }

                            Rectangle {
                                visible: parent.isCurrent
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: 3
                                color: Theme.accent
                            }

                            Text {
                                id: folderLabel
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 10 + (modelData.depth || 0) * 14
                                textFormat: Text.PlainText
                                text: modelData.displayName
                                color: Theme.inkStrong
                                font.family: Theme.fontUi
                                // Subfolders read as children of the mailbox
                                // above them, not as peers.
                                font.pixelSize: modelData.depth > 0 ? 13 : 14
                            }

                            HoverHandler {
                                id: folderHover
                            }

                            TapHandler {
                                id: folderTap
                                onTapped: root.selectFolder(modelData.wireName)
                            }

                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: folderMenu.popup()
                            }

                            Menu {
                                id: folderMenu

                                MenuItem {
                                    text: i18n("New Subfolder…")
                                    onTriggered: root.promptCreateFolder(modelData.wireName)
                                }
                                MenuItem {
                                    // deletable is the server's verdict
                                    // (built-in mailboxes and top-level
                                    // folders are refused), so don't offer
                                    // what the backend will reject.
                                    text: i18n("Rename…")
                                    enabled: modelData.deletable === true
                                    onTriggered: root.promptRenameFolder(modelData.wireName, modelData.displayName)
                                }
                                MenuItem {
                                    text: i18n("Delete Folder…")
                                    enabled: modelData.deletable === true
                                    onTriggered: root.promptDeleteFolder(modelData.wireName, modelData.displayName)
                                }
                            }
                        }
                    }

                    Item { Layout.preferredHeight: 12 }

                    SectionLabel { text: i18n("People") }

                    Rectangle {
                        id: contactsSectionRow
                        Layout.fillWidth: true
                        implicitHeight: contactsLabel.implicitHeight + 16
                        radius: Theme.shapeButton
                        readonly property bool isCurrent: root.currentSection === "contacts"
                        color: isCurrent ? Theme.accentSoft
                            : (contactsTap.pressed ? Theme.bg : (contactsHover.hovered ? Theme.panel : "transparent"))

                        Behavior on color {
                            ColorAnimation { duration: 120 }
                        }

                        Rectangle {
                            visible: contactsSectionRow.isCurrent
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            color: Theme.accent
                        }

                        Text {
                            id: contactsLabel
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            text: i18n("Contacts")
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                        }

                        HoverHandler {
                            id: contactsHover
                        }

                        TapHandler {
                            id: contactsTap
                            onTapped: root.selectContactsSection()
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- list column --------------------------------------------
            Rectangle {
                Layout.fillWidth: root.detailCollapsed
                Layout.preferredWidth: 340
                Layout.minimumWidth: 340
                Layout.fillHeight: true
                color: Theme.bg

                // Single hairline at the seam with the detail column (the
                // sidebar/list seam is already drawn by the sidebar's own
                // right-edge hairline above, so no left border here).
                Rectangle {
                    visible: !root.detailCollapsed
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 1
                    color: Theme.line
                }

                // ---- mail section: own ListView (no swipe actions -- a
                // right-click context menu with Delete replaces them per
                // the task-39 brief's explicit "reasonable minimum" call).
                // Same delegate content as MobileRoot's Inbox rows (Task
                // 38), minus swipe -- kept as a local duplicate rather than
                // a shared delegate component: the two hosts' interaction
                // models (swipe vs. select+right-click) differ enough that
                // a shared delegate would need its own mode-switching logic,
                // which isn't worth it for one delegate this size.
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8
                    visible: root.currentSection === "mail"

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            Layout.fillWidth: true
                            text: root.folderDisplayName(root.currentFolder)
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 16
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }
                        BusyIndicator {
                            running: MailApp.isBusy
                            visible: MailApp.isBusy
                            implicitWidth: 24
                            implicitHeight: 24
                        }
                        GhostButton {
                            text: i18n("Refresh")
                            enabled: !MailApp.isBusy
                            onClicked: MailApp.refresh()
                        }
                    }

                    // ---- keyword pill row ---------------------------------
                    // Ported from MobileRoot.qml's keyword pill row: "All" is
                    // hardcoded first and always present (not part of
                    // MailApp.keywordTabs), the rest come straight from
                    // keywordTabs. Horizontally-scrolling Flickable rather
                    // than a wrapping Flow so it reads as a single scannable
                    // filter row above the list, not a block that grows the
                    // header as keywords accumulate.
                    Flickable {
                        Layout.fillWidth: true
                        // +10 reserves dedicated space below the pills for
                        // the horizontal scrollbar thumb (4px thick + a
                        // little breathing room) -- an overlay scrollbar
                        // here would sit on top of the pills themselves,
                        // which is fine for a vertical list's right edge but
                        // not for a row of tappable pills.
                        implicitHeight: keywordRow.implicitHeight + 10
                        contentWidth: keywordRow.implicitWidth
                        contentHeight: keywordRow.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.HorizontalFlick
                        ScrollBar.horizontal: ThemedScrollBar {}

                        Row {
                            id: keywordRow
                            spacing: 8

                            PillTab {
                                text: i18n("All")
                                selected: MailApp.selectedKeyword === ""
                                onClicked: MailApp.selectKeyword("")
                            }
                            Repeater {
                                model: MailApp.keywordTabs
                                delegate: PillTab {
                                    text: i18n("%1 (%2)", modelData.name, modelData.count)
                                    selected: MailApp.selectedKeyword === modelData.name
                                    onClicked: MailApp.selectKeyword(modelData.name)
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: MailApp.lastError !== ""
                        textFormat: Text.PlainText
                        text: MailApp.lastError
                        color: Theme.dangerColor
                        font.family: Theme.fontUi
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }

                    EmptyState {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: emailListView.count === 0
                        text: i18n("No emails yet — tap Refresh.")
                    }

                    ListView {
                        id: emailListView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: count > 0
                        clip: true
                        spacing: 4
                        model: MailApp.emailModel
                        ScrollBar.vertical: ThemedScrollBar {}

                        delegate: Rectangle {
                            id: emailRow
                            width: emailListView.width
                            implicitHeight: emailRowContent.implicitHeight + 16
                            radius: Theme.shapeButton
                            readonly property bool isSelected: root.detailMode === "email"
                                && root.selectedMessageId === model.messageId
                            color: isSelected ? Theme.accentSoft
                                : (emailTap.pressed ? Theme.panel : (emailHover.hovered ? Theme.bg : "transparent"))

                            Behavior on color {
                                ColorAnimation { duration: 120 }
                            }

                            Rectangle {
                                visible: emailRow.isSelected
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: 3
                                color: Theme.accent
                            }

                            HoverHandler {
                                id: emailHover
                            }

                            RowLayout {
                                id: emailRowContent
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: 10
                                spacing: 10

                                Avatar {
                                    initials: emailRow.initialsForSender(model.sender)
                                    size: 32
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Text {
                                        Layout.fillWidth: true
                                        textFormat: Text.PlainText
                                        text: model.sender
                                        color: Theme.inkStrong
                                        font.family: Theme.fontUi
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }
                                    // Subject preceded by the OpenPGP marker, when there is
                                    // one. Both the glyph and its spoken equivalent come from
                                    // EmailListModel roles (app/mail/PgpMessagePresentation.h);
                                    // only ClientProtected and DecryptFailed are marked, so
                                    // most rows show nothing here.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6

                                        Text {
                                            visible: !!model.pgpMarker
                                            textFormat: Text.PlainText
                                            text: model.pgpMarker || ""
                                            color: Theme.inkStrong
                                            font.family: Theme.fontUi
                                            font.pixelSize: 13
                                            // The glyph alone is announced inconsistently (or
                                            // not at all) across AT stacks, so carry the words.
                                            Accessible.role: Accessible.StaticText
                                            Accessible.name: model.pgpMarkerAccessibleName || ""
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            textFormat: Text.PlainText
                                            text: model.subject
                                            color: Theme.inkStrong
                                            font.family: Theme.fontUi
                                            font.pixelSize: 13
                                            elide: Text.ElideRight
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        textFormat: Text.PlainText
                                        text: model.preview
                                        color: Theme.ink
                                        font.family: Theme.fontUi
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                    }
                                }
                            }

                            // Same "reasonable initials logic" shape as
                            // EmailDetail.qml/MobileRoot.qml's own copies --
                            // the whitespace-split-to-2-initials core is
                            // shared (Format.initialsFromNamePart()), same
                            // as MobileRoot.qml's initialsForSender().
                            function initialsForSender(sender) {
                                const s = sender || ""
                                const lt = s.indexOf("<")
                                const namePart = (lt !== -1 ? s.substring(0, lt) : s).trim()
                                const initials = Format.initialsFromNamePart(namePart)
                                return initials.length > 0 ? initials : "?"
                            }

                            TapHandler {
                                id: emailTap
                                acceptedButtons: Qt.LeftButton
                                onTapped: root.selectEmail(model.messageId, model.folder)
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: emailContextMenu.popup()
                            }

                            Menu {
                                id: emailContextMenu
                                MenuItem {
                                    text: i18n("Delete")
                                    // Fire and forget -- the detail pane is
                                    // closed by root's own onActionCompleted
                                    // handler if the deleted message was the
                                    // one it was showing. Doing it here
                                    // would need this delegate to still be
                                    // alive when the reply lands, and the
                                    // optimistic row removal is exactly what
                                    // destroys it.
                                    onTriggered: MailApp.deleteEmails([model.messageId])
                                }
                            }
                        }
                    }
                }

                // ---- contacts section: ContactsList.qml embedded as-is --
                // Reuses Task 36's component directly (same row content,
                // same Add/Sync header) rather than duplicating its
                // delegate -- its layout works fine anchored into this
                // column, per the task-39 brief's explicit "reuse it
                // directly if its layout works embedded" call.
                ContactsList {
                    anchors.fill: parent
                    visible: root.currentSection === "contacts"
                    onContactSelected: function (uid) { root.selectContact(uid) }
                    onScanPgpKeyRequested: {
                        pgpScanContactKeySheet.targetContactDetail = null
                        pgpScanContactKeySheet.open()
                    }
                }
            }

            // ---- detail column (collapsible) -----------------------------
            // Grows to absorb any window width beyond the sidebar+list's own
            // fixed needs (fillWidth true whenever visible) -- previously
            // neither this column nor the list column claimed the extra
            // space when the window was widened past its default size, so
            // it just sat there as unused background. The reading/composing
            // pane is the one that actually benefits from more width (more
            // comfortable line length for email bodies and the compose
            // body), same as most 3-pane mail clients growing their message
            // pane rather than the folder/message list columns.
            Rectangle {
                Layout.preferredWidth: 420
                Layout.fillWidth: !root.detailCollapsed
                Layout.fillHeight: true
                Layout.minimumWidth: root.detailCollapsed ? 0 : 360
                Layout.maximumWidth: root.detailCollapsed ? 0 : 4096
                visible: !root.detailCollapsed
                clip: true
                color: Theme.bg

                // Seam with the list column is already drawn by that
                // column's own right-edge hairline; this is the rightmost
                // column (flush against the window edge), so no border here.

                EmptyState {
                    anchors.centerIn: parent
                    visible: root.detailMode === "empty"
                    text: root.currentSection === "mail" ? i18n("Select an email") : i18n("Select a contact")
                }

                // EmailDetail/ContactDetail are embedded directly and kept
                // alive across selection changes -- both already rebind off
                // messageId/uid changes and reload() accordingly (see their
                // own doc comments confirming this is the DesktopRoot
                // pattern they were built for), unlike Compose above.
                EmailDetail {
                    anchors.fill: parent
                    visible: root.detailMode === "email"
                    messageId: root.detailMode === "email" ? root.selectedMessageId : ""
                    folder: root.detailMode === "email" ? root.selectedEmailFolder : ""
                    onComposeRequested: function (to, subject, body) { root.openCompose(to, subject, body) }
                    onActionCompleted: root.closeDetail()
                    onPopOutRequested: root.popOutEmail(root.selectedMessageId, root.selectedEmailFolder)
                }

                ContactDetail {
                    id: contactDetailPane
                    anchors.fill: parent
                    visible: root.detailMode === "contact"
                    uid: root.detailMode === "contact" ? root.selectedContactUid : ""
                    onClosed: root.closeDetail()
                    onSaved: function (uid) { ContactsApp.load() }
                    onScanPgpKeyRequested: {
                        pgpScanContactKeySheet.targetContactDetail = contactDetailPane
                        pgpScanContactKeySheet.open()
                    }
                    onPopOutRequested: function (uid) { root.popOutContact(uid) }
                }

                Loader {
                    anchors.fill: parent
                    visible: active
                    active: root.detailMode === "compose"
                    sourceComponent: composePaneComponent
                }
            }
        }
    }
    // ---- send warnings ---------------------------------------------------
    // MailApp.sendWarning means the message WAS sent, with partial trouble
    // (the Sent copy failed, or a pickup link did not reach every recipient).
    // Shown as a notice, never as a failure, and never with a retry that would
    // duplicate it.
    //
    // The sink lives HERE rather than in Compose.qml because a warning only
    // ever accompanies a successful send, and this file answers Compose's
    // sendSucceeded() by closing the detail pane (destroying the composer) --
    // a notice parented to it would be torn down in the same turn it appeared.
    // MailApp is a singleton, so this receives the warning whichever composer
    // sent.
    //
    // Known limitation, stated rather than papered over: this Toast is a child
    // of THIS window, so it cannot render over the pop-out compose Window
    // (composeWindowComponent above), which is a separate top-level window. A
    // second Toast inside that Window would not help -- the pop-out closes
    // itself on sendSucceeded(), in the same turn the warning arrives, which
    // is the exact "doomed component" problem that moved this sink out of
    // Compose.qml in the first place. So for a pop-out send the notice appears
    // here, on the main window, which the user is returned to as the pop-out
    // disappears. It is missed only if the main window is minimized or fully
    // occluded for the Toast's 2.5s lifetime; fixing that properly needs a
    // persistent, window-independent notification surface this app does not
    // have yet.
    Connections {
        target: MailApp
        function onSendWarning(warning) {
            sendWarningToast.show(warning)
        }
    }

    Toast {
        id: sendWarningToast
        z: 900 // under the app-lock overlay (1000), over every pane
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        anchors.horizontalCenter: parent.horizontalCenter
    }

    // Persistent warnings for an expired pairing / undecryptable credentials
    // -- see components/StatusBanner.qml. Declared before the lock overlay so
    // it paints underneath it: a locked app must not leak account state.
    StatusBanner {
        onCertificateReviewRequested: certificateChangeDialog.open()
    }

    // Hosted here rather than inside StatusBanner: it fills the window, and
    // the banner is only as tall as its own text. Declared before the lock
    // overlay for the same reason the banner is -- it is an Item, not a
    // Popup, so z-order alone keeps it under the lock gate.
    CertificateChangeDialog {
        id: certificateChangeDialog
        onReconnectRequested: Pairing.reconnectToServer()
    }

    // ---- app lock -----------------------------------------------------
    // One per Window, including every pop-out below -- see
    // components/LockOverlay.qml for why this cannot live only here.
    // Every QQC2 Popup and Kirigami.OverlaySheet renders inside QQuickOverlay,
    // which Qt stacks ABOVE the window's content item -- so LockOverlay's
    // z: 1000 cannot reach it, and a sheet left open when the app locked
    // stayed visible and clickable over the PIN screen. Because these popups
    // are modal, their dimmer also swallowed clicks aimed at the PIN field, so
    // they blocked unlocking as well as sitting on top of it.
    //
    // Only root-scope ids are reachable here; menus declared inside delegates
    // are transient and close on their own.
    Connections {
        target: AppLock
        function onLockedChanged() {
            if (!AppLock.locked)
                return;
            settingsSheet.close();
            pgpMyQrCodeSheet.close();
            pgpScanContactKeySheet.close();
            folderPrompt.close();
            folderDeleteConfirm.close();
        }
    }

    LockOverlay { id: unlockOverlay }
}
