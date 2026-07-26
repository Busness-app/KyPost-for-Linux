.pragma library

// Escapes HTML metacharacters so text of unknown origin (email body, contact
// name synced from the Relay server/vCard import/PGP-QR scan, etc.) can't
// inject tags when placed inside a Text.RichText item. Was duplicated
// verbatim as escapeHtml() in EmailDetail.qml, Compose.qml, and
// AutocompleteDropdown.qml.
function escapeHtml(s) {
    return (s || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
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
