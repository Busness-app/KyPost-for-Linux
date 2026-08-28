import QtQuick 2.15
import QtTest 1.15

// DesktopRoot has two compose seed callers: only the editor's own sanitized
// draft may opt into HTML. Keep the trust bit explicit at the window boundary.
TestCase {
    name: "ComposeSeeding"

    function test_popout_compose_preserves_body_trust() {
        var source = new XMLHttpRequest()
        source.open("GET", "qrc:/qml/DesktopRoot.qml", false)
        source.send()
        compare(source.status, 200)
        verify(source.responseText.indexOf(
            "initialBodyIsHtml: composeWindow.popInitialBodyIsHtml") !== -1)
        verify(source.responseText.indexOf(
            "root.popOutCompose(to || \"\", subject || \"\", body || \"\", false)") !== -1)
    }
}
