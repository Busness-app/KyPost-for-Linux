import QtQuick 2.15
import QtTest 1.15
import "qrc:/qml/utils/format.js" as Format

// A display name is attacker-controlled and is authenticated by nothing: DKIM,
// SPF and DMARC all validate the domain a message was sent from, never the
// human-readable label in front of it. So this arrives intact and aligned:
//
//     From: "evil@attacker.tld" <bob@corp.com>
//
// Reply/Reply All/Forward feed from this extractor and carry the quoted
// original, so picking the wrong address out of a From header sends a thread to
// a party who never sent it.
//
// These six vectors are shared verbatim with the webmail
// (frontend/src/lib/addressText.test.ts) and Android clients, so all three
// agree that the real address is the LAST angle-addr.
TestCase {
    name: "AddressText"

    function test_ordinary_display_name_and_address() {
        compare(Format.addressFromHeader("Bob <bob@corp.com>"), "bob@corp.com")
    }

    // The confidentiality bug: the old rule took the FIRST "<", so a display
    // name dressed up as an angle-addr won.
    function test_address_planted_in_the_display_name_is_ignored() {
        compare(Format.addressFromHeader('"evil@attacker.tld" <bob@corp.com>'), "bob@corp.com")
    }

    // The mirror case: the attacker really is the sender, and the display name
    // mimics Bob. The reply must go where the mail actually came from.
    function test_real_address_wins_over_a_mimicking_display_name() {
        compare(Format.addressFromHeader('"Bob <bob@corp.com>" <evil@attacker.tld>'), "evil@attacker.tld")
    }

    function test_bare_address_passes_through() {
        compare(Format.addressFromHeader("bob@corp.com"), "bob@corp.com")
    }

    function test_comma_inside_a_quoted_display_name() {
        compare(Format.addressFromHeader('"a, b" <bob@corp.com>'), "bob@corp.com")
    }

    // Not address-shaped is not an address. Better to populate nothing than to
    // put a display name in a recipient field.
    function test_value_with_no_address_yields_empty() {
        compare(Format.addressFromHeader("Unknown sender"), "")
        compare(Format.addressFromHeader(""), "")
        compare(Format.addressFromHeader(undefined), "")
    }
}
