import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtMultimedia
import com.kysecurity.mail 1.0
import "../components"

// PGP QR key exchange (Client_PGP_Update.md): scans someone else's "My PGP
// QR Code" screen via the camera, shows their fingerprint for out-of-band
// confirmation, then hands the (name, publicKey) pair back to the host via
// keyScanned() -- same "don't assume how the host persists this" shape as
// every other page component here (Compose's sendSucceeded, EmailDetail's
// actionCompleted). The host decides what to do with it: ContactsList's
// entry point creates a brand-new contact; ContactDetail's entry point
// writes straight into its own still-open form fields instead (see
// ContactDetail.qml's applyScannedKey()).
//
// Decoding is ZXingQt via PgpQrScanner (app/pgp/), attached to this
// VideoOutput's own videoSink -- see PgpQrScanner.h's doc comment for why
// that's a separate small C++ class rather than anything QML-only.
Item {
    id: root

    signal closed()
    signal keyScanned(string name, string publicKey)

    implicitWidth: 360
    implicitHeight: 640

    Component.onCompleted: PgpQr.clearScanResult()

    function trySave() {
        root.keyScanned(PgpQr.scannedName, PgpQr.scannedPublicKey)
    }

    CaptureSession {
        camera: Camera {
            active: PgpQr.scannedFingerprint === ""
        }
        videoOutput: preview
    }

    PgpQrScanner {
        id: scanner
        onDecoded: (text) => PgpQr.scanQrPayload(text)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: i18n("Scan to Add Contact Key")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            GhostButton {
                text: i18n("Close")
                onClicked: root.closed()
            }
        }

        // Camera preview -- hidden once a key has been scanned (the
        // fingerprint-confirmation panel below replaces it).
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 280
            visible: PgpQr.scannedFingerprint === ""
            color: "black"
            radius: Theme.shapePanel
            clip: true

            VideoOutput {
                id: preview
                anchors.fill: parent
                Component.onCompleted: scanner.attachSink(preview.videoSink)
                Component.onDestruction: scanner.detachSink()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: PgpQr.lastError !== ""
            textFormat: Text.PlainText
            text: PgpQr.lastError
            color: Theme.dangerColor
            font.family: Theme.fontUi
            font.pixelSize: 13
            wrapMode: Text.WordWrap
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: PgpQr.scannedFingerprint !== ""
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: PgpQr.scannedName
                // VibeSec fix: Text.textFormat defaults to Text.AutoText,
                // which renders HTML-like content as rich text -- including
                // fetching <img src> remotely (Qt's own docs warn about
                // exactly this). scannedName is server-controlled and
                // renders the instant a scan completes, with no user
                // action -- an attacker-controlled server (any host that
                // otherwise honestly passes isSafeQrTarget) could return an
                // <img> tag pointing at an internal/metadata address and
                // get it fetched via Qt Quick's own network stack,
                // completely bypassing isSafeQrTarget. Explicit PlainText
                // closes that off; this is untrusted display data, never
                // meant to carry formatting.
                textFormat: Text.PlainText
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 16
                font.weight: Font.Bold
                wrapMode: Text.WordWrap
            }
            SectionLabel { text: i18n("Fingerprint") }
            Text {
                Layout.fillWidth: true
                text: PgpQr.scannedFingerprint
                // Same VibeSec fix as scannedName above.
                textFormat: Text.PlainText
                color: Theme.inkStrong
                font.family: Theme.fontMono
                font.pixelSize: 13
                wrapMode: Text.WrapAnywhere
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Confirm this fingerprint matches what they show you in person before saving.")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                GhostButton {
                    text: i18n("Scan again")
                    onClicked: PgpQr.clearScanResult()
                }
                Item { Layout.fillWidth: true }
                PrimaryButton {
                    text: i18n("Save")
                    onClicked: root.trySave()
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
