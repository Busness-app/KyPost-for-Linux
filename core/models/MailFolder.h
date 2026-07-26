#pragma once

#include <QString>

// One mailbox as reported by GET /api/inbox/folders (backend's `inboxFolder`
// struct: {path, deletable}).
//
// `parent` is not on the wire per-folder -- the response carries a single
// top-level `parent` echoing the request's query -- so it is filled in by
// FolderClient from that echoed value, which is what makes a FolderDao row
// addressable by parent.
//
// Distinct from core/db/FolderDao.h's FolderRecord: that is the DAO's
// read-side row type (and carries sourceMode, a storage concern). This is
// the wire/domain shape.
struct MailFolder
{
    QString path;
    QString parent;
    // False for the six built-in mailboxes and for any top-level folder --
    // the backend refuses to rename or delete either (isBuiltinMailbox /
    // mailboxParentPath checks in handleInboxFolders), so the UI must not
    // offer the action. Server-decided rather than re-derived here, so the
    // two can never disagree.
    bool deletable = false;

    bool operator==(const MailFolder&) const = default;
};
