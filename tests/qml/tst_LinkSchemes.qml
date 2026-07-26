import QtQuick 2.15
import QtTest 1.15
import "qrc:/qml/utils/format.js" as Format

// Regression coverage for the link-scheme allowlist.
//
// EmailDetail handed every clicked link straight to Qt.openUrlExternally,
// which dispatches on scheme to whatever handler the desktop registered.
// This app registers itself for kypost://, so a link in a message could
// route back into PairingController and raise this app's own pairing-confirm
// dialog naming an attacker's server -- phishing wearing the trusted UI.
TestCase {
    name: "LinkSchemes"

    function test_ordinary_web_and_mail_links_open() {
        verify(Format.isExternallyOpenableUrl("https://example.com/a"))
        verify(Format.isExternallyOpenableUrl("http://example.com/a"))
        verify(Format.isExternallyOpenableUrl("mailto:someone@example.com"))
        // Scheme matching is case-insensitive, as schemes are.
        verify(Format.isExternallyOpenableUrl("HTTPS://example.com"))
        verify(Format.isExternallyOpenableUrl("MailTo:someone@example.com"))
    }

    function test_own_scheme_is_refused() {
        // The attack this exists to stop.
        verify(!Format.isExternallyOpenableUrl(
                   "kypost://native-pair?sub=x&srv=https://evil.example&pt=y"))
    }

    function test_other_schemes_are_refused() {
        verify(!Format.isExternallyOpenableUrl("file:///etc/passwd"))
        verify(!Format.isExternallyOpenableUrl("smb://server/share"))
        verify(!Format.isExternallyOpenableUrl("javascript:alert(1)"))
        verify(!Format.isExternallyOpenableUrl("data:text/html,<script>"))
        verify(!Format.isExternallyOpenableUrl("ftp://example.com"))
    }

    function test_degenerate_input_is_refused() {
        verify(!Format.isExternallyOpenableUrl(""))
        verify(!Format.isExternallyOpenableUrl(undefined))
        verify(!Format.isExternallyOpenableUrl(null))
        verify(!Format.isExternallyOpenableUrl("no-scheme-at-all"))
        verify(!Format.isExternallyOpenableUrl("://example.com"))
    }

    function test_scheme_extraction_for_the_user_facing_notice() {
        compare(Format.urlScheme("kypost://native-pair?x=1"), "kypost")
        compare(Format.urlScheme("FILE:///tmp"), "file")
        compare(Format.urlScheme("nonsense"), "")
    }

    // The escaping used for reply/forward quote blocks, which is what keeps
    // a hostile sender's markup out of the compose editor's DOM.
    function test_html_escaping_of_quoted_text() {
        compare(Format.escapeHtml("<img src=x onerror=alert(1)>"),
                "&lt;img src=x onerror=alert(1)&gt;")
        compare(Format.escapeHtml("a & b"), "a &amp; b")
        compare(Format.escapeHtml(""), "")
        compare(Format.escapeHtml(undefined), "")
    }
}
