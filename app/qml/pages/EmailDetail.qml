import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtWebEngine
import com.kysecurity.mail 1.0
import "../components"
import "../utils/format.js" as Format

// Task 35 -- plain reusable Item, deliberately NOT a Kirigami.Page (see
// Phase 6 global constraint 4): MobileRoot wraps this in a thin
// Kirigami.Page shell when it pushes it (Task 38); DesktopRoot embeds it
// directly inside its detail-pane Item (Task 39). This file itself must not
// assume which root is hosting it -- Reply/Reply All/Forward surface their
// pre-filled fields via composeRequested() instead of pushing Compose.qml
// themselves, and Archive/Junk/Delete surface completion via
// actionCompleted() instead of popping pageStack/clearing a selection
// directly.
Item {
    id: root

    // Public API.
    property string messageId: ""
    property string folder: "" // wire folder name, e.g. "INBOX"
    // True for the instance DesktopRoot's emailWindowComponent embeds inside
    // an already-standalone pop-out Window -- hides the "Open in New Window"
    // button there (see below), since a pop-out of a pop-out has nowhere
    // more standalone to go.
    property bool isPoppedOut: false

    signal composeRequested(string to, string subject, string body)
    signal actionCompleted(string action) // action: "archive" | "junk" | "delete"
    // Detach into a standalone top-level window (Desktop mode only -- see
    // the pop-out IconButton below, gated on General.isDesktopMode). The
    // host (DesktopRoot) owns the actual Window creation and is expected to
    // clear its own embedded selection in response, same "host decides what
    // to do" shape as composeRequested/actionCompleted above.
    signal popOutRequested()

    implicitWidth: 360
    implicitHeight: 640

    // MailApp.findByMessageId() result -- a QVariantMap keyed the same way
    // as EmailListModel's roles (see MailController::findByMessageId's doc
    // comment). {} (empty map) means "nothing loaded yet" or "not cached".
    property var email: ({})
    property var attachments: [] // [{index, name, mimeType, size}, ...]
    property string attachmentStatus: ""
    // Remote images are blocked by default (see WebEngineView's
    // settings.autoLoadImages below) -- true once the user has explicitly
    // opted in via the "Show images" affordance, for the message currently
    // loaded. Reset on every reload() so switching to a different email
    // re-blocks images until asked again.
    property bool imagesLoaded: false

    // Whether there is any message content for the image-blocking notices
    // below to sit under.
    //
    // The `!!` is load-bearing, not decoration. `email` is {} until a message
    // is loaded, so both member reads are undefined then, and JavaScript's
    // `a || b` yields undefined rather than false -- which QML cannot assign
    // to a bool, and logged "Unable to assign [undefined] to bool" twice on
    // every startup. Hoisted to one property because the expression was
    // duplicated across the two notices that share it.
    readonly property bool hasRenderableBody: root.hasDecryptedBody
        || !!(root.email && (root.email.body || root.email.preview))

    // The decrypted body, but ONLY when the one MailApp is holding belongs to
    // the message this view is showing. MailApp is a singleton and holds at
    // most one plaintext, so without the id comparison a decrypted message
    // would appear under the next message's headers the moment the reader
    // moved on.
    // The id comparison inside is the control -- see Format.decryptedBodyFor,
    // where it lives so it can be tested without this file's singleton graph.
    readonly property var decrypted: Format.decryptedBodyFor(root.messageId,
                                                              MailApp.decryptedMessageId,
                                                              MailApp.decryptedHtml,
                                                              MailApp.decryptedPlain)
    readonly property bool hasDecryptedBody: root.decrypted.body !== ""
    // Which form it is comes from the message's own MIME Content-Type, not
    // from sniffing the characters -- see Format.renderedEmailHtml.
    readonly property bool decryptedIsHtml: root.decrypted.isHtml
    readonly property string decryptedBody: root.decrypted.body

    onMessageIdChanged: {
        // The reader has moved on, so the held plaintext is no longer
        // anybody's business. Dropped rather than merely hidden.
        MailApp.forgetDecrypted()
        reload()
    }
    onFolderChanged: reload()

    // MailApp is a singleton and the plaintext arrives asynchronously, so the
    // web view has to be told to re-render when it does -- and again when it
    // is forgotten, so the decrypted page does not stay on screen after the
    // app has dropped it.
    Connections {
        target: MailApp
        function onDecryptedChanged() {
            webViewLoader.applyContent()
        }
    }
    Component.onCompleted: reload()

    // Archive/Junk/Delete dispatch and return; the answer arrives here.
    //
    // MailApp is a singleton, so this fires for every EmailDetail alive --
    // DesktopRoot can have an embedded one and a popped-out Window showing a
    // different message at the same time. `messageIds` is what tells them
    // apart: only the instance whose own message was acted on reacts. The
    // old code could rely on "my call is on the stack" instead, because the
    // request ran synchronously inside the click handler; nothing has a call
    // on the stack any more.
    Connections {
        target: MailApp
        function onActionCompleted(action, messageIds, ok) {
            if (!ok || root.messageId === "" || messageIds.indexOf(root.messageId) === -1)
                return
            // "spam" is the wire verb; this component's own signal has always
            // called it "junk", matching the button.
            root.actionCompleted(action === "spam" ? "junk" : action)
        }

        // Both checks matter. mailbox/messageId keeps another EmailDetail's
        // answer out of this one, and it also drops a reply for a message
        // this instance has since navigated away from -- otherwise a slow
        // list would repopulate the chips with the previous mail's
        // attachments after the user had already moved on.
        function onAttachmentsListed(mailbox, messageId, attachments) {
            if (mailbox === root.folder && messageId === root.messageId)
                root.attachments = attachments
        }

        function onAttachmentDownloaded(messageId, index, ok) {
            if (messageId !== root.messageId)
                return
            // Under Hostile Location Protection the file is opened from a
            // temporary location rather than saved, so say so -- claiming
            // "Saved to Downloads" would be wrong, and claiming "never
            // touched disk" would oversell a tmpfs file. See
            // MailController::openAttachmentEphemerally.
            root.attachmentStatus = ok
                ? (AppLock.hostileLocationEnabled
                    ? i18n("Opened temporarily — not saved")
                    : i18n("Saved to Downloads"))
                : MailApp.lastError
            attachmentStatusTimer.restart()
        }
    }

    // Clears the refused-link notice a few seconds after it appears.
    Timer {
        id: blockedLinkTimer
        interval: 4000
        running: root.blockedLinkScheme !== ""
        onTriggered: root.blockedLinkScheme = ""
    }

    // ---- data loading -------------------------------------------------

    function reload() {
        root.imagesLoaded = false
        if (root.messageId === "") {
            root.email = {}
            root.attachments = []
            return
        }
        root.email = MailApp.findByMessageId(root.messageId)
        // Cleared, then refilled by onAttachmentsListed below. The list is
        // fetched off-thread now, so it cannot be assigned here -- and
        // leaving the previous message's attachments up while the new
        // message's are in flight would offer downloads that belong to the
        // mail the user just navigated away from.
        root.attachments = []
        if (root.email && root.email.hasAttachments)
            MailApp.listAttachments(root.folder, root.messageId)
        webViewLoader.applyContent()
    }

    // Re-parses the same HTML with settings.autoLoadImages now true --
    // toggling that setting alone doesn't retroactively fetch images a
    // completed parse already skipped, so this is a real reload, not just a
    // property flip.
    function showImages() {
        root.imagesLoaded = true
        webViewLoader.applyContent()
    }

    // ---- string-transform helpers (ported from Android's
    // EmailDetailActivity, per Task 35's brief -- kept here as plain JS
    // helpers rather than promoted to MailController, since none of them
    // touch anything beyond string manipulation of already-loaded QML data)
    // ---------------------------------------------------------------

    // If `raw` contains "<...>", returns the content between the first '<'
    // and the matching '>'; otherwise returns `raw` trimmed as-is.
    // Delegates to Format (utils/format.js) so the rule is testable on its own
    // and stays identical to the webmail and Android clients -- see
    // tests/qml/tst_AddressText.qml for why a display name must never win.
    function extractAddress(raw) {
        return Format.addressFromHeader(raw)
    }

    // Splits a comma/semicolon-joined address-list string (Email.sentTo/.cc
    // wire format, see RelayMailSource.h) into trimmed, non-empty parts.
    function splitAddresses(raw) {
        if (!raw)
            return []
        return raw.split(/[,;]/).map(function (s) { return s.trim() })
                   .filter(function (s) { return s.length > 0 })
    }

    // Prepends "<prefix> " unless `subject` already starts with `prefix`
    // case-insensitively (no double "Re: Re:"/"Fwd: Fwd:" prefixing).
    function withPrefix(subject, prefix) {
        const s = subject || ""
        if (s.toLowerCase().indexOf(prefix.toLowerCase()) === 0)
            return s
        return prefix + " " + s
    }

    // First letter(s) of the sender's display name (or, absent a display
    // name, the local-part of the address), split on whitespace, up to 2
    // characters -- "reasonable initials logic" per Task 35's brief, same
    // shape as Avatar's other call sites. The actual whitespace-split-to-
    // initials core is shared (Format.initialsFromNamePart()); the
    // "Name <email>" parsing and email-local-part fallback stay here since
    // MobileRoot.qml's own sender-initials wrapper doesn't need the latter.
    function initialsFor(sender) {
        const s = sender || ""
        const lt = s.indexOf("<")
        let namePart = (lt !== -1 ? s.substring(0, lt) : s).trim()
        if (namePart.length === 0) {
            const addr = extractAddress(s)
            const at = addr.indexOf("@")
            namePart = at !== -1 ? addr.substring(0, at) : addr
        }
        return Format.initialsFromNamePart(namePart)
    }

    function formatSize(size) {
        if (size < 1024)
            return i18n("%1 B", size)
        if (size < 1048576)
            return i18n("%1 KB", (size / 1024).toFixed(1))
        return i18n("%1 MB", (size / 1048576).toFixed(1))
    }

    // ---- body HTML scaffold --------------------------------------------

    function colorToHex(c) {
        function pad(v) {
            const h = Math.round(v * 255).toString(16)
            return h.length === 1 ? "0" + h : h
        }
        return "#" + pad(c.r) + pad(c.g) + pad(c.b)
    }

    // Schemes a message is allowed to open in the outside world live in
    // Format (utils/format.js) so the rule is testable on its own -- see
    // isExternallyOpenableUrl() there for what it protects against.
    function isExternallyOpenable(url) {
        return Format.isExternallyOpenableUrl(url)
    }

    // Non-empty for a moment after a link with a refused scheme was clicked,
    // so the user gets told the click did something rather than silently
    // nothing. Cleared by the notice's own timer.
    property string blockedLinkScheme: ""

    // The theme half of the body document. Colours come from the Theme
    // singleton, which is why this stays here and the security-relevant half
    // (the CSP, the HTML-vs-text decision, the escaping) lives in Format --
    // see utils/format.js. Those three are what a test needs to reach, and
    // they must not require QtWebEngine and the singleton graph to reach.
    function bodyStyle() {
        return "body { font-family: monospace; font-size: 14px; line-height: 1.5;"
            + " color: " + colorToHex(Theme.inkStrong) + "; background-color: " + colorToHex(Theme.bg) + ";"
            + " margin: 0; padding: 12px; word-break: break-word; }"
            + "a { color: " + colorToHex(Theme.accent) + "; }"
            + "img { max-width: 100%; height: auto; }"
            + "pre { white-space: pre-wrap; }"
    }

    function renderedHtml(body, forcePlainText) {
        return Format.renderedEmailHtml(body, root.imagesLoaded, root.bodyStyle(), forcePlainText)
    }

    // ---- layout ----------------------------------------------------

    // Wrapped in a Flickable rather than anchored straight to root: the sum
    // of the header/actions/WebEngineView/attachments' minimum heights can
    // exceed whatever fixed-size pane/Kirigami.Page ends up hosting this
    // component (a long email plus a full attachment row, on a short
    // window), and plain ColumnLayout doesn't clip or scroll its own
    // overflow. Scrolling here, internally, means this component behaves
    // correctly regardless of how MobileRoot/DesktopRoot (Task 38/39) end
    // up sizing it, rather than relying on every future host to remember to
    // wrap it in a scrollable container itself.
    Flickable {
        id: flickable
        anchors.fill: parent
        contentWidth: width
        contentHeight: contentColumn.implicitHeight + contentColumn.anchors.margins * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThemedScrollBar {}

        ColumnLayout {
            id: contentColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 12

        // Everything above the WebEngineView, grouped under one id so
        // webViewLoader below can size itself against "whatever's left"
        // of the available height (see that Loader's Layout.preferredHeight
        // binding) without manually summing each row's height.
        ColumnLayout {
            id: aboveWebView
            Layout.fillWidth: true
            spacing: contentColumn.spacing

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Avatar {
                initials: root.initialsFor(root.email.sender)
                size: 52
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Text {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: root.email.subject || ""
                    color: Theme.inkStrong
                    font.family: Theme.fontUi
                    font.pixelSize: 20
                    font.weight: Font.Bold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: root.email.sender || ""
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Text {
                    // Raw ISO string as-is for v1 -- no date-formatting
                    // library decision needed this task (see Task 35 brief);
                    // follow-up: format this for display in a later task.
                    textFormat: Text.PlainText
                    text: root.email.atUtc || ""
                    color: Theme.ink
                    opacity: 0.6
                    font.family: Theme.fontUi
                    font.pixelSize: 11
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6
            visible: (root.email.keywords || []).length > 0

            Repeater {
                model: root.email.keywords || []
                delegate: PillTab {
                    // Non-interactive display mode: always selected:true,
                    // no onClicked wired up -- a stray tap just plays
                    // PillTab's own press-opacity animation with no effect,
                    // which reads fine for a read-only chip.
                    text: modelData
                    selected: true
                }
            }
        }

        // Action row -- icon-only buttons (a text-labelled six-button row
        // read as too heavy for this row) with tooltips carrying the label
        // instead. Order/grouping unchanged from the original brief:
        // Reply/Reply All/Forward are non-destructive (Reply gets the
        // "primary" treatment as the single most likely next action, the
        // rest "ghost"); Delete alone is "danger". Pop-out is Desktop-only
        // (General.isDesktopMode) -- popping out a message on Mobile has no
        // separate-window concept to detach into. Centered via
        // Layout.alignment rather than left-anchored -- a RowLayout sized
        // to its own content (no Layout.fillWidth) so the alignment has
        // slack to center within.
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 8

            IconButton {
                icon: "mail-reply-sender"
                tooltip: i18n("Reply")
                variant: "primary"
                enabled: !MailApp.isBusy
                onClicked: {
                    const to = root.extractAddress(root.email.sender)
                    const subject = root.withPrefix(root.email.subject, "Re:")
                    const body = "\n\n" + i18n("%1 wrote:", root.email.sender) + "\n" + root.email.preview
                    root.composeRequested(to, subject, body)
                }
            }
            IconButton {
                icon: "mail-reply-all"
                tooltip: i18n("Reply All")
                enabled: !MailApp.isBusy
                onClicked: {
                    const addrs = [root.extractAddress(root.email.sender)]
                        .concat(root.splitAddresses(root.email.sentTo).map(root.extractAddress))
                        .concat(root.splitAddresses(root.email.cc).map(root.extractAddress))
                    const seen = {}
                    const deduped = []
                    for (let i = 0; i < addrs.length; i++) {
                        const a = addrs[i]
                        if (a.length > 0 && !seen[a]) {
                            seen[a] = true
                            deduped.push(a)
                        }
                    }
                    const subject = root.withPrefix(root.email.subject, "Re:")
                    const body = "\n\n" + i18n("%1 wrote:", root.email.sender) + "\n" + root.email.preview
                    root.composeRequested(deduped.join(", "), subject, body)
                }
            }
            IconButton {
                icon: "mail-forward"
                tooltip: i18n("Forward")
                enabled: !MailApp.isBusy
                onClicked: {
                    const subject = root.withPrefix(root.email.subject, "Fwd:")
                    const body = "\n\n" + i18n("---------- Forwarded message ----------")
                        + "\n" + i18n("From: %1", root.email.sender)
                        + "\n" + i18n("Subject: %1", root.email.subject)
                        + "\n\n" + root.email.preview
                    root.composeRequested("", subject, body)
                }
            }
            IconButton {
                icon: "mail-archive"
                tooltip: i18n("Archive")
                enabled: !MailApp.isBusy
                onClicked: MailApp.archiveEmails([root.messageId])
            }
            IconButton {
                icon: "mail-mark-junk"
                tooltip: i18n("Junk")
                enabled: !MailApp.isBusy
                onClicked: MailApp.markSpam([root.messageId])
            }
            IconButton {
                icon: "edit-delete"
                tooltip: i18n("Delete")
                variant: "danger"
                enabled: !MailApp.isBusy
                onClicked: MailApp.deleteEmails([root.messageId])
            }
            IconButton {
                // "Images blocked" affordance -- settings.autoLoadImages is
                // false by default below (tracking-pixel/read-receipt
                // protection, see that property's own comment); this is the
                // opt-in way back for a message the user trusts. Inline with
                // the other actions rather than its own row, so it reads as
                // one more toolbar action instead of a separate banner.
                icon: "image-x-generic"
                tooltip: i18n("Show images")
                visible: !root.imagesLoaded && root.hasRenderableBody
                onClicked: root.showImages()
            }
            IconButton {
                icon: "window-new"
                tooltip: i18n("Open in New Window")
                visible: General.isDesktopMode && !root.isPoppedOut
                onClicked: root.popOutRequested()
            }
        }

        // Refused-link notice. A click that does nothing at all reads as a
        // broken app and invites the user to go find the link some other
        // way, which is exactly the outcome the refusal is trying to avoid.
        MutedHint {
            Layout.fillWidth: true
            visible: root.blockedLinkScheme !== ""
            wrapMode: Text.WordWrap
            text: i18n("That link uses an address type KyPost will not open (%1:). It was blocked.",
                       root.blockedLinkScheme)
        }

        Text {
            Layout.fillWidth: true
            visible: !root.imagesLoaded && root.hasRenderableBody
            text: i18n("Images are hidden to protect your privacy.")
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        // Anti-phishing banner, above the PGP one because it is the more urgent
        // thing to read: PGP describes how a message was protected, this says
        // the message is trying to take over this device.
        //
        // Driven by the $Phishing IMAP keyword the server sets (see
        // backend/internal/processor/phish_scan.go). Advisory only -- the actual
        // refusal already happened locally and unconditionally, in
        // Format.isExternallyOpenableUrl below, which is what keeps this banner
        // from being load-bearing: if the server never flagged the message, the
        // kypost:// link still does not open.
        //
        // Deliberately shaped like pgpBanner's problem state rather than
        // extracted into a shared component: factoring the two together would
        // mean reworking the PGP banner's neutral/problem split for no
        // behavioural gain.
        Rectangle {
            id: phishingBanner

            readonly property bool flagged:
                Format.hasPhishingKeyword(root.email ? root.email.keywords : undefined)

            Layout.fillWidth: true
            visible: phishingBanner.flagged
            implicitHeight: visible ? phishingBannerLayout.implicitHeight + 24 : 0
            radius: Theme.shapePanel
            color: Theme.dangerFillColor
            border.width: 1
            border.color: Theme.dangerBorderColor

            ColumnLayout {
                id: phishingBannerLayout
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 6

                Text {
                    Layout.fillWidth: true
                    text: i18n("This message impersonates KyPost")
                    color: Theme.inkStrong
                    font.family: Theme.fontUi
                    font.pixelSize: 13
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: i18n("Links to KyPost app addresses have been blocked. KyPost will never "
                               + "ask you to confirm a pairing request by email — never approve "
                               + "one you did not start yourself, on this device.")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
        }

        // OpenPGP protection-mode banner. Every string and the decision
        // behind it come from C++ (MailController::findByMessageId ->
        // app/mail/PgpMessagePresentation.h -> core/domain/PgpMessageState.h),
        // so this file never re-derives the rule -- it only picks whether to
        // show the block and which accent to paint it.
        //
        // pgpState mirrors the PgpMessageState enum: 0 None, 1
        // ClientProtected, 2 DecryptFailed, 3 DecryptedByServer.
        Rectangle {
            id: pgpBanner

            readonly property int state: root.email.pgpState !== undefined ? root.email.pgpState : 0
            readonly property bool isProblem: (state === 1 || state === 2)
                                              || (root.hasDecryptedBody && MailApp.decryptedSignatureIsWarning)

            Layout.fillWidth: true
            // Stays up after a successful decryption too: the signature line
            // lives here, and it is the one thing about a decrypted message
            // the reader cannot work out for themselves.
            visible: (pgpBanner.state !== 0 && root.email.pgpBannerTitle)
                     || (root.hasDecryptedBody && !!MailApp.decryptedSignature)
            implicitHeight: visible ? pgpBannerLayout.implicitHeight + 24 : 0
            radius: Theme.shapePanel
            // A problem state gets the warning accent; "the server decrypted
            // this" is disclosure, not a fault, so it stays neutral.
            color: pgpBanner.isProblem ? Theme.dangerFillColor : Theme.panel
            border.width: 1
            border.color: pgpBanner.isProblem ? Theme.dangerBorderColor : Theme.line

            ColumnLayout {
                id: pgpBannerLayout
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 6

                Text {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: root.email.pgpBannerTitle || ""
                    color: Theme.inkStrong
                    font.family: Theme.fontUi
                    font.pixelSize: 13
                    font.bold: true
                    wrapMode: Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: root.email.pgpBannerBody || ""
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                // Why the decryption is not automatic: it prompts pinentry,
                // and for a hardware token that means the user has to
                // physically touch the key. That belongs to a deliberate
                // action, not to a list selection changing.
                Text {
                    objectName: "signatureLabel"
                    Layout.fillWidth: true
                    // PlainText, and the sentence comes from C++. It names the
                    // address the RELAY resolved from the From header -- never
                    // the display form, which the sender chooses freely and
                    // which this app does not parse.
                    textFormat: Text.PlainText
                    visible: root.hasDecryptedBody && !!MailApp.decryptedSignature
                    text: MailApp.decryptedSignature || ""
                    color: MailApp.decryptedSignatureIsWarning ? Theme.dangerBorderColor : Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                    font.bold: MailApp.decryptedSignatureIsWarning
                    wrapMode: Text.WordWrap
                }

                PrimaryButton {
                    objectName: "decryptButton"
                    // C++ decides: ClientProtected AND a usable gpg on this
                    // machine. Already decrypted, or busy, and it goes away.
                    visible: !!root.email.canDecryptHere && !root.hasDecryptedBody
                             && !MailApp.decryptBusy
                    text: i18n("Decrypt with your key")
                    onClicked: MailApp.decryptMessage(root.messageId)
                }

                Text {
                    objectName: "decryptBusyLabel"
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    visible: MailApp.decryptBusy
                    text: i18n("Waiting for your key…")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                Text {
                    objectName: "decryptFailureLabel"
                    Layout.fillWidth: true
                    // PlainText, not AutoText: this sentence is chosen by
                    // C++, but it sits beside relay-influenced content and
                    // Text.AutoText is banned in this repo for that reason.
                    textFormat: Text.PlainText
                    visible: !!MailApp.decryptFailure && !MailApp.decryptBusy
                    text: MailApp.decryptFailure || ""
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                PrimaryButton {
                    objectName: "decryptRetryButton"
                    // Offered only where retrying can actually change the
                    // answer. C++ owns that classification -- every other
                    // failure returns the same result however often it is
                    // asked.
                    visible: MailApp.decryptRetryable && !MailApp.decryptBusy
                    text: i18n("Try again")
                    onClicked: MailApp.decryptMessage(root.messageId)
                }

                PrimaryButton {
                    // C++ leaves webmailUrl empty unless the message really
                    // is unreadable here AND the pairing has a usable https
                    // base URL, so an emptiness test is the whole rule.
                    // Still offered alongside decryption: a key kept on
                    // another machine is an ordinary setup, and decrypting
                    // here cannot help there.
                    visible: !!root.email.webmailUrl && !root.hasDecryptedBody
                    text: i18n("Open in webmail")
                    onClicked: Qt.openUrlExternally(root.email.webmailUrl)
                }
            }
        }
        } // aboveWebView

        // Loader, not a directly-embedded WebEngineView -- active only once
        // there's a real message to show. QtWebEngine's view owns a native
        // compositor surface that isn't always fully suppressed by a plain
        // `visible: false` on an ancestor (observed as a stray rendered
        // rectangle showing through on the empty "Select an email" state);
        // not instantiating it at all until there's content to render is a
        // more robust fix than fighting that visibility quirk, and it also
        // means an idle detail pane isn't keeping a full web-rendering
        // process alive for nothing.
        Loader {
            id: webViewLoader
            Layout.fillWidth: true
            // Not a plain fillHeight -- this sits inside contentColumn,
            // which is wrapped in a content-sized Flickable rather than a
            // bounded parent, so there's no ambient "remaining space" to
            // fill implicitly. Instead this computes it explicitly: the
            // pane's real height (flickable.height, kept in sync with
            // whatever hosts this component via that anchors.fill: parent)
            // minus everything else sharing contentColumn, floored at 360
            // so a long aboveWebView/attachments section still leaves the
            // body a usable minimum rather than being squeezed to nothing
            // (the outer Flickable takes over and scrolls in that case).
            Layout.preferredHeight: active
                ? Math.max(360, flickable.height
                    - aboveWebView.implicitHeight
                    - (attachmentsColumn.visible ? attachmentsColumn.implicitHeight : 0)
                    - contentColumn.spacing * (attachmentsColumn.visible ? 2 : 1)
                    - contentColumn.anchors.margins * 2)
                : 0
            active: root.messageId !== ""

            function applyContent() {
                if (!item)
                    return
                // The decrypted body wins when there is one for THIS
                // message: the cached row for a client-protected message has
                // no body at all, so the alternative is a blank page.
                if (root.hasDecryptedBody) {
                    item.awaitingInitialLoad = true
                    item.loadHtml(root.renderedHtml(root.decryptedBody, !root.decryptedIsHtml))
                    return
                }
                const source = (root.email && root.email.body) ? root.email.body
                                                                 : (root.email ? root.email.preview : "")
                // Whether this is HTML comes from the server's own MIME parse
                // (`bodyMode`, carried on every inbox row), not from sniffing
                // the characters. The sniff is still the fallback for a row
                // cached before this client stored the mode, and only then --
                // see Format.emailBodyIsHtml for what the guess costs.
                const isHtml = Format.emailBodyIsHtml(root.email ? root.email.bodyMode : "", source || "")
                // Rearm the one-shot gate in onNavigationRequested below
                // before triggering the loadHtml() call that it needs to
                // let through.
                item.awaitingInitialLoad = true
                item.loadHtml(root.renderedHtml(source || "", !isHtml))
            }

            Connections {
                target: Theme
                function onThemeChanged() { webViewLoader.applyContent() }
            }

            sourceComponent: WebEngineView {
                backgroundColor: Theme.bg

                // Email body HTML is untrusted content -- it comes from
                // whatever sender wrote the message, not from this app or
                // the paired relay server. Two settings changed from
                // WebEngineView's defaults to close the two classic
                // mail-client HTML risks: running the sender's JavaScript,
                // and auto-fetching remote <img> sources (a standard
                // tracking-pixel/read-receipt leak -- it would fire on every
                // open even though the HTML itself is rendered via
                // loadHtml(), since <img src="https://..."> is still a real
                // network fetch regardless of the base content being
                // local). JavaScript is never needed for anything this view
                // does (link clicks are already intercepted below via
                // navigationRequested/openUrlExternally, not JS);
                // autoLoadImages follows root.imagesLoaded so the "Show
                // images" affordance above can opt back in per-message.
                settings.javascriptEnabled: false
                settings.autoLoadImages: root.imagesLoaded

                // VibeSec fix: settings.autoLoadImages above only gates
                // Blink's "Image" resource-loading policy -- a sender's
                // <link rel="stylesheet">, CSS @import, or <video>/<audio>
                // source fired a tracking-pixel-equivalent remote request
                // even with autoLoadImages false, since those are separate
                // "Stylesheet"/"Media" policies AutoLoadImages doesn't
                // touch. This profile's interceptor blocks every request
                // except the initial document load while imagesLoaded is
                // false, closing that gap; see RemoteContentInterceptor's
                // own class doc comment.
                // QQuickWebEngineProfile.setUrlRequestInterceptor() is a
                // plain C++ method in Qt6 (not a Q_PROPERTY as it was in
                // Qt5's QML API), so it can't be assigned declaratively --
                // wired up imperatively via RemoteContentInterceptor's own
                // installOn() Q_INVOKABLE instead.
                profile: WebEngineProfile {
                    id: emailProfile
                    Component.onCompleted: contentInterceptor.installOn(emailProfile)
                }

                property RemoteContentInterceptor contentInterceptor: RemoteContentInterceptor {
                    imagesLoaded: root.imagesLoaded
                }

                // VibeSec fix: this used to only reject LinkClickedNavigation,
                // leaving every other navigationType -- including a
                // RedirectNavigation/OtherNavigation triggered by an
                // in-message `<meta http-equiv="refresh" ...>` (no
                // JavaScript required) -- free to auto-navigate in place.
                // That let a sender's HTML silently fetch an attacker URL
                // the instant the message was opened, exactly the
                // read-receipt/tracking leak autoLoadImages above is meant
                // to prevent, just via a different navigation vector. Only
                // the single navigationRequested that applyContent()'s own
                // loadHtml() call produces (flagged via awaitingInitialLoad,
                // reset there right before each loadHtml()) is allowed
                // through now; every other navigation is rejected, and a
                // real link click is additionally routed to the system
                // browser.
                property bool awaitingInitialLoad: true

                onNavigationRequested: function (request) {
                    if (request.navigationType === WebEngineNavigationRequest.LinkClickedNavigation) {
                        request.reject()
                        // Scheme allowlist, not a bare handoff. This URL is
                        // written by whoever sent the message, and
                        // Qt.openUrlExternally hands it to xdg-open, which
                        // dispatches on scheme to whatever handler the
                        // desktop has registered. This app registers itself
                        // for kypost:// (packaging's .desktop MimeType), so
                        // an <a href="kypost://native-pair?...&srv=evil">
                        // in a message used to bounce straight back into
                        // PairingController and raise this app's own
                        // pairing-confirm dialog for an attacker's server --
                        // phishing wearing the trusted UI. Every other
                        // scheme the session happens to have a handler for
                        // was reachable the same way.
                        if (root.isExternallyOpenable(request.url))
                            Qt.openUrlExternally(request.url)
                        else
                            root.blockedLinkScheme = Format.urlScheme(request.url)
                        return
                    }
                    if (awaitingInitialLoad) {
                        awaitingInitialLoad = false
                        return
                    }
                    request.reject()
                }

                Component.onCompleted: webViewLoader.applyContent()
            }
        }

        ColumnLayout {
            id: attachmentsColumn
            Layout.fillWidth: true
            spacing: 8
            visible: root.email.hasAttachments === true

            SectionLabel { text: i18n("Attachments") }

            Flow {
                Layout.fillWidth: true
                spacing: 8

                Repeater {
                    model: root.attachments
                    delegate: Rectangle {
                        radius: Theme.shapeButton
                        color: Theme.panel
                        border.width: 1
                        border.color: Theme.line
                        implicitWidth: chipRow.implicitWidth + 24
                        implicitHeight: chipRow.implicitHeight + 16

                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 8

                            Text {
                                textFormat: Text.PlainText
                                text: i18n("%1 (%2)", modelData.name, root.formatSize(modelData.size))
                                color: Theme.inkStrong
                                font.family: Theme.fontUi
                                font.pixelSize: 12
                            }
                        }

                        TapHandler {
                            // Fire and forget -- the outcome is reported by
                            // root's onAttachmentDownloaded handler, which
                            // has to live up there rather than here: the
                            // status line outlives this chip, and the chip
                            // does not survive a reload().
                            onTapped: MailApp.downloadAttachment(
                                root.folder, root.messageId, modelData.index, modelData.name)
                        }
                    }
                }
            }

            Text {
                textFormat: Text.PlainText
                text: root.attachmentStatus
                visible: root.attachmentStatus !== ""
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 12
            }
        }
        } // contentColumn
    } // flickable

    // No dedicated Toast component exists yet (Task 35 brief allows either
    // choice) -- a plain Text that self-clears via this Timer is enough for
    // a one-off "Saved to Downloads"/error line.
    Timer {
        id: attachmentStatusTimer
        interval: 3000
        onTriggered: root.attachmentStatus = ""
    }
}
