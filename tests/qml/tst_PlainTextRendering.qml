import QtQuick 2.15
import QtTest 1.15
import com.kysecurity.mail 1.0
import "qrc:/qml/components" as Components

// Regression coverage for the Text.AutoText default.
//
// QML's Text defaults to Text.AutoText, which runs Qt::mightBeRichText() and
// hands anything markup-shaped to the StyledText parser -- which supports
// <img src> and fetches it over the QML engine's own QNetworkAccessManager.
// That is a different network stack from the mail viewer's WebEngineView, so
// neither settings.autoLoadImages nor RemoteContentInterceptor can see it: an
// <img> tag in a Subject header beaconed the moment the inbox list laid out,
// before the message was ever opened, while the app displayed "Images are
// hidden to protect your privacy."
//
// Only two files in the whole QML tree used to set textFormat, both added by
// an earlier fix to the PGP QR pane. Every Text bound to wire or sender data
// must set it, so these tests assert the shipped components rather than
// synthetic ones -- a delegate that loses its textFormat fails here.
//
// Every reusable label component under app/qml/components/ is swept, not just
// the ones known to carry wire data today. This file used to assert only
// PillTab and Toast, and PrimaryButton -- which NotificationFolderDialog hands
// a raw IMAP mailbox path off /api/inbox -- passed CI with an AutoText label
// for as long as it existed. A generic `text` property is an open door: which
// caller walks through it is not this component's to know, so the whole family
// is pinned and the sweep is the thing that keeps it that way.
TestCase {
    id: testCase
    name: "PlainTextRendering"
    when: windowShown
    width: 300
    height: 200

    // The payload a sender would put in a Subject header, an attachment
    // filename, or a contact field. Points at a closed port: nothing should
    // fetch it, and if the parser ever does, that is the bug.
    readonly property string hostilePayload:
        "Your package has shipped <img src=\"http://127.0.0.1:1/beacon\" width=\"1\" height=\"1\">"

    Component {
        id: pillTabComponent
        Components.PillTab {}
    }

    Component {
        id: toastComponent
        Components.Toast {}
    }

    Component {
        id: primaryButtonComponent
        Components.PrimaryButton {}
    }

    Component {
        id: ghostButtonComponent
        Components.GhostButton {}
    }

    Component {
        id: dangerButtonComponent
        Components.DangerButton {}
    }

    Component {
        id: emptyStateComponent
        Components.EmptyState {}
    }

    Component {
        id: statusBadgeComponent
        Components.StatusBadge {}
    }

    // Walks an item tree and returns every Text-like descendant that has not
    // opted out of rich text. Structure-independent on purpose: these
    // components are free to be rearranged, but not to start parsing markup.
    function richTextCapableDescendants(item) {
        var offenders = []
        if (item === null || item === undefined)
            return offenders
        // textFormat is the marker for a Text (or Label, which derives from
        // it); anything else in the tree is irrelevant here.
        if (item.textFormat !== undefined && item.textFormat !== Text.PlainText)
            offenders.push(item)
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i)
            offenders = offenders.concat(richTextCapableDescendants(kids[i]))
        return offenders
    }

    // PillTab renders server-supplied keyword names (EmailDetail's keyword
    // chips bind straight to Email.keywords).
    function test_pill_tab_renders_keywords_as_plain_text() {
        var pill = createTemporaryObject(pillTabComponent, testCase, { text: hostilePayload })
        verify(pill !== null)
        compare(richTextCapableDescendants(pill).length, 0)
    }

    // Toast carries relay-supplied warning strings.
    function test_toast_renders_server_text_as_plain_text() {
        var toast = createTemporaryObject(toastComponent, testCase, { text: hostilePayload })
        verify(toast !== null)
        compare(richTextCapableDescendants(toast).length, 0)
    }

    // PrimaryButton renders raw IMAP mailbox paths: NotificationFolderDialog
    // builds one choice button per folder returned by foldersHolding(), which
    // is /api/inbox's `folder` column verbatim and has no character
    // restriction. Laying the dialog out was enough to issue the fetch.
    function test_primary_button_renders_wire_text_as_plain_text() {
        var button = createTemporaryObject(primaryButtonComponent, testCase, { text: hostilePayload })
        verify(button !== null)
        compare(richTextCapableDescendants(button).length, 0)
    }

    // GhostButton, DangerButton, EmptyState and StatusBadge share
    // PrimaryButton's `text` API and are interchangeable with it by design, so
    // the next caller to hand one a folder name, a contact name or a relay
    // error is the whole distance between latent and reachable.
    function test_ghost_button_renders_wire_text_as_plain_text() {
        var button = createTemporaryObject(ghostButtonComponent, testCase, { text: hostilePayload })
        verify(button !== null)
        compare(richTextCapableDescendants(button).length, 0)
    }

    function test_danger_button_renders_wire_text_as_plain_text() {
        var button = createTemporaryObject(dangerButtonComponent, testCase, { text: hostilePayload })
        verify(button !== null)
        compare(richTextCapableDescendants(button).length, 0)
    }

    function test_empty_state_renders_wire_text_as_plain_text() {
        var empty = createTemporaryObject(emptyStateComponent, testCase, { text: hostilePayload })
        verify(empty !== null)
        compare(richTextCapableDescendants(empty).length, 0)
    }

    function test_status_badge_renders_wire_text_as_plain_text() {
        var badge = createTemporaryObject(statusBadgeComponent, testCase, { text: hostilePayload })
        verify(badge !== null)
        compare(richTextCapableDescendants(badge).length, 0)
    }

    // The reason the rule exists, stated as a check: left to itself, Qt
    // reclassifies exactly this kind of string.
    function test_qt_still_defaults_to_autotext() {
        var probe = Qt.createQmlObject('import QtQuick 2.15; Text { text: "" }', testCase)
        verify(probe !== null)
        compare(probe.textFormat, Text.AutoText)
        verify(probe.textFormat !== Text.PlainText)
        probe.destroy()
    }
}
