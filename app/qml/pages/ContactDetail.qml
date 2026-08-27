import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.kysecurity.mail 1.0
import "../components"
import "../utils/format.js" as Format

// Task 36 -- plain reusable Item, deliberately NOT a Kirigami.Page (see
// Phase 6 global constraint 4): MobileRoot wraps this in a thin
// Kirigami.Page shell when it pushes it (Task 38); DesktopRoot embeds it
// directly inside its detail-pane Item, rebinding `uid` as the selected
// contact changes rather than re-instantiating this component (Task 39) --
// that's why both Component.onCompleted AND onUidChanged reload below,
// mirroring EmailDetail.qml's onMessageIdChanged/onFolderChanged +
// Component.onCompleted shape (Task 35).
//
// Emails, phones, and addresses are each full multi-entry list editors (via
// RepeaterField, same generic component used for ims/websites/relations/
// events/customFields below) -- every one of these is a whole-list replace
// on save, matching ContactsController::createContact/updateContact's
// documented contract (see its header doc comment).
Item {
    id: root

    // Public API. "" means "Add" (create mode); a non-empty uid means
    // "view/edit an existing contact". Whichever root hosts this component
    // owns the actual uid value (a fresh push with uid: "", a tapped list
    // row's uid, or a persistently-rebound selection) -- this file never
    // assumes push-navigation vs. a pane, per constraint 4.
    property string uid: ""

    // Emitted when a Cancel (from a fresh "Add"), a successful "Add" Save,
    // or a Delete completes -- the signal for "this screen is done, please
    // navigate back / clear the selection". NOT emitted when Cancel/Save
    // happen from edit mode on an *existing* contact -- those return to
    // this same component's own read-only view instead, since there is
    // somewhere sensible to go back to without leaving the screen.
    signal closed()
    // Emitted whenever createContact/updateContact succeeds, carrying the
    // (possibly newly-assigned) uid -- lets a host refresh a list/selection
    // without this component needing to know anything about lists.
    signal saved(string uid)
    // PGP QR key exchange: the "Scan to add key" button below emits this
    // instead of navigating itself, same "let the host decide" shape as
    // closed()/saved() -- the host pushes/opens PgpScanContactKey.qml and
    // wires its keyScanned(name, publicKey) signal back to applyScannedKey()
    // below, directly on this still-open form (nothing is persisted until
    // the user hits this form's own Save button).
    signal scanPgpKeyRequested()
    // Detach into a standalone top-level window (Desktop mode only -- see
    // the pop-out IconButton below, gated on General.isDesktopMode). Same
    // "host owns the actual Window" shape as EmailDetail.popOutRequested/
    // Compose.popOutRequested -- DesktopRoot creates the window and clears
    // its own embedded selection in response.
    signal popOutRequested(string uid)

    // True for the instance DesktopRoot's contactWindowComponent embeds
    // inside an already-standalone pop-out Window -- hides the "Open in New
    // Window" button there, same reasoning as EmailDetail.isPoppedOut.
    property bool isPoppedOut: false

    implicitWidth: 360
    implicitHeight: 640

    // "view" (read-only card) or "edit" (form). Recomputed by loadContact()
    // whenever `uid` changes; edit-mode Cancel/Save-success on an existing
    // contact flips this directly without going through loadContact() (see
    // cancelEdit()/trySave()) since neither of those changes `uid`.
    property string mode: "view"
    // Raw ContactsApp.contactAt(uid) result -- {} until loadContact() runs.
    // Read-only card binds to this directly; edit mode copies it into the
    // form fields once, on entry (beginEdit()/clearFormFields()), so typing
    // in the form doesn't live-edit the read-only card underneath it.
    property var contact: ({})
    // True iff this component loaded with an empty uid -- i.e. this is an
    // "Add" flow, not "Edit an existing contact". Drives Cancel/Save-success
    // navigation semantics (see cancelEdit()/trySave()): an Add flow has no
    // read-only view to fall back to, so it always ends by emitting
    // closed(); an edit-existing flow falls back to its own view mode
    // instead.
    property bool wasNew: false

    // extended-contact-fields Task 5: group-assignment checkbox list state.
    // availableGroups is a snapshot of ContactsApp.allGroups() taken when
    // edit mode is entered (beginEdit()/loadContact()) -- a plain property
    // rather than a live binding, matching how `contact` itself is a
    // one-time-copied snapshot the form edits independently of the
    // read-only card (see the class comment on `contact` above).
    // editingGroupIds is the in-progress QVariantList<QString> of group ids
    // the checkboxes toggle membership in; trySave() reads it directly.
    property var availableGroups: []
    property var editingGroupIds: []

    // Real synced/pending read via ContactsController::isSynced(uid), which
    // queries ContactSyncRepository's pending-change table directly (see
    // ContactListModel::SyncedRole's doc comment for the ground truth this
    // replaces a duplicated rev!=0 formula with).
    readonly property bool synced: root.contact.uid ? ContactsApp.isSynced(root.contact.uid) : false

    // label/value pairs for the read-only card, in the brief's specified
    // Email/Phone/Org/Notes order, extended (extended-contact-fields Task 5)
    // with Department/Pronouns/PGP Key/Groups. A Repeater over this avoids
    // writing the same RowLayout{SectionLabel+Text} block out by hand for
    // every field.
    readonly property var detailRows: [
        { label: i18nc("contact detail field label, the person's email address", "Email"), value: root.joinedValuesText(root.contact.emails) },
        { label: i18n("Phone"), value: root.joinedValuesText(root.contact.phones) },
        { label: i18n("Address"), value: root.joinedAddressesText(root.contact.addresses) },
        { label: i18n("Org"), value: root.contact.org || "" },
        { label: i18n("Notes"), value: root.contact.notes || "" },
        { label: i18n("Department"), value: root.contact.department || "" },
        { label: i18n("Pronouns"), value: root.contact.pronouns || "" },
        { label: i18n("PGP Key"), value: root.contact.pgpKey || "" },
        { label: i18n("Groups"), value: root.groupNamesText() },
    ]

    // Resolves root.contact.groupIds (backend group UUIDs) to display names
    // via ContactsApp.allGroups() (extended-contact-fields Task 5's
    // companion ContactsController method, backed by Task 2's GroupDao name
    // cache) and joins them for the single-line "Groups" detail row. Falls
    // back to the raw id for any uid the cache doesn't (yet) know about --
    // e.g. GroupsClient degraded on 401/error and the cache is stale/empty --
    // rather than silently dropping it, so an assigned-but-unresolved group
    // is still visible.
    function groupNamesText() {
        const ids = root.contact.groupIds || []
        if (ids.length === 0)
            return ""
        const groups = ContactsApp.allGroups()
        const names = ids.map(function (id) {
            const found = groups.find(function (g) { return g.id === id })
            return found ? found.name : id
        })
        return names.join(", ")
    }

    // Joins every entry's .value with "; " for the read-only card's
    // single-line Email/Phone summary rows -- shows every entry now,
    // not just the first.
    function joinedValuesText(entries) {
        return (entries || []).map(function (e) { return e.value }).filter(function (v) { return v !== "" }).join("; ")
    }

    // One formatted "street, city region postalCode, country" string per
    // address (blank components skipped), multiple addresses joined with
    // "; " for the read-only card's single-line Address summary row.
    function joinedAddressesText(addresses) {
        return (addresses || []).map(function (a) {
            const cityLine = [a.city, a.region, a.postalCode].filter(function (v) { return v; }).join(" ")
            return [a.street, cityLine, a.country].filter(function (p) { return p; }).join(", ")
        }).filter(function (s) { return s !== "" }).join("; ")
    }

    Component.onCompleted: loadContact()
    onUidChanged: loadContact()

    // ---- state transitions ------------------------------------------

    function loadContact() {
        if (root.uid === "") {
            root.wasNew = true
            root.contact = {}
            root.availableGroups = ContactsApp.allGroups()
            root.mode = "edit"
            clearFormFields()
        } else {
            root.wasNew = false
            root.contact = ContactsApp.contactAt(root.uid)
            root.mode = "view"
        }
    }

    function beginEdit() {
        root.availableGroups = ContactsApp.allGroups()
        loadFormFromContact()
        root.mode = "edit"
    }

    function cancelEdit() {
        // An "Add" flow has no existing contact to fall back to -- discard
        // and tell the host to navigate back. Editing an existing contact
        // just drops the in-progress form and returns to its own read-only
        // card (loadFormFromContact() will re-sync the form from
        // root.contact, unchanged, if the user re-enters edit mode later).
        if (root.wasNew)
            root.closed()
        else
            root.mode = "view"
    }

    function trySave() {
        const fields = {
            fn: nameField.text.trim(),
            org: orgField.text,
            notes: notesArea.text,
            // Every field below (including emails/phones/addresses) is a
            // whole-value/whole-list replace, matching
            // ContactsController::createContact/updateContact's documented
            // contract (see its header doc comment).
            emails: emailsField.entries,
            phones: phonesField.entries,
            addresses: addressesField.entries,
            department: departmentField.text,
            pronouns: pronounsField.text,
            phoneticGivenName: phoneticGivenNameField.text,
            phoneticFamilyName: phoneticFamilyNameField.text,
            pgpKey: pgpKeyField.text,
            // Not user-editable in this form (Task 3's lazy fetch/cache
            // path owns photoRef, see photoPathFor()'s doc comment) -- but
            // still passed through unchanged rather than omitted. Every key
            // here is a *whole-value replace* on updateContact, so omitting
            // it would silently clear an existing photoRef on every edit
            // save, not just leave it untouched.
            photoRef: root.contact.photoRef || "",
            groupIds: root.editingGroupIds,
            ims: imsField.entries,
            websites: websitesField.entries,
            relations: relationsField.entries,
            events: eventsField.entries,
            customFields: customFieldsField.entries,
        }
        if (root.wasNew) {
            const newUid = ContactsApp.createContact(fields)
            if (newUid !== "") {
                root.saved(newUid)
                root.closed()
            }
            // else: ContactsApp.lastError is now set and displayed below;
            // stay in the form so the user can fix it and retry.
        } else {
            const ok = ContactsApp.updateContact(root.uid, fields)
            if (ok) {
                root.contact = ContactsApp.contactAt(root.uid)
                root.mode = "view"
                root.saved(root.uid)
            }
        }
    }

    // PGP QR key exchange: called by the host after PgpScanContactKey.qml's
    // keyScanned(name, publicKey) fires (see scanPgpKeyRequested() above).
    // Only fills nameField if it's still blank -- never overwrites a name
    // the user already typed/loaded for an existing contact.
    function applyScannedKey(name, publicKey) {
        if (nameField.text.trim() === "")
            nameField.text = name
        pgpKeyField.text = publicKey
    }

    function doDelete() {
        // No confirmation dialog -- matches Android's behavior exactly, and
        // Mac's reviewed source doesn't have one either (per the brief).
        ContactsApp.deleteContact(root.uid, root.contact.rev || 0)
        root.closed()
    }

    // Deep-clones a QVariantList<QVariantMap>-derived JS array of entry
    // objects so RepeaterField's in-place mutation (see that component's
    // class comment) edits a form-local copy, never the objects still
    // referenced by root.contact -- same "typing in the form doesn't
    // live-edit the read-only card underneath it" rule the class comment on
    // `contact` above already states for the scalar fields.
    function cloneEntries(list) {
        return (list || []).map(function (entry) { return Object.assign({}, entry) })
    }

    function loadFormFromContact() {
        nameField.text = root.contact.fn || ""
        orgField.text = root.contact.org || ""
        notesArea.text = root.contact.notes || ""
        emailsField.entries = cloneEntries(root.contact.emails)
        phonesField.entries = cloneEntries(root.contact.phones)
        addressesField.entries = cloneEntries(root.contact.addresses)

        departmentField.text = root.contact.department || ""
        pronounsField.text = root.contact.pronouns || ""
        phoneticGivenNameField.text = root.contact.phoneticGivenName || ""
        phoneticFamilyNameField.text = root.contact.phoneticFamilyName || ""
        pgpKeyField.text = root.contact.pgpKey || ""

        root.editingGroupIds = (root.contact.groupIds || []).slice()

        imsField.entries = cloneEntries(root.contact.ims)
        websitesField.entries = cloneEntries(root.contact.websites)
        relationsField.entries = cloneEntries(root.contact.relations)
        eventsField.entries = cloneEntries(root.contact.events)
        customFieldsField.entries = cloneEntries(root.contact.customFields)
    }

    function clearFormFields() {
        nameField.text = ""
        orgField.text = ""
        notesArea.text = ""
        emailsField.entries = []
        phonesField.entries = []
        addressesField.entries = []

        departmentField.text = ""
        pronounsField.text = ""
        phoneticGivenNameField.text = ""
        phoneticFamilyNameField.text = ""
        pgpKeyField.text = ""

        root.editingGroupIds = []

        imsField.entries = []
        websitesField.entries = []
        relationsField.entries = []
        eventsField.entries = []
        customFieldsField.entries = []
    }

    // ---- layout ----------------------------------------------------

    // Wrapped in a Flickable for the same reason as EmailDetail.qml (Task
    // 35): the read-only card's rows plus its two action buttons, or the
    // edit form's five fields plus its two buttons, can exceed whatever
    // fixed-size pane/Kirigami.Page ends up hosting this component on a
    // short window -- scrolling internally means this component behaves
    // correctly regardless of how MobileRoot/DesktopRoot (Task 38/39) size
    // it.
    Flickable {
        id: flickable
        anchors.fill: parent
        contentWidth: width
        contentHeight: contentColumn.implicitHeight + contentColumn.anchors.margins * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThemedScrollBar {}

        ColumnLayout {
            id: contentColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 16

            // Pop-out -- Desktop-only (General.isDesktopMode), and only once
            // there's a real saved contact to detach (a fresh, unsaved "Add"
            // flow has nothing to reopen by uid in the new window). Visible
            // above both the read-only card and the edit form below, same
            // "persistent header row regardless of mode" shape as Compose's
            // own pop-out row.
            RowLayout {
                Layout.fillWidth: true
                visible: General.isDesktopMode && !root.isPoppedOut && root.uid !== ""
                spacing: 8

                Item { Layout.fillWidth: true }
                IconButton {
                    icon: "window-new"
                    tooltip: i18n("Open in New Window")
                    onClicked: root.popOutRequested(root.uid)
                }
            }

            // ---- read-only card -------------------------------------

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.mode === "view"
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Avatar {
                        initials: Format.initialsFromName(root.contact.fn)
                        // extended-contact-fields Task 3: fetch lazily on
                        // contact-detail open -- root.contact is already
                        // ContactsApp.contactAt(uid)'s full result (loaded
                        // by loadContact() below), so photoRef is available
                        // here with no extra model wiring, unlike
                        // ContactsList.qml's row delegate.
                        // ContactsApp.photoRevision is read for its binding
                        // dependency, not its value -- see ContactsList.qml's
                        // Avatar for why the call alone is not enough.
                        photoSource: root.contact.photoRef && ContactsApp.photoRevision >= 0
                                     ? ContactsApp.photoPathFor(root.contact.uid) : ""
                        size: 72
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: (root.contact.fn && root.contact.fn.length > 0) ? root.contact.fn : i18n("Unnamed")
                            color: Theme.inkStrong
                            font.family: Theme.fontUi
                            font.pixelSize: 22
                            font.weight: Font.Bold
                            wrapMode: Text.WordWrap
                        }
                        StatusBadge {
                            active: root.synced
                            text: root.synced ? i18n("Synced") : i18n("Local")
                        }
                        StatusBadge {
                            visible: root.contact.isSelf === true
                            active: true
                            text: i18n("Your contact")
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Repeater {
                        model: root.detailRows
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            SectionLabel {
                                Layout.preferredWidth: 70
                                Layout.alignment: Qt.AlignTop
                                textFormat: Text.PlainText
                                text: modelData.label
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                textFormat: Text.PlainText
                                text: modelData.value !== "" ? modelData.value : "—"
                                color: Theme.inkStrong
                                font.family: Theme.fontMono
                                font.pixelSize: 14
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    GhostButton {
                        text: i18n("Edit")
                        onClicked: root.beginEdit()
                    }
                    DangerButton {
                        text: i18n("Delete")
                        visible: root.uid !== ""
                        // Matches ContactsList.qml: a sync blocks the GUI
                        // thread on a nested event loop, so an unguarded
                        // button stays clickable while it runs.
                        enabled: !ContactsApp.isBusy
                        onClicked: root.doDelete()
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            // ---- edit form -------------------------------------------

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.mode === "edit"
                spacing: 10

                SectionLabel {
                    text: root.wasNew ? i18n("Add contact") : i18n("Edit contact")
                }

                ThemedTextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: i18n("Name")
                }
                ThemedTextField {
                    id: orgField
                    Layout.fillWidth: true
                    placeholderText: i18n("Org")
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 120
                    radius: Theme.shapeField
                    color: Theme.panel
                    border.width: 1
                    border.color: notesArea.activeFocus ? Theme.accent : Theme.line

                    Behavior on border.color {
                        ColorAnimation { duration: 120 }
                    }

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 10
                        contentWidth: width
                        contentHeight: notesArea.implicitHeight
                        clip: true
                        ScrollBar.vertical: ThemedScrollBar {}

                        TextArea {
                            id: notesArea
                            width: parent.width
                            wrapMode: TextArea.Wrap
                            color: Theme.inkStrong
                            placeholderTextColor: Theme.ink
                            font.family: Theme.fontUi
                            font.pixelSize: 14
                            background: null
                            selectByMouse: true
                            placeholderText: i18n("Notes…")
                        }
                    }
                }

                // ---- extended-contact-fields Task 5: scalar fields -------

                ThemedTextField {
                    id: departmentField
                    Layout.fillWidth: true
                    placeholderText: i18n("Department")
                }
                ThemedTextField {
                    id: pronounsField
                    Layout.fillWidth: true
                    placeholderText: i18n("Pronouns")
                }
                ThemedTextField {
                    id: phoneticGivenNameField
                    Layout.fillWidth: true
                    placeholderText: i18n("Phonetic given name")
                }
                ThemedTextField {
                    id: phoneticFamilyNameField
                    Layout.fillWidth: true
                    placeholderText: i18n("Phonetic family name")
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 120
                    radius: Theme.shapeField
                    color: Theme.panel
                    border.width: 1
                    border.color: pgpKeyField.activeFocus ? Theme.accent : Theme.line

                    Behavior on border.color {
                        ColorAnimation { duration: 120 }
                    }

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 10
                        contentWidth: width
                        contentHeight: pgpKeyField.implicitHeight
                        clip: true
                        ScrollBar.vertical: ThemedScrollBar {}

                        TextArea {
                            id: pgpKeyField
                            width: parent.width
                            wrapMode: TextArea.Wrap
                            color: Theme.inkStrong
                            placeholderTextColor: Theme.ink
                            font.family: Theme.fontMono
                            font.pixelSize: 12
                            background: null
                            selectByMouse: true
                            placeholderText: i18n("PGP Key")
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    GhostButton {
                        text: i18n("Scan to add key")
                        onClicked: root.scanPgpKeyRequested()
                    }
                    Item { Layout.fillWidth: true }
                }

                // ---- list-typed fields ---
                // One RepeaterField per list field (see that component's
                // class comment for the shared design) -- `columns` picks
                // out however many string sub-fields each entry shape needs
                // (addresses needs 6; ims needs 3; the rest need 2),
                // `entries` is populated by loadFormFromContact()/
                // clearFormFields() above and read back out by trySave().

                SectionLabel { text: i18nc("contact detail field label, the person's email address", "Email") }
                RepeaterField {
                    id: emailsField
                    columns: [
                        { key: "label", placeholder: i18n("Label") },
                        { key: "value", placeholder: i18nc("contact detail field label, the person's email address", "Email") },
                    ]
                }

                SectionLabel { text: i18n("Phone") }
                RepeaterField {
                    id: phonesField
                    columns: [
                        { key: "label", placeholder: i18n("Label") },
                        { key: "value", placeholder: i18n("Phone") },
                    ]
                }

                SectionLabel { text: i18n("Addresses") }
                RepeaterField {
                    id: addressesField
                    columns: [
                        { key: "label", placeholder: i18n("Label") },
                        { key: "street", placeholder: i18n("Street") },
                        { key: "city", placeholder: i18n("City") },
                        { key: "region", placeholder: i18n("Region") },
                        { key: "postalCode", placeholder: i18n("Postal code") },
                        { key: "country", placeholder: i18n("Country") },
                    ]
                }

                SectionLabel { text: i18n("Instant messaging") }
                RepeaterField {
                    id: imsField
                    columns: [
                        { key: "service", placeholder: i18n("Service") },
                        { key: "label", placeholder: i18n("Label") },
                        { key: "value", placeholder: i18n("Handle") },
                    ]
                }

                SectionLabel { text: i18n("Websites") }
                RepeaterField {
                    id: websitesField
                    columns: [
                        { key: "label", placeholder: i18n("Label") },
                        { key: "value", placeholder: i18n("URL") },
                    ]
                }

                SectionLabel { text: i18n("Relations") }
                RepeaterField {
                    id: relationsField
                    columns: [
                        { key: "label", placeholder: i18n("Relation") },
                        { key: "name", placeholder: i18n("Name") },
                    ]
                }

                SectionLabel { text: i18n("Events") }
                RepeaterField {
                    id: eventsField
                    columns: [
                        { key: "label", placeholder: i18n("Event") },
                        { key: "date", placeholder: i18n("Date (YYYY-MM-DD)") },
                    ]
                }

                SectionLabel { text: i18n("Custom fields") }
                RepeaterField {
                    id: customFieldsField
                    columns: [
                        { key: "label", placeholder: i18n("Label") },
                        { key: "value", placeholder: i18n("Value") },
                    ]
                }

                // ---- extended-contact-fields Task 5: group assignment ----
                // Simplest defensible UI per the task brief: a checkbox per
                // known group (Task 2's GroupDao name cache, via
                // ContactsApp.allGroups()), no create-new-group affordance --
                // groups are backend-managed, this client only assigns
                // existing ones.

                SectionLabel { text: i18n("Groups") }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    visible: root.availableGroups.length > 0

                    Repeater {
                        model: root.availableGroups
                        delegate: CheckBox {
                            Layout.fillWidth: true
                            // No textFormat here -- QQC2's CheckBox has no
                            // such property, and assigning it made this whole
                            // page fail to load. The group name is
                            // server-supplied, so the plain-text pin it was
                            // reaching for is real; it belongs on the
                            // contentItem Text that actually renders it,
                            // below.
                            text: modelData.name
                            checked: root.editingGroupIds.indexOf(modelData.id) !== -1
                            // QQC2's implicit `control` id (documented on
                            // Control's contentItem/background/indicator
                            // properties) refers back to this CheckBox --
                            // used here rather than `parent`, which isn't a
                            // documented/guaranteed way to reach the control
                            // from inside its own delegate.
                            contentItem: Text {
                                // Text defaults to Text.AutoText, which
                                // interprets HTML. This renders a group name
                                // the server supplies.
                                textFormat: Text.PlainText
                                text: control.text
                                color: Theme.inkStrong
                                font.family: Theme.fontUi
                                font.pixelSize: 14
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: control.indicator.width + control.spacing
                            }
                            onCheckedChanged: {
                                const groupId = modelData.id
                                const updated = root.editingGroupIds.slice()
                                const idx = updated.indexOf(groupId)
                                if (checked && idx === -1)
                                    updated.push(groupId)
                                else if (!checked && idx !== -1)
                                    updated.splice(idx, 1)
                                root.editingGroupIds = updated
                            }
                        }
                    }
                }
                MutedHint {
                    Layout.fillWidth: true
                    visible: root.availableGroups.length === 0
                    text: i18n("No groups available")
                }

                Text {
                    Layout.fillWidth: true
                    visible: ContactsApp.lastError !== ""
                    textFormat: Text.PlainText
                    text: ContactsApp.lastError
                    color: Theme.dangerColor
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    GhostButton {
                        text: i18n("Cancel")
                        onClicked: root.cancelEdit()
                    }
                    Item { Layout.fillWidth: true }
                    PrimaryButton {
                        text: i18n("Save")
                        enabled: !ContactsApp.isBusy && nameField.text.trim() !== ""
                        onClicked: root.trySave()
                    }
                }
            }
        } // contentColumn
    } // flickable
}
