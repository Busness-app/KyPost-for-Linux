import QtQuick
import QtTest

import "../../app/qml/utils/format.js" as Format

// The two rules that decide what a client-decrypted OpenPGP message renders
// as. Both are security controls, and both live in format.js precisely so
// they can be exercised without EmailDetail.qml's singleton graph.
TestCase {
    name: "DecryptedBody"

    // MailApp is a singleton holding at most ONE plaintext. Without the id
    // comparison, a decrypted message renders under the next message's
    // headers the moment the reader moves on.
    function test_bodyIsShownOnlyForTheMessageItBelongsTo() {
        const mine = Format.decryptedBodyFor("m1", "m1", "", "secret text")
        compare(mine.body, "secret text")

        const someoneElses = Format.decryptedBodyFor("m2", "m1", "", "secret text")
        compare(someoneElses.body, "", "another message's plaintext was offered for m2")
        compare(someoneElses.isHtml, false)
    }

    // The empty-selection state must not match an empty held id and let a
    // body through on a view showing nothing.
    function test_noMessageSelectedShowsNothing() {
        compare(Format.decryptedBodyFor("", "", "", "text").body, "")
        compare(Format.decryptedBodyFor(undefined, undefined, "<b>x</b>", "").body, "")
        compare(Format.decryptedBodyFor(null, null, "", "text").body, "")
    }

    function test_htmlWinsAndIsMarkedAsHtml() {
        const both = Format.decryptedBodyFor("m1", "m1", "<b>rich</b>", "flat")
        compare(both.body, "<b>rich</b>")
        compare(both.isHtml, true)
    }

    // isHtml comes from which field C++ populated -- taken from the part's
    // MIME Content-Type -- never from the characters. A plain-text message
    // that happens to contain markup is still plain text.
    function test_plainTextIsNeverPromotedToHtmlByItsContent() {
        const plain = Format.decryptedBodyFor("m1", "m1", "", "<html><body>not markup</body></html>")
        compare(plain.isHtml, false, "plain text was reported as HTML because of its content")
    }

    // ...and the renderer must honour that rather than sniffing again.
    function test_forcedPlainTextIsEscapedNotRendered() {
        const html = Format.renderedEmailHtml("<html><body>not markup</body></html>", false, "", true)
        verify(html.indexOf("&lt;html&gt;") !== -1,
               "plain text was rendered as markup despite the sender's Content-Type")
        verify(html.indexOf("<pre>") !== -1)
    }

    // The existing three-argument callers must be untouched by that parameter.
    function test_theSniffingPathIsUnchangedForEveryOtherCaller() {
        const doc = Format.renderedEmailHtml("<html><body>real</body></html>", false, "")
        verify(doc.indexOf("&lt;html&gt;") === -1, "an HTML message was escaped")
    }
}
