import QtQuick 2.15
import QtTest 1.15
import "qrc:/qml/utils/format.js" as Format

// The $Phishing IMAP keyword is how the server tells this client a message
// impersonates KyPost (see backend/internal/processor/phish_scan.go). The
// warning banner in EmailDetail reads it through this predicate.
//
// Advisory only: the actual refusal is Format.isExternallyOpenableUrl, which
// blocks the kypost:// link whether or not the server ever flagged anything.
TestCase {
    name: "PhishingFlag"

    function test_recognizes_the_server_keyword() {
        verify(Format.hasPhishingKeyword(["Primary", "$Phishing"]))
    }

    // IMAP keywords are case-insensitive, so a server may hand back a
    // different case than the one the poller set. A case-sensitive check would
    // silently drop the banner on exactly the mail it exists for.
    function test_matching_is_case_insensitive() {
        verify(Format.hasPhishingKeyword(["$phishing"]))
        verify(Format.hasPhishingKeyword(["$PHISHING"]))
        verify(Format.hasPhishingKeyword(["$PhIsHiNg"]))
    }

    function test_surrounding_whitespace_is_tolerated() {
        verify(Format.hasPhishingKeyword(["  $Phishing  "]))
    }

    function test_ordinary_mail_is_not_flagged() {
        verify(!Format.hasPhishingKeyword(["Primary", "Receipts"]))
    }

    function test_partial_matches_do_not_count() {
        verify(!Format.hasPhishingKeyword(["$PhishingReport"]))
        verify(!Format.hasPhishingKeyword(["NotPhishing"]))
    }

    // The relay omits the key entirely for mail with no keywords, and
    // Email::keywords starts empty, so both degenerate shapes reach here.
    function test_degenerate_input_is_not_flagged() {
        verify(!Format.hasPhishingKeyword([]))
        verify(!Format.hasPhishingKeyword(undefined))
        verify(!Format.hasPhishingKeyword(null))
    }
}
