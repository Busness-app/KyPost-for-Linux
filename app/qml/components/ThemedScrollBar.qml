import QtQuick 2.15
import QtQuick.Controls 2.15
import com.kysecurity.mail 1.0

// Themed replacement for the default platform ScrollBar -- thin capsule thumb
// colored from Theme.line/Theme.accent (same tokens every other component
// uses), no boxed track. `interactive` is bound to General.isDesktopMode
// rather than left at its default (Qt Quick Controls' own heuristic already
// picks a sane default per-platform, but this app already has an explicit,
// user-facing Desktop/Mobile mode toggle -- General.isDesktopMode -- so this
// reuses that instead of a second, possibly-inconsistent signal): a
// draggable thumb on Desktop (mouse/trackpad idiom), a passive flick-only
// indicator on Mobile (matches MobileRoot.qml's existing SwipeDelegate touch
// pattern). One component, correct in both hosts, since the page components
// wrapping content in a Flickable/ListView (EmailDetail/ContactDetail/
// Compose/ContactsList/Settings/etc.) are shared between DesktopRoot.qml and
// MobileRoot.qml.
ScrollBar {
    id: root

    readonly property real thickness: 4

    policy: ScrollBar.AlwaysOn
    interactive: General.isDesktopMode

    background: null

    contentItem: Rectangle {
        radius: root.thickness / 2
        color: root.pressed ? Theme.accent : Theme.line
        // Faintly visible at rest whenever there's actually more content to
        // reach (size < 1.0), not just during interaction -- a thumb that's
        // fully invisible until the user already knows to hover/scroll
        // doesn't solve "no indicator", it just relocates the problem.
        // Brightens on hover/press/active-scroll for the usual overlay-
        // scrollbar feel.
        opacity: root.size >= 1.0 ? 0.0 : (root.active || root.hovered || root.pressed ? 0.9 : 0.35)

        Behavior on color {
            ColorAnimation { duration: 120 }
        }
        Behavior on opacity {
            NumberAnimation { duration: 120 }
        }
    }

    // Forces the CROSS-axis thickness with a real (not implicit) property
    // assignment, which always wins over whatever the active QQC2 style's
    // own ScrollBar template computes -- testing showed the default/implicit
    // sizing route producing a handle that covered an entire horizontal
    // pill row's height instead of a thin 4px line. The ALONG-axis
    // dimension (width for horizontal, height for vertical) is left alone:
    // that one is already driven live by the Flickable's own attachment
    // machinery when this is assigned via `ScrollBar.horizontal:`/
    // `ScrollBar.vertical:`, and must keep tracking the flickable's size as
    // it changes -- only orientation, fixed once at creation, decides which
    // axis this component pins.
    Component.onCompleted: {
        if (orientation === Qt.Horizontal)
            height = thickness
        else
            width = thickness
    }
}
