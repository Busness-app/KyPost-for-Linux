import QtQuick 2.15
import com.kysecurity.mail 1.0

// STYLE_GUIDE.md §4 "Section eyebrow label" (mirrors web's
// `.sidebar-section-label`, `.contact-details-section-title`): small
// uppercase, letter-spaced text at ~72%-opacity Theme.inkStrong. Group
// headers only, not body copy -- no interactive behavior, so this
// component is a plain styled Text rather than a Rectangle wrapper.
//
// `text` is Text's own built-in property -- reused as-is rather than
// re-declared, same reasoning as `enabled` on the button components: QML
// forbids shadowing a property already defined on the base type.
//
// Judgment call: the ~72% opacity is applied via the Text element's own
// `opacity` property (0.72) rather than pre-multiplying alpha into a
// custom color -- keeps Theme.inkStrong's live theme-swap binding intact
// with one extra property, instead of recomputing an alpha-blended QColor
// by hand on every themeChanged().
//
// Judgment call: STYLE_GUIDE doesn't give an exact letter-spacing value --
// 1 (device-independent pixel) is used as a modest, legible spacing for
// small uppercase text; it's a one-off typographic detail, not a
// themeable color/radius, so it isn't sourced from Theme.
Text {
    id: root

    color: Theme.inkStrong
    opacity: 0.72
    font.family: Theme.fontUi
    font.pixelSize: 12
    font.weight: Font.Medium
    font.capitalization: Font.AllUppercase
    font.letterSpacing: 1
}
