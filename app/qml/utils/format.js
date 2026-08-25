.pragma library

// Escapes HTML metacharacters so text of unknown origin (email body, contact
// name synced from the Relay server/vCard import/PGP-QR scan, etc.) can't
// inject tags when placed inside a Text.RichText item. Was duplicated
// verbatim as escapeHtml() in EmailDetail.qml, Compose.qml, and
// AutocompleteDropdown.qml.
// Quotes are escaped as well as the angle brackets. Every current call site
// puts the result in text position, where they do not matter -- but the
// function is named for what it promises ("escape HTML"), not for its
// current callers, and the first person to write
// `'<a title="' + escapeHtml(name) + '">'` would otherwise reintroduce
// attribute injection with no warning from the name or this comment.
function escapeHtml(s) {
    return (s || "")
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#39;")
}

// ---- email body rendering ------------------------------------------------
//
// Both functions below live here rather than in EmailDetail.qml for the same
// reason isExternallyOpenableUrl does: EmailDetail needs QtWebEngine and the
// whole singleton graph to instantiate, and these two rules are the ones
// standing between a hostile message and the renderer. They are pure string
// functions and are tested directly (tests/qml/tst_EmailBodyRendering.qml).

// Whether `body` should be handed to the renderer as HTML rather than
// escaped and wrapped in <pre>.
//
// This is a sniff, and a sniff is the wrong mechanism: /api/inbox returns a
// single `body` string with no content-type marker, even though the server
// distinguishes the parts internally (imapadapter.Message has both Body and
// BodyHTML). Until the wire carries the type, the client has to guess.
//
// What it must NOT do is the previous rule, which matched any of seventeen
// tag names ANYWHERE in the string. A plain-text message quoting markup --
// a bug report, a snippet from a colleague, a phishing sample forwarded to
// IT -- was reclassified as HTML and rendered, so a literal
// `<img src="https://tracker/...">` typed into a text/plain mail became a
// live fetch the moment the user pressed "Show images".
//
// The rule here is "the body BEGINS as a document", which is what a real
// text/html part looks like and what quoted markup in prose does not: skip
// whitespace/BOM, an optional doctype, and any leading comments, then
// require a known tag at that position. Misclassifying HTML as text renders
// visible markup -- ugly, and safe; the inverse is the one that fetches.
var htmlLeadingTagRegex = /^<(?:html|head|body|div|p|br|table|span|ul|ol|h[1-6]|center|font|blockquote|pre|meta|style|title|section|article|main)\b/i

function looksLikeHtmlDocument(body) {
    var s = String(body === undefined || body === null ? "" : body)
    // Byte-order mark, then leading whitespace.
    s = s.replace(/^﻿/, "").replace(/^\s+/, "")
    // An optional doctype and any number of leading comments, in either
    // order, each followed by more whitespace.
    for (;;) {
        var before = s
        s = s.replace(/^<!doctype[^>]*>/i, "").replace(/^\s+/, "")
        s = s.replace(/^<!--[\s\S]*?-->/, "").replace(/^\s+/, "")
        if (s === before)
            break
    }
    return htmlLeadingTagRegex.test(s)
}

// Content-Security-Policy for the untrusted-sender document.
//
// Everything the email view renders is written by whoever sent the message.
// Three separate mechanisms already constrain it -- javascriptEnabled false,
// RemoteContentInterceptor, and onNavigationRequested -- and every one of
// them has needed a fix at least once. CSP is the only one of the four that
// Blink enforces itself, so it keeps holding if the interceptor is not
// installed on a future profile, if navigationRequested stops covering a
// frame type, or if a Qt upgrade changes any of that wiring.
//
// default-src 'none' is the point: every fetch directive not named below
// inherits it and is refused. style-src 'unsafe-inline' is required because
// the scaffold injects the theme's own <style> block, and because mail is
// built out of inline style= attributes -- for styles that keyword grants no
// script capability. img-src widens to the network only once the user has
// asked for images, mirroring the interceptor rather than trusting it.
//
// No `sandbox` directive: it is specified to be ignored when CSP arrives via
// <meta> rather than an HTTP header, and loadHtml() offers no way to set a
// header. Listing it would look like a control and be none.
function emailContentSecurityPolicy(imagesLoaded) {
    return "default-src 'none'; "
        + "style-src 'unsafe-inline'; "
        + (imagesLoaded ? "img-src http: https: data:; " : "img-src data:; ")
        + "frame-src 'none'; object-src 'none'; media-src 'none'; font-src 'none'; "
        + "connect-src 'none'; script-src 'none'; form-action 'none'; base-uri 'none'"
}

