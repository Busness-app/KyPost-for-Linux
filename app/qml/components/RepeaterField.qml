import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.kysecurity.mail 1.0

// New component (extended-contact-fields Task 5): ContactDetail.qml's edit
// form gained five list-typed fields (ims/websites/relations/events/
// customFields) that need genuine add/remove list-editing UI -- nothing
// existing covers this (ThemedTextField is single-value; Compose.qml's
// attachment chips are display-only, no per-field editing). One generic,
// data-driven component covers all five call sites rather than five
// near-duplicate blocks, per the task brief's explicit recommendation.
//
// Public API: `columns` is an ordered array of {key, placeholder} describing
// each entry's string sub-fields -- 2 columns for websites/relations/events/
// customFields, 3 for ims' service/label/value -- so this one component
// handles every list field without a per-type subclass. `entries` is a
// plain JS array of {key: value, ...} objects matching `columns`' keys;
// ContactDetail.qml reads `.entries` back out directly in trySave(), and
// QML's JS-array<->QVariantList-of-QVariantMap bridging does the rest (same
// bridging contactAt() already relies on for the read side).
//
// Judgment call on mutation strategy: `entries` (a plain JS array) is the
// external read/write contract -- ContactDetail.qml only ever reassigns it
// wholesale (loadFormFromContact()/clearFormFields()), never mutates it in
// place, and reads it back the same way in trySave(). Internally, the
// Repeater is backed by `entriesModel` (a ListModel), not `entries`
// directly: a plain-JS-array-backed Repeater treats every reassignment as
// an entirely new model, tearing down and recreating every delegate --
// including on unrelated rows -- which drops keyboard focus/cursor
// position whenever add()/removeAt()/a keystroke touches any row.
// `entriesModel` gives per-row/per-field in-place updates
// (append/remove/setProperty) that the Repeater does NOT treat as
// structural changes to unrelated rows. `_syncingInternally` distinguishes
// "external wholesale reassignment of entries" (full entriesModel rebuild,
// via onEntriesChanged) from "this component's own add()/removeAt()/edit()
// writing entries back out to stay in sync for trySave() to read" (must
// NOT re-trigger a rebuild, or every keystroke would rebuild the model it
// just edited).
ColumnLayout {
    id: root

    // [{key: "value", placeholder: "..."}, ...] -- static per call site, set
    // once by ContactDetail.qml.
    property var columns: []
    // [{<column.key>: <string>, ...}, ...] -- the live list being edited.
    property var entries: []

    property bool _syncingInternally: false

    Layout.fillWidth: true
    spacing: 6

    // Every appended/rebuilt row always carries exactly root.columns' keys
    // (missing ones default to "") so ListModel's inferred roles stay
    // consistent across every row, regardless of what the source entry
    // object did or didn't contain.
    function _rowFor(entryData) {
        const row = {}
        for (let i = 0; i < root.columns.length; i++) {
            const key = root.columns[i].key
            row[key] = (entryData && entryData[key] !== undefined) ? entryData[key] : ""
        }
        return row
    }

    // Guarded for its entire duration, not just the final write: each
    // entriesModel.append() below synchronously constructs a delegate whose
    // ThemedTextField.Component.onCompleted sets an initial `text`, firing
    // onTextChanged -- which, unguarded, calls _syncEntriesFromModel() and
    // reassigns root.entries mid-loop to a shorter array (reflecting
    // entriesModel's not-yet-fully-populated count at that instant). Since
    // `root.entries.length` is this loop's own bound, that reassignment
    // shrinks the bound out from under it and cuts the rebuild short
    // (confirmed via a headless QML reproduction: a 2-entry rebuild silently
    // produced only 1 row). Guarding the whole function prevents
    // onTextChanged's resync from touching entries at all here, which is
    // correct anyway -- entries IS the source of truth during a rebuild, so
    // no resync back into it is needed.
    function _rebuildModelFromEntries() {
        root._syncingInternally = true
        entriesModel.clear()
        for (let i = 0; i < root.entries.length; i++)
            entriesModel.append(root._rowFor(root.entries[i]))
        root._syncingInternally = false
    }

    // Writes entriesModel's current contents back out to entries. Guarded
    // (both here and by callers checking _syncingInternally before calling
    // this at all) so this doesn't loop back into a full rebuild via
    // onEntriesChanged, or run at all during _rebuildModelFromEntries()
    // above.
    function _syncEntriesFromModel() {
        const updated = []
        for (let i = 0; i < entriesModel.count; i++)
            updated.push(root._rowFor(entriesModel.get(i)))
        root._syncingInternally = true
        root.entries = updated
        root._syncingInternally = false
    }

    onEntriesChanged: {
        if (root._syncingInternally)
            return
        root._rebuildModelFromEntries()
    }

    Component.onCompleted: root._rebuildModelFromEntries()

    function add() {
        entriesModel.append(root._rowFor(null))
        root._syncEntriesFromModel()
    }

    function removeAt(index) {
        entriesModel.remove(index)
        root._syncEntriesFromModel()
    }

    ListModel {
        id: entriesModel
    }

    Repeater {
        model: entriesModel
        delegate: RowLayout {
            id: rowDelegate
            // Captures this row's outer index/model wrapper under distinct
            // names before the nested Repeater below shadows the bare
            // `index`/`model` identifiers with its own (over root.columns).
            // entryData is used for a one-time initial read only (see the
            // nested ThemedTextField's Component.onCompleted below) -- never
            // as a live binding source, since this field is also that same
            // role's only writer.
            property int entryIndex: index
            property var entryData: model

            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: root.columns
                delegate: ThemedTextField {
                    Layout.fillWidth: true
                    placeholderText: modelData.placeholder
                    // One-time read at delegate creation, NOT a live binding
                    // to entryData[key] -- this field is the only writer of
                    // that role (via onTextChanged below), so a live binding
                    // back to the same role it just wrote creates a genuine
                    // read-triggers-write-triggers-read cycle (unlike the
                    // old plain-JS-array version, a ListModel role change
                    // DOES fire real property-change notifications, so this
                    // actually loops -- confirmed via a headless QML
                    // reproduction; the engine detects and aborts it,
                    // corrupting entriesModel/entries).
                    Component.onCompleted: text = rowDelegate.entryData[modelData.key] || ""
                    // In-place role update -- entriesModel.setProperty()
                    // does not cause the Repeater to rebuild delegates, so
                    // typing here never touches unrelated rows. The
                    // Component.onCompleted initializer above also fires
                    // this (text goes from "" to its initial value), which
                    // happens mid-rebuild while _rebuildModelFromEntries()
                    // is still appending later rows -- skip the resync
                    // entirely in that window (root._syncingInternally),
                    // since entries is already correct and the source of
                    // truth during a rebuild; see that function's own
                    // comment for the bug this avoids.
                    onTextChanged: {
                        entriesModel.setProperty(rowDelegate.entryIndex, modelData.key, text)
                        if (!root._syncingInternally)
                            root._syncEntriesFromModel()
                    }
                }
            }

            Text {
                text: "✕"
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 16

                TapHandler {
                    onTapped: root.removeAt(rowDelegate.entryIndex)
                }
            }
        }
    }

    GhostButton {
        text: i18n("+ Add")
        onClicked: root.add()
    }
}
