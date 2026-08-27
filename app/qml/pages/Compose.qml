import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Dialogs
import com.kysecurity.mail 1.0
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

    // The token MailApp.sendMail() returned for the send THIS instance
    // started, or 0 when it has none outstanding.
    //
    // MailApp is a QML singleton, so its pickupFallbackRequired/sendCompleted
    // signals reach every live Compose -- and two coexist easily (a pop-out
    // compose window plus Ctrl+N in the main window). Comparing this token is
    // how each one asks "is this signal mine?".
    //
    // This used to be a `sendInFlight` bool, which worked only because
    // MailController emitted pickupFallbackRequired synchronously from inside
    // sendMail(): the true owner was the instance with that call on its
    // stack. The request runs on a worker thread now and nobody has a call on
    // the stack, so ownership had to become a value we hold. It is the same
    // token that already answered "is this confirmation still valid?", so the
    // two mechanisms collapsed into one rather than a second being invented.
    property var mySendToken: 0
    // Separately tracked because saveDraft and sendMail can both be
    // outstanding and their tokens come from the same counter.
    property var myDraftToken: 0

    // Disables Send/Save while this instance has something outstanding.
    // MailApp.isBusy is singleton-wide and would also disable this composer
    // while an unrelated inbox refresh ran.
    readonly property bool sendInFlight: root.mySendToken !== 0
    readonly property bool draftInFlight: root.myDraftToken !== 0
    // Which wording the draft acknowledgement uses. Set alongside the call
    // rather than derived, because openWebmailDrafts and saveDraft report
    // through the same signal.
    property bool draftHandoffToWebmail: false
    // True between the 409 refusal and the user answering the dialog. Keeps
    // sendCompleted from releasing mySendToken while the confirmation still
    // needs it -- see onSendCompleted below.
    property bool awaitingPickupConfirmation: false

    // Recipient fields joined into one value purely so a single change handler
    // can debounce the PGP key preflight. Watching the three TokenFields'
    // joinedText is enough -- every way an address enters or leaves a field
    // (typing, Enter/Tab/comma, the address book, removing a token) ends up
    // reflected there.
    readonly property string recipientsSnapshot:
        toField.joinedText + "|" + ccField.joinedText + "|" + bccField.joinedText

    onRecipientsSnapshotChanged: {
        // Only while Encrypt is on: the warning is meaningless otherwise, and
        // this would be a network call on every keystroke for nothing.
        if (encryptToggle.checked)
            preflightTimer.restart()
    }

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
        // Which PGP controls may appear at all depends on the account's key
        // custody, which only the relay knows. A failure leaves every control
        // hidden -- "couldn't check" is never "no PGP".
        MailApp.refreshPgpComposeState()
    }

    // Hands the composition to webmail: saves it as a draft, then opens the
    // system browser at Drafts. Only reachable for a client-custody account,
    // whose private key exists solely in the user's browser.
    function tryWebmailHandoff() {
        toField.commitInputAsToken()
        ccField.commitInputAsToken()
        bccField.commitInputAsToken()

        bodyEditor.requestSendableHtml(function(result) {
            root.validationError = ""
            // Deliberately stays on the compose page on failure: the message
            // is still here, and navigating away would lose it. Shares
            // saveDraft's token and signal; draftHandoffToWebmail only picks
            // the wording of the acknowledgement.
            root.draftHandoffToWebmail = true
            root.myDraftToken = MailApp.openWebmailDrafts(toField.joinedText, ccField.joinedText,
                                                           bccField.joinedText, subjectField.text, result.html,
                                                           root.attachmentPaths)
        })
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

            // A client-custody account whose key is in THIS machine's gpg:
            // encrypt and sign here rather than handing the user to webmail.
            // C++ owns both halves of that decision -- the account's custody
            // mode and whether there is a usable gpg -- so this is one test.
            if (MailApp.pgpCanSendFromThisDevice) {
                // Refused rather than silently dropped. This path builds its
                // own MIME and does not write multipart bodies yet, so an
                // attachment cannot travel; sending the message without it
                // would be a data loss the sender never sees.
                root.mySendToken = MailApp.sendClientEncrypted(toField.joinedText, ccField.joinedText,
                                                               bccField.joinedText, subjectField.text,
                                                               result.html, root.attachmentPaths)
                return
            }

            // Returns a token, not a result. 0 means it was refused before any
            // request was built (not paired, or an attachment that could not
            // be read or is over the cap) -- MailApp.lastError says which.
            // Anything else identifies this send, and every signal about it
            // carries the same value back.
            root.mySendToken = MailApp.sendMail(toField.joinedText, ccField.joinedText, bccField.joinedText,
                                                 subjectField.text, result.html, root.attachmentPaths,
                                                 signToggle.checked, encryptToggle.checked)
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
            // Acknowledged by onDraftSaveCompleted below, once the draft has
            // actually reached the server.
            root.myDraftToken = MailApp.saveDraft(toField.joinedText, ccField.joinedText, bccField.joinedText,
                                                   subjectField.text, result.html, root.attachmentPaths)
            root.draftHandoffToWebmail = false
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
                            textFormat: Text.PlainText
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

        // --- PGP controls -------------------------------------------------
        // Which of these exist at all comes from the account's key custody
        // (MailApp.refreshPgpComposeState above). Encrypt is offered even with
        // no PGP identity of the user's own, because encryption targets the
        // RECIPIENTS' public keys; only signing needs the account's own key.
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            visible: MailApp.pgpCanEncrypt || MailApp.pgpCanSign

            CheckBox {
                id: encryptToggle
                visible: MailApp.pgpCanEncrypt
                text: i18n("Encrypt")
                contentItem: Text {
                    textFormat: Text.PlainText
                    text: encryptToggle.text
                    color: Theme.inkStrong
                    font.family: Theme.fontUi
                    font.pixelSize: 14
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: encryptToggle.indicator.width + encryptToggle.spacing
                }
                // Turning it on is the first moment the preflight has any
                // meaning, so ask then rather than waiting for the next
                // recipient edit. Turning it off cancels a pending debounce:
                // the warning it would produce is not shown while Encrypt is
                // off, so the request would be pure waste.
                onCheckedChanged: {
                    if (checked)
                        preflightTimer.restart()
                    else
                        preflightTimer.stop()
                }
            }
            CheckBox {
                id: signToggle
                visible: MailApp.pgpCanSign
                text: i18n("Sign")
                contentItem: Text {
                    textFormat: Text.PlainText
                    text: signToggle.text
                    color: Theme.inkStrong
                    font.family: Theme.fontUi
                    font.pixelSize: 14
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: signToggle.indicator.width + signToggle.spacing
                }
            }
            Item { Layout.fillWidth: true }
        }

        // Inline, non-blocking. Deliberately worded as "no key on file" rather
        // than "this will be sent as a plaintext link": the preflight reads only
        // the user's contacts, while the send path additionally runs WKD and
        // keyserver discovery, so this is a lower bound and must not be phrased
        // as a prediction. The server's 409 is what actually drives the
        // confirmation.
        Text {
            Layout.fillWidth: true
            visible: encryptToggle.checked && MailApp.pgpKeylessRecipients.length > 0
            textFormat: Text.PlainText
            text: i18np("We don't have a PGP key on file for %2.",
                        "We don't have PGP keys on file for %2.",
                        MailApp.pgpKeylessRecipients.length,
                        MailApp.pgpKeylessRecipients.join(", "))
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        // Client-custody accounts get this instead of the toggles: the private
        // key exists only in the user's browser, so no request from this app
        // can sign or encrypt on its behalf.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            // Only when this machine CANNOT do the work. When it can, the
            // ordinary Send button encrypts and signs here and there is
            // nothing to hand off.
            visible: MailApp.pgpHandoffToWebmail && !MailApp.pgpCanSendFromThisDevice

            Text {
                Layout.fillWidth: true
                text: i18n("This account's PGP key is stored only in your browser, so this app can't encrypt on its behalf.")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                GhostButton {
                    text: i18n("Continue in webmail")
                    enabled: !root.sendInFlight && !root.draftInFlight
                    onClicked: root.tryWebmailHandoff()
                }
                Item { Layout.fillWidth: true }
            }
        }

        Text {
            objectName: "clientEncryptNotice"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            visible: MailApp.pgpCanSendFromThisDevice
            text: root.attachmentPaths.length > 0
                  ? i18n("This message and its attachments will be encrypted and signed on this computer with your own key.")
                  : i18n("This message will be encrypted and signed on this computer with your own key.")
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        Text {
            objectName: "missingSentCopyNotice"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            // The message went out; the outbox will not have it. Said plainly
            // rather than logged, because the user cannot discover it any
            // other way.
            visible: MailApp.lastSendMissingSentCopy
            text: i18n("Sent. A copy could not be saved to your Sent folder, because it can only be stored encrypted to your own key.")
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: root.validationError !== "" || MailApp.lastError !== ""
            textFormat: Text.PlainText
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
                // This instance's own send, NOT MailApp.isBusy. That property
                // is singleton-wide, and now that the inbox refresh is
                // asynchronous too it goes true for reasons that have nothing
                // to do with this composer -- which would both show "Sending…"
                // over a message nobody sent and disable Send below.
                visible: root.sendInFlight
                text: i18n("Sending…")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 13
            }
            Item { Layout.fillWidth: true }
            GhostButton {
                text: i18n("Save Draft")
                enabled: !root.sendInFlight && !root.draftInFlight
                onClicked: root.trySaveDraft()
            }
            PrimaryButton {
                text: i18n("Send")
                enabled: !root.sendInFlight && !root.draftInFlight
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

    // Debounces the recipient key preflight so typing an address doesn't fire
    // one request per keystroke. Restarted by recipientsSnapshot changes and by
    // switching Encrypt on.
    Timer {
        id: preflightTimer
        interval: 500
        onTriggered: MailApp.preflightRecipients(toField.joinedText, ccField.joinedText,
                                                  bccField.joinedText)
    }

    PickupFallbackDialog {
        id: pickupFallbackDialog
        z: 40
        onConfirmed: function (token) {
            // Re-sends the exact payload the server refused, with the opt-in
            // set. Nothing is rebuilt here on purpose -- MailController holds
            // the original request so the confirmed send is byte-identical.
            // The token proves that request is still the one it refused, and
            // the confirmed re-send keeps it -- it is the same logical send,
            // so this instance stays the one waiting on it. A 0 back means
            // the pending send was already replaced or resolved and nothing
            // was dispatched.
            root.awaitingPickupConfirmation = false
            root.mySendToken = MailApp.confirmPickupFallbackSend(token)
        }
        onCancelled: {
            // Releases the token too. Without that this composer's Send would
            // stay disabled for the rest of its life, because sendCompleted
            // for the refusal deliberately left it set.
            root.awaitingPickupConfirmation = false
            root.mySendToken = 0
            MailApp.discardPendingSend()
        }
    }

    Connections {
        target: MailApp

        // The relay refused an encrypted send because at least one recipient has
        // no usable key. Nothing was delivered, so confirming is safe and cannot
        // duplicate the message.
        //
        // The guard is load-bearing, not defensive: without it every live
        // Compose opens this dialog naming the sending window's addresses, and
        // a confirmation given in the wrong window would send the OTHER
        // window's message while destroying this one's unsent draft.
        function onPickupFallbackRequired(token, recipients) {
            if (token !== root.mySendToken)
                return
            root.awaitingPickupConfirmation = true
            pickupFallbackDialog.open(token, recipients)
        }

        // Terminal outcome of this instance's send.
        //
        // This fires for the pickup-fallback refusal too, with ok false, and
        // immediately after pickupFallbackRequired. The token must NOT be
        // released in that case: the dialog is still waiting on it, and a
        // cleared token would make the user's confirmation arrive looking
        // like somebody else's send. Hence the explicit flag rather than
        // testing the popup's visibility, which depends on when Popup.open()
        // takes effect.
        function onSendCompleted(token, ok) {
            if (token !== root.mySendToken)
                return
            if (!root.awaitingPickupConfirmation)
                root.mySendToken = 0
            if (ok)
                root.sendSucceeded()
        }

        function onDraftSaveCompleted(token, ok) {
            if (token !== root.myDraftToken)
                return
            root.myDraftToken = 0
            if (!ok)
                return // MailApp.lastError carries the reason
            // Acknowledge in place: this component already owns a Toast for
            // its other inline feedback, and the composer stays open so there
            // is no navigation change to signal a save happened.
            toast.show(root.draftHandoffToWebmail
                ? i18n("Saved to Drafts — continue in your browser")
                : i18n("Draft saved"))
            if (!root.draftHandoffToWebmail)
                root.draftSaved()
        }

        // No onSendWarning here on purpose. A warning only ever accompanies a
        // SUCCESSFUL send, and the hosts answer sendSucceeded() by destroying
        // this component -- so a notice shown here would die in the same turn
        // it appeared. MobileRoot.qml / DesktopRoot.qml own that sink.
    }

    Toast {
        id: toast
        z: 30
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        anchors.horizontalCenter: parent.horizontalCenter
    }
}
