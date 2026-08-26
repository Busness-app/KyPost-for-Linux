import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import com.kysecurity.mail 1.0
import "components"
import "pages"
import "utils/format.js" as Format

// Task 38 -- top-level mobile navigation shell, replacing the Task 1 stub.
// Kirigami.ApplicationWindow + pageStack (Kirigami.PageRow, single-column
// at this window's size) per Phase 6 global constraint 4: Inbox is
// pageStack.initialPage; EmailDetail/Compose/ContactsList/ContactDetail/
// Pairing (Tasks 35-37's plain-Item page components) are pushed wrapped in
// a thin Kirigami.Page shell -- those files are deliberately NOT
// Kirigami.Page themselves so DesktopRoot (Task 39) can embed them
// directly instead.
Kirigami.ApplicationWindow {
    id: root
    width: 360
    height: 640
    visible: true
    title: "KyPost" // product name -- not translated, same as Firefox/Thunderbird's own names

    // ---- bottom tab bar selection state --------------------------------
    // "inbox" | "contacts" -- Compose is action-only and deliberately never
    // written here (see footer below / task-38 brief's exact semantics).
    property string activeTab: "inbox"

    function goInbox() {
        // Re-tapping Inbox while its root page is already showing
        // re-triggers a refresh (brief's exact spec); tapping it from
        // anywhere else (EmailDetail/Contacts/ContactDetail/Compose/
        // Pairing) just pops back to the Inbox root first. That "always
        // return to this section's root" half is this file's own
        // extension of the brief's narrower "re-tap while already there"
        // rule to the general pushed-page case -- standard bottom-tab-bar
        // behavior, not contradicted by anything in the brief.
        const alreadyAtInboxRoot = root.activeTab === "inbox" && root.pageStack.depth <= 1
        while (root.pageStack.depth > 1)
            root.pageStack.pop()
        root.activeTab = "inbox"
        if (alreadyAtInboxRoot)
            MailApp.refresh()
    }

    // Guards every pop() triggered by a *pushed page's own signal*
    // (actionCompleted/sendSucceeded/closed below) against firing when
    // depth is already 1. Confirmed by hand during this task's manual
    // verification that calling pageStack.pop() while only the Inbox root
    // page remains doesn't just harmlessly no-op -- it can tear down the
    // root page's own children (observed as the Inbox ListView/its
    // delegates' bindings throwing "Cannot read property of null"
    // immediately afterward, i.e. a real, reproducible corruption, not a
    // hypothetical). None of these 4 call sites *should* ever fire at
    // depth 1 in a single normal interaction, but a double-fired signal
    // (e.g. a fast double-tap on a page's own Delete/Send/Close/Done
    // button racing the pop that's already in flight) is exactly the kind
    // of double-fire this task was asked to scrutinize -- this guard
    // closes that window cheaply, independent of whatever guarding (or
    // lack of it) the hosted page component itself does internally.
    function safePop() {
        if (root.pageStack.depth > 1)
            root.pageStack.pop()
    }

    // Task 42: shared by the Inbox ListView's own row-tap (below) and the
    // notification tap-through Connections block further down -- "find and
    // reuse that exact push logic, don't duplicate it" per the task-42
    // brief. Pops back to the Inbox root and re-selects the Inbox tab
    // first: a plain in-app row tap always fires from pageStack depth 1
    // already (the email rows only exist inside inboxPage), so that reset
    // is a no-op for that call site, but notification tap-through can land
    // while the user is anywhere else in the stack (Contacts, a pushed
    // ContactDetail/Compose/Pairing/Settings page) and needs the same
    // "return to a clean Inbox-rooted stack" behavior goInbox()/goContacts()
    // already established elsewhere in this file.
    function openEmailDetail(messageId, folder) {
        while (root.pageStack.depth > 1)
            root.pageStack.pop()
        root.activeTab = "inbox"
        root.pageStack.push(emailDetailPageComponent, { messageId: messageId, folder: folder })
    }

    // ---- notification tap-through ---------------------------------------
    // MailController::openEmailRequested (Task 42) is forwarded from
    // NotificationDispatcher::openRequested (Task 40) via a direct
    // signal-to-signal connect in main.cpp -- see that connect's own
    // comment. messageId is bare (no folder), so this hydrates the full
    // cached email via MailApp.findByMessageId() the same way
    // EmailDetail.qml itself already does, purely to recover the folder
    // for the push() below (EmailDetail re-fetches by messageId on its own
    // reload() regardless). An empty folder (message not yet locally
    // cached -- e.g. a push arrived for mail this client hasn't synced yet)
    // is not an error here: EmailDetail.qml's own reload() already handles
    // an empty/miss findByMessageId() result gracefully (renders blank
    // until the user next refreshes), same as it does for any other
    // not-yet-cached messageId.
    //
    // raise()/requestActivate() (Window's own base-type methods, per
    // Kirigami.ApplicationWindow extending QtQuick's Window) run
    // unconditionally here because this handler only ever fires from a
    // genuine user click on the notification's "View" action -- never from
    // NotificationDispatcher.notify() itself, which only builds/sends the
    // KNotification and touches no window state (confirmed by reading
    // NotificationDispatcher.h/.cpp and main.cpp's Task 41 wiring).
    Connections {
        target: MailApp
        function onOpenEmailRequested(messageId) {
            // Hand it to C++ rather than hydrating from the cache here. A
            // push is generated the instant mail arrives and this client may
            // not have synced it yet, which used to mean a blank detail page;
            // openFromNotification() refreshes first and answers on
            // onNotificationEmailResolved below.
            MailApp.openFromNotification(messageId)
            root.raise()
            root.requestActivate()
        }
        // Cached in several mailboxes and the notification did not say which.
        // Only the user can answer this, so ask instead of rendering the
        // refusal as a blank detail page.
        function onNotificationEmailAmbiguous(messageId, folders) {
            root.currentSection = "mail"
            notificationFolderDialog.open(messageId, folders)
        }

        function onNotificationEmailResolved(messageId, folder) {
            // An empty folder means it could not be found even after a
            // refresh. MailController has already set lastError; pushing a
            // blank detail page on top of that would just hide it.
            if (folder.length > 0)
                root.openEmailDetail(messageId, folder)
        }
    }

    function goContacts() {
        if (root.activeTab === "contacts") {
            // Already in the Contacts section -- pop back to its root
            // (ContactsList) instead of pushing a second copy.
            while (root.pageStack.depth > 2)
                root.pageStack.pop()
            return
        }
        while (root.pageStack.depth > 1)
            root.pageStack.pop()
        root.activeTab = "contacts"
        root.pageStack.push(contactsListPageComponent)
    }

    // ---- deep-link auto-navigate to Pairing.qml ------------------------
    // main.cpp's routeDeepLink() -> Pairing.pairFromDeepLink() (Task 34)
    // drives Pairing.pairingState away from "idle" whenever a real
    // kypost://native-pair link arrives, whether or not this window
    // is already up. autoNavigatedForAttempt guards against
    // double-pushing Pairing.qml: pairingStateChanged can fire more than
    // once per attempt ("confirm" -> "working" -> "paired"/"failed" each
    // emit it), so only the *first* transition of a given attempt pushes.
    //
    // The flag rearms on a transition INTO "confirm" (VibeSec fix: every
    // real attempt now starts there -- see PairingController::
    // pairFromDeepLink's doc comment -- rather than "working", which a
    // pending attempt only reaches once the user explicitly confirms it),
    // not on a return to "idle": per PairingController.cpp, a fresh
    // pairFromDeepLink()/pairFromPastedLink() call sets pairingState to
    // "confirm" unconditionally from whatever state it was already in -- it
    // never passes back through "idle" first. The ONLY paths that ever set
    // pairingState back to "idle" are reset() and cancelPendingPair(),
    // whose only callers are Pairing.qml's own "Try Again" and "Cancel"
    // buttons. So rearming on "idle" meant this guard only ever fired for
    // the FIRST pairing attempt of a session (or one right after a manual
    // "Try Again"/"Cancel"); every subsequent real attempt updated
    // pairingState correctly but had its auto-navigate silently swallowed.
    // Rearming on "confirm" fires exactly once per real attempt, regardless
    // of what state preceded it.
    //
    // Also checked against the currently active page's objectName so a
    // user who has already manually navigated to Pairing.qml (via the
    // global drawer) doesn't get a second copy pushed on top of the one
    // they're already looking at.
    property bool autoNavigatedForAttempt: false

    Connections {
        target: Pairing
        function onPairingStateChanged() {
            if (Pairing.pairingState === "confirm")
                root.autoNavigatedForAttempt = false
            if (Pairing.pairingState === "idle" || root.autoNavigatedForAttempt)
                return
            root.autoNavigatedForAttempt = true
            // Same raise()+requestActivate() as onOpenEmailRequested above
            // -- a kypost://native-pair link can arrive via KDBusService
            // while this window is minimized/behind others, and pushing
            // pairingPageComponent alone doesn't bring the window forward,
            // so the confirm page would open invisibly and the pairing
            // attempt would silently stall on this step forever.
            root.raise()
            root.requestActivate()
            if (!root.pageStack.currentItem || root.pageStack.currentItem.objectName !== "pairingPage")
                root.pageStack.push(pairingPageComponent)
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
    // sendSucceeded() by popping (destroying) it -- a notice parented to the
    // composer would be torn down in the same turn it appeared. MailApp is a
    // singleton, so this receives the warning whichever composer sent.
    Connections {
        target: MailApp
        function onSendWarning(warning) {
            sendWarningToast.show(warning)
        }
    }

    Toast {
        id: sendWarningToast
        z: 900 // under the app-lock overlay (1000), over every page
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        anchors.horizontalCenter: parent.horizontalCenter
    }

    // ---- pushed-page wrappers -------------------------------------------
    // Each Component wraps one of Tasks 35-37's plain-Item page components
    // in a thin Kirigami.Page shell (Phase 6 global constraint 4), pushed
    // via pageStack.push(component, { ...initial property values... }).
    // Those properties apply to the Component's root object -- the
    // Kirigami.Page wrapper -- which forwards them into the hosted Item;
    // this indirection is the only way to seed initial property values on
    // a push without the wrapped Item itself needing to know about
    // push-navigation.

    Component {
        id: emailDetailPageComponent
        Kirigami.Page {
            id: emailDetailPage
            objectName: "emailDetailPage"
            title: i18nc("page title for viewing a single email message", "Email")
            property string messageId: ""
            property string folder: ""

            EmailDetail {
                anchors.fill: parent
                messageId: emailDetailPage.messageId
                folder: emailDetailPage.folder

                onComposeRequested: function (to, subject, body) {
                    root.pageStack.push(composePageComponent,
                        { initialTo: to, initialSubject: subject, initialBody: body })
                }
                // Archive/Junk/Delete have already happened by the time
                // this fires -- EmailDetail.qml only emits actionCompleted
                // after its own MailApp.* call reports success (see its
                // Archive/Junk/Delete onClicked handlers) -- so this just
                // returns to whatever pushed this page.
                onActionCompleted: root.safePop()
            }
        }
    }

    Component {
        id: composePageComponent
        Kirigami.Page {
            id: composePage
            objectName: "composePage"
            title: i18n("Compose")
            property string initialTo: ""
            property string initialSubject: ""
            property string initialBody: ""

            Compose {
                anchors.fill: parent
                initialTo: composePage.initialTo
                initialSubject: composePage.initialSubject
                initialBody: composePage.initialBody
                onSendSucceeded: root.safePop()
            }
        }
    }

    Component {
        id: contactsListPageComponent
        Kirigami.Page {
            id: contactsListPage
            objectName: "contactsListPage"
            title: i18n("Contacts")

            ContactsList {
                anchors.fill: parent
                onContactSelected: function (uid) {
                    root.pageStack.push(contactDetailPageComponent, { uid: uid })
                }
                // PGP QR key exchange: no existing form to write into here
                // (this is the contacts list, not a specific contact) --
                // pgpScanContactKeyPageComponent's targetContactDetail
                // defaults to null, which its onKeyScanned handler treats
                // as "create a brand-new contact from the scan".
                onScanPgpKeyRequested: root.pageStack.push(pgpScanContactKeyPageComponent, {})
            }
        }
    }

    Component {
        id: contactDetailPageComponent
        Kirigami.Page {
            id: contactDetailPage
            objectName: "contactDetailPage"
            title: i18n("Contact")
            property string uid: ""

            ContactDetail {
                id: contactDetailInner
                anchors.fill: parent
                uid: contactDetailPage.uid
                onClosed: root.safePop()
                // PGP QR key exchange: captures this still-open form
                // instance so the pushed scan page can write the scanned
                // key straight into it once confirmed (see
                // pgpScanContactKeyPageComponent's onKeyScanned below) --
                // nothing is persisted until the user hits this form's own
                // Save button.
                onScanPgpKeyRequested: root.pageStack.push(
                    pgpScanContactKeyPageComponent, { targetContactDetail: contactDetailInner })
            }
        }
    }

    Component {
        id: pgpMyQrCodePageComponent
        Kirigami.Page {
            objectName: "pgpMyQrCodePage"
            title: i18n("My PGP QR Code")

            PgpMyQrCode {
                anchors.fill: parent
                onClosed: root.safePop()
            }
        }
    }

    Component {
        id: pgpScanContactKeyPageComponent
        Kirigami.Page {
            id: pgpScanContactKeyPage
            objectName: "pgpScanContactKeyPage"
            title: i18n("Scan Contact Key")
            // Set by whichever entry point pushed this page -- null means
            // "no form to write into, create a brand-new contact instead"
            // (ContactsList's entry point); non-null means "write into this
            // still-open ContactDetail form" (ContactDetail's own entry
            // point, for filling in the key of the contact already being
            // edited/created).
            property var targetContactDetail: null

            PgpScanContactKey {
                anchors.fill: parent
                onClosed: root.safePop()
                onKeyScanned: function (name, publicKey) {
                    if (pgpScanContactKeyPage.targetContactDetail) {
                        pgpScanContactKeyPage.targetContactDetail.applyScannedKey(name, publicKey)
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
                    root.safePop()
                }
            }
        }
    }

    Component {
        id: pairingPageComponent
        Kirigami.Page {
            id: pairingPage
            objectName: "pairingPage"
            title: i18n("Pair Device")

            Pairing {
                anchors.fill: parent
                onClosed: root.safePop()
            }
        }
    }

    // Task 39: Settings.qml landed -- wraps it in the same thin
    // Kirigami.Page shell pattern as every other pushed page above. Settings
    // itself emits closed() from its own "Done" button (same signal shape
    // as Pairing.qml/ContactDetail.qml above), which just pops back here,
    // same as those.
    Component {
        id: settingsPageComponent
        Kirigami.Page {
            id: settingsPage
            objectName: "settingsPage"
            title: i18n("Settings")

            Settings {
                anchors.fill: parent
                onClosed: root.safePop()
                onMyPgpQrCodeRequested: root.pageStack.push(pgpMyQrCodePageComponent)
            }
        }
    }

    // ---- global drawer (hamburger menu): Settings + Pair Device --------
    globalDrawer: Kirigami.GlobalDrawer {
        title: "KyPost" // product name -- not translated, see ApplicationWindow.title above
        actions: [
            Kirigami.Action {
                text: i18n("Pair Device")
                onTriggered: root.pageStack.push(pairingPageComponent)
            },
            Kirigami.Action {
                text: i18n("Settings")
                onTriggered: root.pageStack.push(settingsPageComponent)
            }
        ]
    }

    // ---- bottom Inbox / Compose / Contacts bar --------------------------
    // Judgment call: 3 PillTabs in a RowLayout, NOT Kirigami.NavigationTabBar.
    // NavigationTabBar's own delegate (org/kde/kirigami/controls/
    // NavigationTabBar.qml, confirmed installed at Kirigami 6.27.0)
    // unconditionally forces `action.checkable = true` on every action it's
    // given, and clicking a checkable AbstractButton toggles its own
    // `checked` property directly -- which destroys a QML `checked: <expr>`
    // binding the instant it's clicked. This bar's required semantics
    // (Compose is action-only and must NEVER visually become "selected", no
    // matter how many times it's tapped) can't be expressed cleanly against
    // that behavior, so this uses the fallback PillTab bar that the brief
    // and Phase 6 global constraint 4 explicitly allow instead.
    footer: Rectangle {
        width: parent ? parent.width : 0
        height: 64
        color: Theme.panel
        border.width: 1
        border.color: Theme.line

        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 8

            PillTab {
                Layout.fillWidth: true
                text: i18n("Inbox")
                selected: root.activeTab === "inbox"
                onClicked: root.goInbox()
            }
            PillTab {
                Layout.fillWidth: true
                text: i18n("Compose")
                selected: false // action-only -- never participates in tab selection
                onClicked: root.pageStack.push(composePageComponent)
            }
            PillTab {
                Layout.fillWidth: true
                text: i18n("Contacts")
                selected: root.activeTab === "contacts"
                onClicked: root.goContacts()
            }
        }
    }

    // ---- Inbox: pageStack.initialPage -----------------------------------
    pageStack.initialPage: Kirigami.Page {
        id: inboxPage
        objectName: "inboxPage"
        title: i18n("Inbox")

        // Populates MailApp's model from the on-disk cache (and attempts a
        // network refresh -- see MailController::selectFolder/refresh's
        // doc comment) as soon as this page exists, so a cold launch shows
        // whatever's already cached rather than staying empty until the
        // user taps something. currentFolder already defaults to "INBOX"
        // in MailController's constructor, so a plain refresh() (not
        // selectFolder) is enough here.
        Component.onCompleted: {
            MailApp.refresh()
            // Populates the folder popup's Archive subfolders. Quiet on
            // failure -- see MailController::refreshFolders.
            MailApp.refreshFolders()
        }

        function currentFolderDisplayName() {
            return Format.folderDisplayName(MailApp.mailFolders(), MailApp.currentFolder)
        }

        // Same "reasonable initials logic" shape as EmailDetail.qml's own
        // initialsFor() (Task 35) / ContactsList.qml's initialsFor() (Task
        // 36). The whitespace-split-to-initials core is shared
        // (Format.initialsFromNamePart()); this wrapper keeps its own
        // "Name <email>" parsing and "?" fallback on empty, since it
        // doesn't need EmailDetail.qml's further email-local-part fallback.
        function initialsForSender(sender) {
            const s = sender || ""
            const lt = s.indexOf("<")
            const namePart = (lt !== -1 ? s.substring(0, lt) : s).trim()
            const initials = Format.initialsFromNamePart(namePart)
            return initials.length > 0 ? initials : "?"
        }

        function formatTimestamp(atUtc) {
            if (!atUtc)
                return ""
            const d = new Date(atUtc)
            return isNaN(d.getTime()) ? atUtc : Qt.formatDateTime(d, "MMM d, hh:mm")
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            // ---- folder picker + refresh header -----------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                GhostButton {
                    text: i18n("Folder: %1", inboxPage.currentFolderDisplayName())
                    onClicked: folderPopup.open()
                }
                Item { Layout.fillWidth: true }
                BusyIndicator {
                    running: MailApp.isBusy
                    visible: MailApp.isBusy
                    implicitWidth: 28
                    implicitHeight: 28
                }
                GhostButton {
                    text: i18n("Refresh")
                    enabled: !MailApp.isBusy
                    onClicked: MailApp.refresh()
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

            // ---- keyword pill row ---------------------------------------
            // "All" is hardcoded first and always present (not part of
            // MailApp.keywordTabs -- see MailController::keywordTabs's own
            // doc comment); the rest come straight from keywordTabs.
            // Horizontally-scrolling Flickable rather than a wrapping Flow:
            // this is meant to read as a single scannable filter row, not
            // a multi-line block that pushes the list down as more
            // keywords accumulate.
            Flickable {
                Layout.fillWidth: true
                // +10 reserves dedicated space below the pills for the
                // horizontal scrollbar thumb, so it doesn't sit on top of
                // the pills themselves.
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
                            // modelData.name is a keyword the user typed --
                            // data, not wrapped (see the task-49 brief's
                            // exclusion list); only the "%1 (%2)" chrome
                            // wrapper around it is.
                            text: i18n("%1 (%2)", modelData.name, modelData.count)
                            selected: MailApp.selectedKeyword === modelData.name
                            onClicked: MailApp.selectKeyword(modelData.name)
                        }
                    }
                }
            }

            // ---- email list -----------------------------------------------
            // No unread/read visual weight: Email.status (core/models/
            // Email.h) has no documented enumeration of values anywhere in
            // core/ and nothing in the seeded/test data conventions implies
            // one either -- per the task-38 brief's own instruction, this
            // skips inventing an "unread" signal rather than guessing at
            // one that doesn't exist in the current schema.
            EmptyState {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: emailListView.count === 0
                // "tap Refresh", not "pull to refresh" -- this screen uses
                // the visible-Refresh-button fallback (see the header
                // above), not an actual pull gesture; see task-38-report.md
                // for why.
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
                // Recycling isn't worth the correctness hazard here: a
                // reused SwipeDelegate would keep its `actionTriggered`
                // guard (see below) across whichever new model row it gets
                // rebound to, which could silently disable that row's
                // swipe actions forever. This app's inbox lists are small
                // (a single relay folder, no infinite scroll), so disabling
                // delegate reuse costs nothing observable.
                reuseItems: false

                delegate: SwipeDelegate {
                    id: emailRow
                    width: emailListView.width

                    // Guards against a double-fire if a user manages to
                    // trigger two click events on the same revealed action
                    // label in quick succession (e.g. a fast double-tap)
                    // before the model updates and this row disappears.
                    //
                    // Set when the action is dispatched and cleared only if
                    // it FAILS. A failed archive ("Not paired" on first
                    // launch, say) has to stay retryable on this same row
                    // instance -- reuseItems: false only recreates delegates
                    // when the model itself rebuilds, so a permanently
                    // latched row would never offer its swipe actions again.
                    property bool actionTriggered: false

                    Connections {
                        target: MailApp
                        function onActionCompleted(action, messageIds, ok) {
                            if (!ok && messageIds.indexOf(model.messageId) !== -1)
                                emailRow.actionTriggered = false
                        }
                    }

                    contentItem: RowLayout {
                        spacing: 12

                        Avatar {
                            initials: inboxPage.initialsForSender(model.sender)
                            size: 40
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    Layout.fillWidth: true
                                    textFormat: Text.PlainText
                                    text: model.sender
                                    color: Theme.inkStrong
                                    font.family: Theme.fontUi
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: inboxPage.formatTimestamp(model.atUtc)
                                    color: Theme.ink
                                    font.family: Theme.fontUi
                                    font.pixelSize: 11
                                }
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

                    background: Rectangle {
                        color: emailRow.pressed ? Theme.panel : Theme.bg
                    }

                    // Swipe RIGHT reveals Delete. Per SwipeDelegate's own
                    // documented contract (verified against the installed
                    // Qt 6 docs, not assumed from the property name):
                    // `swipe.left` is the delegate that gets revealed when
                    // the control is swiped to the right.
                    swipe.left: Rectangle {
                        anchors.fill: parent
                        color: Theme.dangerColor

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            text: i18n("Delete")
                            // readableOnAccent reuses the same "light text
                            // on a solid saturated background" pairing
                            // already established for PillTab's selected
                            // state / PrimaryButton, rather than inventing
                            // a new semantic-color pairing for this one
                            // spot.
                            color: Theme.readableOnAccent
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                            font.weight: Font.Medium

                            SwipeDelegate.onClicked: {
                                if (emailRow.actionTriggered)
                                    return
                                // Latched BEFORE the call, and released by
                                // emailRow's onActionCompleted below if the
                                // action failed. The old code latched on
                                // success instead, which worked only because
                                // deleteEmails ran synchronously: the double-
                                // tap window it guards is now the whole round
                                // trip, so latching afterwards would leave it
                                // wide open for exactly as long as the guard
                                // matters.
                                emailRow.actionTriggered = true
                                MailApp.deleteEmails([model.messageId])
                            }
                        }
                    }

                    // Swipe LEFT reveals Archive. `swipe.right` is revealed
                    // when the control is swiped to the left (same
                    // documented contract as above).
                    swipe.right: Rectangle {
                        anchors.fill: parent
                        color: Theme.warningColor

                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            text: i18n("Archive")
                            color: Theme.readableOnAccent
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                            font.weight: Font.Medium

                            SwipeDelegate.onClicked: {
                                if (emailRow.actionTriggered)
                                    return
                                // See the matching comment on the Delete
                                // handler above -- latch first, release below
                                // on failure.
                                emailRow.actionTriggered = true
                                MailApp.archiveEmails([model.messageId])
                            }
                        }
                    }

                    onClicked: root.openEmailDetail(model.messageId, model.folder)
                }
            }
        }

        // ---- folder picker popup -----------------------------------------
        Popup {
            id: folderPopup
            modal: true
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            padding: 8
            // Centered over the whole window rather than anchored beneath
            // the Folder button -- simpler and just as discoverable given
            // there are only 6 standard folders, and avoids doing
            // mapToItem coordinate math for a dropdown-style anchor.
            x: (root.width - width) / 2
            y: (root.height - height) / 2

            background: Rectangle {
                color: Theme.panel
                radius: Theme.shapePanel
                border.width: 1
                border.color: Theme.line
            }

            contentItem: ColumnLayout {
                spacing: 4

                Repeater {
                    id: mobileFolderRepeater
                    // Server subfolders (Archive/...) nested under their
                    // parent -- see MailController::mailFolders().
                    model: MailApp.mailFolders()

                    Connections {
                        target: MailApp
                        function onFoldersChanged() {
                            mobileFolderRepeater.model = MailApp.mailFolders()
                        }
                    }

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        implicitWidth: 200
                        implicitHeight: folderLabel.implicitHeight + 16
                        radius: Theme.shapeButton
                        color: folderTap.pressed ? Theme.bg : "transparent"

                        Text {
                            id: folderLabel
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 10 + (modelData.depth || 0) * 14
                            textFormat: Text.PlainText
                            text: modelData.displayName
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                        }

                        TapHandler {
                            id: folderTap
                            onTapped: {
                                MailApp.selectFolder(modelData.wireName)
                                folderPopup.close()
                            }
                        }
                    }
                }
            }
        }
    }
    // ---- app lock -----------------------------------------------------
    // Highest z in the window and anchored over everything, so no mail,
    // subject line or contact is readable behind it. Loader, so the PIN
    // screen doesn't exist at all when the lock is off.
    // A mobile session genuinely gets backgrounded/suspended the way
    // Android's does, so application state is the right trigger here --
    // unlike Desktop, where it fires constantly on focus changes and would
    // make the lock unusable.
    Connections {
        target: Qt.application
        function onStateChanged() {
            // lockAfterGrace() honours the user's chosen delay, which
            // defaults to 0 -- i.e. locking synchronously, exactly as this
            // did before the setting existed. Coming back cancels a delay
            // still counting down.
            if (Qt.application.state !== Qt.ApplicationActive)
                AppLock.lockAfterGrace()
            else
                AppLock.cancelPendingLock()
        }
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

    // Deliberately a Toast rather than a StatusBanner row. StatusBanner is
    // danger-styled and cannot be dismissed, and is reserved for conditions
    // that stop the app working. A routine "a new version exists" painted in
    // the same red, unclearable strip would devalue the warnings that matter.
    //
    // UpdateToast (not plain Toast): defers the toast until unlock when the
    // startup update check lands while the PIN screen (z: 1000) is covering
    // this z: 940 toast -- see UpdateToast.qml for why a plain Toast here
    // meant a lock-enabled user was never told at all.
    UpdateToast {
        id: updateToast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        z: 940
    }

    // Same component the desktop root and its pop-out windows use, so there
    // is one definition of "what the lock screen covers" rather than two
    // hand-maintained copies. Mobile has no pop-out windows, so this single
    // instance covers everything here.
    // See DesktopRoot.qml for the reasoning: QQuickOverlay stacks above the
    // content item, so anything open when the lock engages outlives it.
    Connections {
        target: AppLock
        function onLockedChanged() {
            if (!AppLock.locked)
                return;
            folderPopup.close();
        }
    }

    LockOverlay { id: unlockOverlay }

    // Answers a notification tap-through that cannot say which mailbox it
    // meant. Anchored over everything, like the other overlay dialogs.
    NotificationFolderDialog {
        id: notificationFolderDialog
        anchors.fill: parent
        z: 900
        onChosen: function (messageId, folder) {
            root.selectEmail(messageId, folder)
        }
    }

}
