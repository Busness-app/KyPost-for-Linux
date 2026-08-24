import QtQuick 2.15
import com.kysecurity.mail 1.0

// The app-lock PIN gate, as one drop-in item.
//
// Exists as a component because the overlay used to be written out by hand,
// once, anchored to DesktopRoot's own contentItem -- while DesktopRoot also
// creates genuine top-level Windows for popped-out email, compose and
// contact views via createObject(null, ...). Those windows are not children
// of root, so `anchors.fill: parent` never covered them: popping an email
// out and then letting the app lock (minimise, hide-to-tray, window state
// change) left the message on screen, readable and fully interactive --
// archive, delete, reply, download attachments -- with the PIN prompt
// dutifully covering the main window next to it. That defeated the entire
// "Require Unlock to Open" feature with one click.
//
// Every Window that can show mail or contact data must instantiate one of
// these. Keeping it in one file means the next pop-out someone adds gets the
// gate by construction rather than by remembering.
Loader {
    anchors.fill: parent
    // Above everything else in its window. Toast uses 900; this must sit
    // over that too, or a notification could render on top of the PIN
    // screen.
    z: 1000
    active: AppLock.locked
    visible: active
    // Absolute qrc path, not "../pages/Unlock.qml": the app always loads its
    // QML from the resource bundle, and a relative source here resolves
    // against whichever context instantiated the component, which breaks the
    // moment this is used from anywhere but app/qml/components/.
    source: "qrc:/qml/pages/Unlock.qml"
}
