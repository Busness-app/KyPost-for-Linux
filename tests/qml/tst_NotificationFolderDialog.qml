import QtQuick 2.15
import QtTest 1.15
import com.urlxl.mail 1.0
import "qrc:/qml/components" as Components

// The dialog that answers a notification tap-through nothing else can.
//
// A push envelope carries no mailbox, and a message id is not unique across
// them, so an id cached in INBOX and Archive has no tie-breaker. Opening the
// wrong copy is a wrong-message bug; opening nothing is what happened until
// 2026-08-23, and gave the user a blank page for a question only they can
// answer.
//
// The property worth a test is that it does not answer for them.
TestCase {
    id: testCase
    name: "NotificationFolderDialog"
    when: windowShown
    visible: true
    width: 480
    height: 360

    Components.NotificationFolderDialog {
        id: dialog
        anchors.fill: parent
    }

    SignalSpy {
        id: chosenSpy
        target: dialog
        signalName: "chosen"
    }

    function init() {
        dialog.close()
        chosenSpy.clear()
    }

    function test_opening_offers_every_mailbox_and_chooses_none() {
        dialog.open("m-1", ["Archive", "INBOX"])

        verify(dialog.isOpen)
        compare(dialog.folders.length, 2)
        // Nothing is picked for the user. There is no safe default here --
        // every option is a real message, and guessing is the exact bug this
        // dialog exists to avoid.
        compare(chosenSpy.count, 0)
    }

    function test_choosing_reports_the_mailbox_and_the_message() {
        dialog.open("m-1", ["Archive", "INBOX"])
        dialog.choose("Archive")

        compare(chosenSpy.count, 1)
        compare(chosenSpy.signalArguments[0][0], "m-1")
        compare(chosenSpy.signalArguments[0][1], "Archive")
        verify(!dialog.isOpen)
    }

    // Dismissing opens nothing. A dialog that fell back to "the first one" on
    // cancel would turn an unanswered question into a silent wrong answer.
    function test_cancelling_opens_nothing() {
        dialog.open("m-1", ["Archive", "INBOX"])
        dialog.cancel()

        compare(chosenSpy.count, 0)
        verify(!dialog.isOpen)
    }

    // The id travels with the choice rather than being read back off the
    // dialog afterwards: close() clears it, and a handler that read it later
    // would get an empty string.
    function test_the_message_id_survives_being_closed_on_the_way_out() {
        dialog.open("m-42", ["Sent", "Trash"])
        dialog.choose("Sent")

        compare(chosenSpy.signalArguments[0][0], "m-42")
        compare(dialog.messageId, "")
    }
}