// Which form a message body is, when the server told us.
//
// `bodyMode` is the MIME Content-Type the server already parsed -- "html" or
// "plain" -- carried on every /api/inbox row and returned by /api/mail/body.
// Knowing beats guessing, and the guess is not merely imprecise: a plain-text
// message containing an RFC 5322 address like <user@example.com> parses as an
// unknown tag, so the address disappears from what the reader sees. That is
// data loss, in the direction of a message that looks like it said less than
// it did.
//
// Only an absent mode falls back to looksLikeHtmlDocument() -- an older relay,
// or a row this client cached before it stored the mode. Absent means "the
// server did not say", never "plain". Anything else that is not exactly
// "html" is treated as text: an unrecognised value must not be what turns a
// body into live HTML.
function emailBodyIsHtml(bodyMode, body) {
    var mode = String(bodyMode === undefined || bodyMode === null ? "" : bodyMode)
    if (mode === "html")
        return true
    if (mode === "")
        return looksLikeHtmlDocument(body)
    return false
}

// The full document handed to WebEngineView.loadHtml(). `style` is the
// caller's already-resolved theme CSS (colours come from a QML singleton
// this module deliberately does not import).
// forcePlainText is the caller's already-made decision about the body's form:
// true escapes, false renders as HTML. Omit it entirely and only then does
// this function guess, via looksLikeHtmlDocument().
//
// It used to only ever force *toward* escaping -- passing false still let the
// sniff overrule it -- so a message the server had parsed as text/html but
// which did not happen to open with a tag ("Hi.<br>...") was shown as source.
// A sender controls the sniff's outcome just as completely as the header's
// (open with "<div>" and it is HTML either way), so obeying the parse is not
// a wider door; it is the same door, correctly labelled.
//
// For a client-decrypted OpenPGP message the MIME Content-Type says which
// form the body is, and knowing beats guessing: a plain-text message that
// happens to contain "<html>" would otherwise be rendered as markup by a
// heuristic, when the sender's own headers said it was text. Existing
// callers pass three arguments and are unaffected.
function renderedEmailHtml(body, imagesLoaded, style, forcePlainText) {
    var asHtml = (forcePlainText === undefined)
        ? looksLikeHtmlDocument(body)
        : !forcePlainText
    var inner = asHtml
        ? String(body === undefined || body === null ? "" : body)
        : ("<pre>" + escapeHtml(body) + "</pre>")
    return "<html><head>"
        + "<meta charset=\"utf-8\">"
        // First, before anything that could trigger a fetch: a CSP meta tag
        // only governs what follows it in the document.
        + "<meta http-equiv=\"Content-Security-Policy\" content=\""
        + emailContentSecurityPolicy(imagesLoaded) + "\">"
        + "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"
        + "<style>" + style + "</style>"
        + "</head><body>" + inner + "</body></html>"
}

// Which decrypted OpenPGP body, if any, belongs to the message on screen.
//
// The id comparison is the security control, not a tidiness check. MailApp is
// a singleton holding at most one plaintext, so without it a decrypted
// message would render under the NEXT message's headers the moment the reader
// moved on -- the same class of mistake as showing a stale reply, except the
// content is the one thing in the app the sender encrypted end to end.
//
// isHtml comes from which field the C++ side populated, which it took from
// the part's MIME Content-Type. Never sniffed from the characters: a
// plain-text message that happens to contain "<html>" is still plain text,
// and the sender's own headers said so.
//
// Lives here rather than inside EmailDetail.qml for the same reason
// isExternallyOpenableUrl does -- so it is testable without standing up the
// whole singleton graph that file needs.
function decryptedBodyFor(messageId, decryptedMessageId, html, plain) {
    const id = String(messageId === undefined || messageId === null ? "" : messageId)
    const heldId = String(decryptedMessageId === undefined || decryptedMessageId === null
                          ? "" : decryptedMessageId)
    if (id === "" || id !== heldId)
        return { body: "", isHtml: false }

    const asHtml = String(html === undefined || html === null ? "" : html)
    if (asHtml !== "")
        return { body: asHtml, isHtml: true }
    return { body: String(plain === undefined || plain === null ? "" : plain), isHtml: false }
}

// Schemes a URL taken from message content may be handed to
// Qt.openUrlExternally.
//
// Everything else is refused. openUrlExternally goes to xdg-open, which
// dispatches on scheme to whatever handler the desktop registered -- and
// this app registers itself for kypost:// (packaging's .desktop MimeType),
// so an <a href="kypost://native-pair?...&srv=attacker"> in a message used
// to route straight back into PairingController and raise this app's own
// pairing-confirm dialog for the attacker's server. Every other scheme with
// a handler on the session was reachable the same way.
//
// Lives here rather than inside EmailDetail.qml so it is testable without
// standing up the whole singleton graph that file needs.
var externallyOpenableSchemes = ["http", "https", "mailto"]

function isExternallyOpenableUrl(url) {
    const s = String(url === undefined || url === null ? "" : url)
    const colon = s.indexOf(":")
    if (colon <= 0)
        return false // relative or scheme-less: nothing to hand to the desktop
    return externallyOpenableSchemes.indexOf(s.substring(0, colon).toLowerCase()) !== -1
}

