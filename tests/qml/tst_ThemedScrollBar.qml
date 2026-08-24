import QtQuick 2.15
import QtQuick.Controls 2.15
import QtTest 1.15
import "qrc:/qml/components" as Components

TestCase {
    name: "ThemedScrollBar"
    when: windowShown
    visible: true
    width: 320
    height: 120

    Flickable {
        id: pills
        anchors.fill: parent
        contentWidth: 200
        contentHeight: height
        ScrollBar.horizontal: Components.ThemedScrollBar { id: bar }
    }

    function test_horizontal_bar_is_thin_and_hidden_without_overflow() {
        tryCompare(bar, "height", bar.thickness)
        compare(bar.policy, ScrollBar.AsNeeded)
        verify(!bar.visible)
    }

    function test_horizontal_bar_appears_only_for_overflow() {
        pills.contentWidth = pills.width + 100
        tryCompare(bar, "visible", true)
        compare(bar.height, bar.thickness)
        pills.contentWidth = 200
    }
}
