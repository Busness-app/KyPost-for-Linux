import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Dialogs
import com.urlxl.mail 1.0
import "../components"
import "../utils/format.js" as Format

// Task 35 -- plain reusable Item, deliberately NOT a Kirigami.Page (see
// Phase 6 global constraint 4); MobileRoot/DesktopRoot each host this
// directly (Tasks 38/39). Body is a RichBodyEditor (WYSIWYG HTML,
// see docs/superpowers/specs/2026-07-18-html-compose-design.md) --
// the earlier "plain text only" constraint no longer applies.
Item {
    id: root

    // Public API -- pre-fill values, set by EmailDetail.qml for reply/
    // reply-all/forward, left empty for a fresh compose.
    property string initialTo: ""
    property string initialSubject: ""
    property string initialBody: ""
    // True only for the pop-out flow (DesktopRoot's composeWindowComponent
    // seeds initialBody from currentDraftForPopOut()'s already-sanitized
    // RichBodyEditor HTML). False (the default) covers EmailDetail.qml's
    // Reply/Reply All/Forward flow, which seeds initialBody with plain text
    // that still needs escaping + quoting -- see quotedInitialBodyHtml()
    // and Component.onCompleted below.
    property bool initialBodyIsHtml: false
    // True for the instance DesktopRoot's composeWindowComponent embeds
    // inside an already-standalone pop-out Window -- hides the pop-out
    // button there (see below), same reasoning as EmailDetail.isPoppedOut.
    property bool isPoppedOut: false

    // Emitted once MailApp.sendMail() reports success -- MobileRoot/
    // DesktopRoot are expected to pop/close this screen in response; this
    // component doesn't assume push-navigation vs a pane, per constraint 4.
    signal sendSucceeded()
    // Emitted once MailApp.saveDraft() reports success. Unlike
    // sendSucceeded(), the composer deliberately stays open -- saving a
    // draft is not finishing with it -- so hosts should acknowledge rather
    // than navigate away.
    signal draftSaved()
    // Detach into a standalone top-level window (Desktop mode only -- see
    // the pop-out IconButton below). Carries the current To/Subject/Body so
    // the draft continues in the new window; Cc/Bcc/attachments don't
    // transfer (Compose's own initialTo/initialSubject/initialBody seed API
    // -- the same one Reply/Reply All/Forward already use via
    // EmailDetail.composeRequested -- doesn't carry those either). The host
    // is expected to close this embedded copy in response, same "host
    // decides what to do" shape as sendSucceeded() above.
    signal popOutRequested(string to, string subject, string body)

    implicitWidth: 360
    implicitHeight: 640

    property var attachmentPaths: []
    property string validationError: ""

    // Compose autocomplete (ContactAutocomplete.md): tracks whichever
    // TokenField most recently changed its query text, so the one shared
    // dropdown/picker below know which field to reposition under / add a
    // picked address into.
    property var activeField: null

    // Reply/Forward seed initialBody with a plain-text quote block
    // (EmailDetail.qml's composeRequested() -- unchanged by this feature).
    // HTML-escape it and preserve line breaks so it renders correctly inside
    // the rich editor while staying fully editable/deletable, same as
    // before.
    function quotedInitialBodyHtml(text) {
        if (text === "")
            return ""
        return "<blockquote>" + Format.escapeHtml(text).replace(/\n/g, "<br>") + "</blockquote>"
    }

    function seedTokensFromString(field, value) {
        const parts = (value || "").split(",")
        for (let i = 0; i < parts.length; i++) {
            const trimmed = parts[i].trim()
            if (trimmed !== "")
                field.addToken(trimmed)
        }
    }

    function repositionDropdown(field) {
        const point = field.mapToItem(root, 0, field.height)
        autocompleteDropdown.x = point.x
        autocompleteDropdown.y = point.y
        autocompleteDropdown.width = field.width
    }

    function onFieldQueryChanged(field, query) {
        root.activeField = field
        autocompleteDropdown.query = query
        if (query === "") {
            autocompleteDropdown.close()
        } else {
            root.repositionDropdown(field)
            autocompleteDropdown.open()
        }
    }

    function targetField(target) {
        if (target === "cc")
            return ccField
        if (target === "bcc")
            return bccField
        return toField
    }

    Component.onCompleted: {
        seedTokensFromString(toField, root.initialTo)
        subjectField.text = root.initialSubject
        // Pop-out drafts arrive as already-sanitized HTML (see
        // initialBodyIsHtml above) and must be loaded as-is -- escaping or
        // blockquote-wrapping it would corrupt the formatting. Reply/Forward
        // drafts are plain text and still need quotedInitialBodyHtml().
        bodyEditor.loadInitialHtml(root.initialBodyIsHtml ? root.initialBody : root.quotedInitialBodyHtml(root.initialBody))
    }

    function trySend() {
        // Any address still sitting uncommitted in a field's input box (the
        // user typed it but never hit Enter/Tab/comma) is committed first --
        // otherwise it would silently vanish from the sent message.
        toField.commitInputAsToken()
        ccField.commitInputAsToken()
        bccField.commitInputAsToken()

        bodyEditor.requestSendableHtml(function(result) {
            // Mirrors Android's "Please fill in all fields" check -- Cc/Bcc
            // stay optional, only To/Subject/Body are required.
            if (toField.joinedText.trim() === "" || subjectField.text.trim() === "" || result.isEmpty) {
                root.validationError = i18n("Please fill in all fields")
                return
            }
            root.validationError = ""
            const ok = MailApp.sendMail(toField.joinedText, ccField.joinedText, bccField.joinedText,
                                         subjectField.text, result.html, root.attachmentPaths)
            if (ok)
                root.sendSucceeded()
        })
    }

    // Saves to the Drafts mailbox. Deliberately does NOT apply trySend()'s
    // "fill in all fields" check -- a half-written draft is the normal thing
    // to save, and refusing to save it would be the opposite of useful.
    function trySaveDraft() {
        toField.commitInputAsToken()
        ccField.commitInputAsToken()
        bccField.commitInputAsToken()

        bodyEditor.requestSendableHtml(function(result) {
            root.validationError = ""
            if (MailApp.saveDraft(toField.joinedText, ccField.joinedText, bccField.joinedText,
                                   subjectField.text, result.html, root.attachmentPaths)) {
                // Acknowledge in place: this component already owns a Toast
                // for its other inline feedback, and the composer stays open
                // so there is no navigation change to signal a save happened.
                toast.show(i18n("Draft saved"))
                root.draftSaved()
            }
        })
    }

    // Commits any still-uncommitted address text the same way trySend()
    // does, so a pop-out doesn't silently drop a typed-but-not-yet-tokenized
    // "To" address.
    function currentDraftForPopOut(callback) {
        toField.commitInputAsToken()
        bodyEditor.requestSendableHtml(function(result) {
            callback({ to: toField.joinedText, subject: subjectField.text, body: result.html })
        })
    }

    function fileNameOf(path) {
        const parts = path.split("/")
        return parts[parts.length - 1]
    }

    // FileDialog's selectedFiles are QML `url` values ("file:///home/…",
    // percent-encoded) -- MailApp.sendMail()'s attachmentFilePaths expects
    // plain local filesystem paths (it hands each one straight to QFile),
    // so strip the scheme and percent-decode here rather than passing the
    // url string through as-is.
    function urlToLocalPath(fileUrl) {
        let s = fileUrl.toString()
        if (s.indexOf("file://") === 0)
            s = s.substring(7)
        return decodeURIComponent(s)
    }

    // Explicit, guaranteed-opaque background -- root is a plain Item with
    // none of its own, previously relying entirely on whatever hosts this
    // component (DesktopRoot's detail-column Rectangle when embedded, the
    // pop-out Window's own color when popped out) to paint behind every
    // pixel. That held for the embedded case, but the pop-out window left
    // at least one real gap unpainted (the ColumnLayout's inter-item
    // spacing between the body editor and the "Attach files" row showed
    // through to whatever's behind the window instead of Theme.bg). Rather
    // than track down every individual sliver a layout gap or a WebEngineView
    // rendering quirk might expose, this makes sure any gap anywhere in this
    // component's own bounds is covered by design.
    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        // Pop-out -- Desktop-only (General.isDesktopMode): popping out a
        // draft on Mobile has no separate-window concept to detach into.
        RowLayout {
            Layout.fillWidth: true
            visible: General.isDesktopMode && !root.isPoppedOut
            spacing: 8

            Item { Layout.fillWidth: true }
            IconButton {
                icon: "window-new"
                tooltip: i18n("Open in New Window")
                onClicked: {
                    root.currentDraftForPopOut(function(draft) {
                        root.popOutRequested(draft.to, draft.subject, draft.body)
                    })
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TokenField {
                id: toField
                Layout.fillWidth: true
                placeholderText: i18n("To")
                dropdown: autocompleteDropdown
                onQueryChanged: (query) => root.onFieldQueryChanged(toField, query)
                onDuplicateRejected: (email) => toast.show(i18n("%1 is already added", email))
            }
            GhostButton {
                text: i18n("Address Book")
                onClicked: addressBookPicker.open()
            }
        }
        TokenField {
            id: ccField
            Layout.fillWidth: true
            placeholderText: i18n("Cc")
            dropdown: autocompleteDropdown
            onQueryChanged: (query) => root.onFieldQueryChanged(ccField, query)
            onDuplicateRejected: (email) => toast.show(i18n("%1 is already added", email))
        }
        TokenField {
            id: bccField
            Layout.fillWidth: true
            placeholderText: i18n("Bcc")
            dropdown: autocompleteDropdown
            onQueryChanged: (query) => root.onFieldQueryChanged(bccField, query)
            onDuplicateRejected: (email) => toast.show(i18n("%1 is already added", email))
        }
        ThemedTextField {
            id: subjectField
            Layout.fillWidth: true
            placeholderText: i18n("Subject")
        }

        // Body -- rich HTML editor (see RichBodyEditor.qml; supersedes the
        // earlier plain-TextArea-only constraint).
        RichBodyEditor {
            id: bodyEditor
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            GhostButton {
                text: i18n("Attach files")
                onClicked: fileDialog.open()
            }
            Item { Layout.fillWidth: true }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 8
            visible: root.attachmentPaths.length > 0

            Repeater {
                model: root.attachmentPaths
                delegate: Rectangle {
                    radius: Theme.shapeButton
                    color: Theme.panel
                    border.width: 1
                    border.color: Theme.line
                    implicitWidth: chipRow.implicitWidth + 20
                    implicitHeight: chipRow.implicitHeight + 12

                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 6

                        Text {
                            text: root.fileNameOf(modelData)
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 12
                        }
                        Text {
                            text: "✕"
                            color: Theme.ink
                            font.family: Theme.fontUi
                            font.pixelSize: 12

                            TapHandler {
                                onTapped: {
                                    const updated = root.attachmentPaths.slice()
                                    updated.splice(index, 1)
                                    root.attachmentPaths = updated
                                }
                            }
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.validationError !== "" || MailApp.lastError !== ""
            text: root.validationError !== "" ? root.validationError : MailApp.lastError
            color: Theme.dangerColor
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                visible: MailApp.isBusy
                text: i18n("Sending…")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 13
            }
            Item { Layout.fillWidth: true }
            GhostButton {
                text: i18n("Save Draft")
                enabled: !MailApp.isBusy
                onClicked: root.trySaveDraft()
            }
            PrimaryButton {
                text: i18n("Send")
                enabled: !MailApp.isBusy
                onClicked: root.trySend()
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: i18n("Attach files")
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            const paths = []
            for (let i = 0; i < selectedFiles.length; i++)
                paths.push(root.urlToLocalPath(selectedFiles[i]))
            root.attachmentPaths = root.attachmentPaths.concat(paths)
        }
    }

    // Compose autocomplete (ContactAutocomplete.md) -- one shared dropdown
    // repositioned under whichever TokenField is currently active (see
    // onFieldQueryChanged/repositionDropdown above), plus the address-book
    // picker and the duplicate-rejection toast. All three are top-level
    // overlay children of `root`, not the ColumnLayout, so they float above
    // the form instead of taking up layout space.
    AutocompleteDropdown {
        id: autocompleteDropdown
        z: 10
        onItemChosen: (email) => {
            if (root.activeField)
                root.activeField.addToken(email)
            autocompleteDropdown.close()
        }
    }

    AddressBookPickerDialog {
        id: addressBookPicker
        z: 20
        toTokens: toField.tokens
        ccTokens: ccField.tokens
        bccTokens: bccField.tokens
        onContactPicked: (email, target) => root.targetField(target).addToken(email)
    }

    Toast {
        id: toast
        z: 30
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        anchors.horizontalCenter: parent.horizontalCenter
    }
}
