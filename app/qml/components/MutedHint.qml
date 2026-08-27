import QtQuick 2.15
import com.kysecurity.mail 1.0

// Small muted single-line hint/status text, e.g. "No groups available".
// Plain styled Text, same shape as SectionLabel.qml (this file's closest
// sibling) -- `text` is Text's own built-in property, reused as-is rather
// than re-declared.
Text {
    id: root

    // The text comes from callers, and some of them pass relay strings
    // (a blocked link's scheme, an enrollment status). Pinned here so
    // no call site has to remember.
    textFormat: Text.PlainText
    color: Theme.ink
    font.family: Theme.fontUi
    font.pixelSize: 12
}
