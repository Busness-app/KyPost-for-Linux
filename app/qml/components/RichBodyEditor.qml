import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtWebEngine
import com.kysecurity.mail 1.0
import "../utils/format.js" as Format

// Compose's rich HTML body editor (supersedes the earlier plain-TextArea-
// only constraint). The editing surface is a contenteditable WebEngineView.
// Unlike EmailDetail.qml's read-only viewer -- which sets
// settings.javascriptEnabled: false because sender HTML is untrusted -- this
// view intentionally enables JavaScript: the script running here is ours
// (execCommand calls + the sanitizer below), a different trust boundary than
// rendering someone else's mail.
Item {
    id: root

    implicitWidth: 360
    implicitHeight: 240

    // Seeds the editor's content. Call exactly once, right after
    // construction (Compose.qml's Component.onCompleted) -- there is no
    // "reload" support, callers only ever seed a fresh draft.
    function loadInitialHtml(html) {
        webView.loadHtml(root.shellHtml(html))
    }

    // Runs the sanitizer over the current DOM and invokes
    // callback({html, isEmpty}). Necessarily asynchronous: runJavaScript
    // crosses into WebEngine's separate render process, there is no
    // synchronous variant.
    function requestSendableHtml(callback) {
        webView.runJavaScript(root.sanitizerScript, callback)
    }

    function shellHtml(bodyHtml) {
        // html/body both get height: 100% + the panel background (not just
        // body, and not relying on WebEngineView.backgroundColor alone) --
        // body's own box only auto-sizes to its content, so on a short/empty
        // draft it doesn't reach the bottom of the WebEngineView's viewport.
        // The leftover strip below it was never painted by the page at all,
        // so the compositor showed whatever sits behind this window through
        // it (only visible live, not in a full-frame screenshot). padding
        // replaces the old margin so body still keeps its inset now that
        // it's stretched to 100% height.
        return "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"Content-Security-Policy\" content=\""
            + Format.emailContentSecurityPolicy(false) + "\"><style>"
            + "html, body { height: 100%; margin: 0; background: " + Theme.panel + "; }"
            + "body { font-family: sans-serif; font-size: 14px; padding: 8px; "
            + "box-sizing: border-box; color: " + Theme.inkStrong + "; }"
            + "</style></head><body contenteditable=\"true\">" + bodyHtml + "</body></html>"
    }

    // Confirmed the trigger is a genuine focus event, not a content
    // mutation (an opacity-toggle repaint nudge alone did NOT fix the
    // pop-out Compose window's transparency; clicking into the body -- a
    // real focus transfer -- does; typing worked too only because it also
    // focuses the field first). webView.forceActiveFocus() gives the
    // WebEngineView actual native/Wayland-level input focus (the part a
    // pure JS-side DOM mutation can't reach), and document.body.focus()
    // completes it by focusing the contenteditable element itself, matching
    // what a real click does. Fired once by startupFocusNudgeTimer below.
    readonly property string focusNudgeScript: "document.body.focus();"

    // Installed after every loadHtml() (see WebEngineView.onLoadingChanged
    // below) -- forces paste to plain text so arbitrary markup/scripts from
    // a webpage never enter the DOM in the first place. The sanitizer below
    // is a backstop, not the primary control.
    readonly property string pasteScript: "
        document.body.addEventListener('paste', function(event) {
            event.preventDefault();
            var text = (event.clipboardData || window.clipboardData).getData('text/plain');
            document.execCommand('insertText', false, text);
        });
    "

    // Allowlist DOM sanitizer: unwraps (keeps children, drops the tag) any
    // element outside the fixed tag list, strips every attribute except
    // href/style on <a> (further restricted below). Runs against a cloned
    // subtree so it never mutates what's on screen.
    readonly property string sanitizerScript: "
        (function() {
            var allowedTags = ['P', 'BR', 'DIV', 'B', 'STRONG', 'I', 'EM', 'U', 'A', 'BLOCKQUOTE'];
            var allowedStyleProps = ['color', 'background-color', 'padding', 'border-radius',
                                      'display', 'font-weight', 'text-decoration', 'border'];
            function isSafeUrl(url) {
                return /^(https?:|mailto:)/i.test(url);
            }
            function clean(node) {
                Array.from(node.childNodes).forEach(function(child) {
                    if (child.nodeType === Node.ELEMENT_NODE) {
                        clean(child);
                        if (allowedTags.indexOf(child.tagName) === -1) {
                            while (child.firstChild) node.insertBefore(child.firstChild, child);
                            node.removeChild(child);
                            return;
                        }
                        Array.from(child.attributes).forEach(function(attr) {
                            if (child.tagName === 'A' && attr.name === 'href') {
                                if (!isSafeUrl(child.getAttribute('href'))) child.removeAttribute('href');
                            } else if (child.tagName === 'A' && attr.name === 'style') {
                                var kept = attr.value.split(';').map(function(rule) {
                                    var prop = (rule.split(':')[0] || '').trim().toLowerCase();
                                    return allowedStyleProps.indexOf(prop) !== -1 ? rule.trim() : null;
                                }).filter(Boolean).join('; ');
                                if (kept) child.setAttribute('style', kept); else child.removeAttribute('style');
                            } else {
                                child.removeAttribute(attr.name);
                            }
                        });
                    } else if (child.nodeType !== Node.TEXT_NODE) {
                        node.removeChild(child);
                    }
                });
            }
            var clone = document.body.cloneNode(true);
            clean(clone);
            return { html: clone.innerHTML, isEmpty: document.body.textContent.trim() === '' };
        })();
    "

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            IconButton {
                icon: "format-text-bold"
                tooltip: i18n("Bold")
                onClicked: webView.runJavaScript("document.execCommand('bold')")
            }
            IconButton {
                icon: "format-text-italic"
                tooltip: i18n("Italic")
                onClicked: webView.runJavaScript("document.execCommand('italic')")
            }
            IconButton {
                icon: "format-text-underline"
                tooltip: i18n("Underline")
                onClicked: webView.runJavaScript("document.execCommand('underline')")
            }
            IconButton {
                icon: "insert-link"
                tooltip: i18n("Insert Link")
                onClicked: linkDialog.open()
            }
            Item { Layout.fillWidth: true }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.shapeField
            color: Theme.panel
            border.width: 1
            border.color: Theme.line
            // The actual reported bug: webView's native render surface was
            // spilling past this Rectangle's own bounds, down into the
            // sibling "Attach files"/Send button rows in Compose.qml (well
            // outside this component entirely) -- not a transparency
            // *inside* the editor's own box, which is what the earlier
            // fixes in this file targeted. clip masks anything painted
            // outside this Rectangle at composite time, regardless of
            // whether the overflowing content is ordinary QML or
            // WebEngineView's own texture.
            clip: true

            WebEngineView {
                id: webView
                anchors.fill: parent
                anchors.margins: 1
                backgroundColor: Theme.panel
                settings.javascriptEnabled: true

                onLoadingChanged: function(loadRequest) {
                    if (loadRequest.status === WebEngineView.LoadSucceededStatus) {
                        webView.runJavaScript(root.pasteScript)
                        // Confirmed: the pop-out Compose window's
                        // transparency isn't tied to resizing at all (it can
                        // appear with no resize), and a real focus transfer
                        // (a manual click into the body) is what clears it.
                        // Fire that once, unconditionally, shortly after the
                        // page finishes loading -- not gated behind any
                        // resize event like earlier attempts. The delay
                        // gives the pop-out window's own initial
                        // show/geometry-negotiation a moment to settle
                        // first.
                        startupFocusNudgeTimer.restart()
                    }
                }
            }

            // Covers webView with a guaranteed-opaque, plain-QML-painted
            // surface until the startup focus nudge below has run.
            // WebEngineView's own native render surface doesn't reliably
            // composite correctly the instant it's shown (see
            // startupFocusNudgeTimer's comment) -- rather than accepting a
            // visible transparent flash while that settles, this sits on
            // top in the same Qt Quick scene (ordinary z-ordering, not
            // anything WebEngineView-specific) so the user only ever sees a
            // solid surface. It's painted the exact same Theme.panel color
            // the page itself uses, so hiding it once the nudge fires is a
            // seamless, imperceptible handoff -- solid from the very first
            // rendered frame, not just "fixed shortly after."
            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                color: Theme.panel
                visible: !startupFocusNudgeTimer.triggered
            }

            Timer {
                id: startupFocusNudgeTimer
                property bool triggered: false
                interval: 400
                onTriggered: {
                    webView.forceActiveFocus()
                    webView.runJavaScript(root.focusNudgeScript)
                    triggered = true
                }
            }
        }
    }

    HyperlinkDialog {
        id: linkDialog
        z: 10
        anchors.fill: parent
        onLinkConfirmed: function(label, url, asButton) {
            const style = asButton
                ? " style=\"display:inline-block;padding:10px 20px;border-radius:" + Theme.shapeButton
                    + "px;background-color:" + Theme.accent + ";color:" + Theme.readableOnAccent
                    + ";text-decoration:none;font-weight:600;\""
                : ""
            const escapedLabel = label.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
            const escapedUrl = url.replace(/&/g, "&amp;").replace(/"/g, "&quot;")
            const html = "<a href=\"" + escapedUrl + "\"" + style + ">" + escapedLabel + "</a>"
            webView.runJavaScript("document.execCommand('insertHTML', false, " + JSON.stringify(html) + ")")
        }
    }
}