// The real address out of a raw From/To/Cc header value.
//
// A display name is attacker-controlled and is authenticated by nothing: DKIM,
// SPF and DMARC all validate the domain a message was sent from, never the
// human-readable label in front of it. So this arrives intact and aligned:
//
//     From: "evil@attacker.tld" <bob@corp.com>
//
// EmailDetail's extractAddress() took the FIRST "<", which on that input still
// happens to work but fails the mirror case, and the webmail's equivalent took
// the first email-shaped substring anywhere -- which picked the address out of
// the display name. Reply/Reply All/Forward all carry the quoted original, so
// getting this wrong sends a thread to someone who never sent it.
//
// The rule, shared verbatim with the webmail and Android clients: the real
// address is the LAST angle-addr, because RFC 5322 puts display-name first and
// addr-spec last. A bare value is the address itself. Anything without an "@"
// is not an address and yields "" rather than being passed through as a
// pseudo-recipient.
function addressFromHeader(raw) {
    const s = String(raw === undefined || raw === null ? "" : raw).trim()
    if (s.length === 0)
        return ""
    let candidate = s
    const close = s.lastIndexOf(">")
    const open = close === -1 ? -1 : s.lastIndexOf("<", close)
    if (open !== -1 && close > open)
        candidate = s.substring(open + 1, close).trim()
    return candidate.indexOf("@") !== -1 ? candidate : ""
}

// The IMAP keyword the server sets on mail that impersonates KyPost itself
// (backend/internal/processor/phish_scan.go). $Phishing is the reserved RFC
// 8621 keyword, so other mail clients understand it too.
//
// The message is flagged in place -- it stays in the inbox, stays unread, and
// keeps its body. Nothing here moves or hides mail.
//
// Compared case-insensitively because IMAP keywords are case-insensitive: a
// server may echo back "$phishing" for a keyword the poller set as "$Phishing",
// and a case-sensitive check would silently drop the warning on precisely the
// mail it exists for.
//
// Lives here rather than inside EmailDetail.qml for the same reason
// isExternallyOpenableUrl does -- so it is testable without standing up the
// whole singleton graph that file needs.
function hasPhishingKeyword(keywords) {
    if (!keywords)
        return false
    for (let i = 0; i < keywords.length; i++) {
        if (String(keywords[i]).trim().toLowerCase() === "$phishing")
            return true
    }
    return false
}

// The scheme part of a URL, lowercased, or "" -- used only to tell the user
// which kind of link was refused.
function urlScheme(url) {
    const s = String(url === undefined || url === null ? "" : url)
    const colon = s.indexOf(":")
    return colon <= 0 ? "" : s.substring(0, colon).toLowerCase()
}

// Splits a plain display name on whitespace and returns up to its first two
// initials, uppercased. Returns "?" when no initials can be derived (empty
// or whitespace-only input) -- this is the exact "up to 2 characters from
// whitespace-split name parts" logic that used to be duplicated verbatim as
// initialsFor() in ContactsList.qml and ContactDetail.qml, which both only
// ever feed it a bare contact name (no "Name <email>" parsing needed).
function initialsFromName(name) {
    const s = (name || "").trim()
    if (s.length === 0)
        return "?"
    const parts = s.split(/\s+/).filter(function (p) { return p.length > 0 })
    let initials = ""
    for (let i = 0; i < parts.length && initials.length < 2; i++)
        initials += parts[i].charAt(0).toUpperCase()
    return initials
}

// Same whitespace-split-to-2-initials core as initialsFromName(), but for
// callers that have already isolated a name part from a "Name <email>"
// formatted sender string (and may pass an email local-part instead, or an
// empty string). Unlike initialsFromName(), an empty/unparseable result
// comes back as "" rather than "?" -- EmailDetail.qml's initialsFor() and
// MobileRoot.qml's initialsForSender() each handle that empty case
// differently (one falls back further to the sender's email local-part
// before ever calling this, the other falls back to "?" itself), so this
// shared core stays a plain splitter and leaves those decisions to the
// caller.
function initialsFromNamePart(namePart) {
    const s = namePart || ""
    const parts = s.split(/\s+/).filter(function (p) { return p.length > 0 })
    let initials = ""
    for (let i = 0; i < parts.length && initials.length < 2; i++)
        initials += parts[i].charAt(0).toUpperCase()
    return initials
}

// Looks up a wire folder name's display name among `folders` (the result of
// MailApp.mailFolders(), passed in by the caller rather than imported here
// so this stays a plain, dependency-free JS module). Falls back to
// `wireName` itself when not found -- which is also what makes a
// just-created subfolder render sanely before the next refresh lands.
// Shared by DesktopRoot.qml's folderDisplayName() and MobileRoot.qml's
// currentFolderDisplayName(), which both did this identical linear scan
// over the same data.
function folderDisplayName(folders, wireName) {
    for (let i = 0; i < folders.length; i++) {
        if (folders[i].wireName === wireName)
            return folders[i].displayName
    }
    return wireName
}
