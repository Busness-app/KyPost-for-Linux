import QtQuick 2.15
import com.kysecurity.mail 1.0

// STYLE_GUIDE.md §4 "Empty state" (mirrors web's `.contacts-empty`,
// `.inbox-empty-state`): dashed 1px border in an accent-tinted line color,
// centered muted text, Theme.shapeEmptyState corner radius (10, distinct
// from the field/button/panel/sheet radii).
//
// Judgment call: QtQuick's Rectangle has no native dashed-border support
// (no CSS-style border-style), so the dashed outline is drawn on a Canvas
// -- part of the base QtQuick module, no extra dependency -- rather than
// reaching for QtQuick.Shapes, which isn't used anywhere else in this
// codebase and would add a new module surface for a single dashed rect.
//
// Judgment call: STYLE_GUIDE says "accent-tinted line color" without an
// exact value, so `dashColor` blends Theme.line and Theme.accent (70%
// line / 30% accent) rather than using Theme.line directly -- a
// full-strength accent tint felt too loud for a large empty-state border,
// but plain Theme.line wouldn't read as "accent-tinted" at all.
//
// Judgment call: implicitWidth/Height default to a reasonable placeholder
// size (STYLE_GUIDE doesn't pin dimensions, only colors/radius); callers
// embedding this in a list/panel are expected to override via anchors or
// explicit width/height.
Rectangle {
    id: root

    // Public API.
    property string text: ""

    readonly property color dashColor: Qt.rgba(
        Theme.line.r * 0.7 + Theme.accent.r * 0.3,
        Theme.line.g * 0.7 + Theme.accent.g * 0.3,
        Theme.line.b * 0.7 + Theme.accent.b * 0.3,
        1.0)

    implicitWidth: 240
    implicitHeight: 160

    color: "transparent"
    radius: Theme.shapeEmptyState

    Canvas {
        id: dashedBorder
        anchors.fill: parent

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()

            var w = width
            var h = height
            var r = Math.min(root.radius, w / 2, h / 2)

            ctx.strokeStyle = root.dashColor
            ctx.lineWidth = 1
            ctx.setLineDash([4, 3])

            ctx.beginPath()
            ctx.moveTo(r, 0.5)
            ctx.lineTo(w - r, 0.5)
            ctx.quadraticCurveTo(w, 0.5, w - 0.5, r)
            ctx.lineTo(w - 0.5, h - r)
            ctx.quadraticCurveTo(w - 0.5, h - 0.5, w - r, h - 0.5)
            ctx.lineTo(r, h - 0.5)
            ctx.quadraticCurveTo(0.5, h - 0.5, 0.5, h - r)
            ctx.lineTo(0.5, r)
            ctx.quadraticCurveTo(0.5, 0.5, r, 0.5)
            ctx.closePath()
            ctx.stroke()
        }

        Connections {
            target: root
            function onDashColorChanged() { dashedBorder.requestPaint() }
            function onRadiusChanged() { dashedBorder.requestPaint() }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 8

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            // Bound to root's width (not the Column's own implicitWidth,
            // which is itself derived from this Text's width when wrapping
            // -- binding it to Column.width instead created a circular
            // binding that silently collapsed the text to zero size).
            width: root.width - 32
            textFormat: Text.PlainText
            text: root.text
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 14
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }
}
