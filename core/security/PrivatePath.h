#pragma once

#include <QString>

// Makes a path owner-only, and -- the part that is easy to skip -- says
// whether it worked.
//
// Every caller of this used to be a QDir().mkpath() next to a
// QFile::setPermissions(), both results dropped on the floor, under a comment
// claiming the contents were protected. On a directory somebody else created,
// or a filesystem with no POSIX permission bits, that comment was the only
// protection there was: the app went on to open a plaintext mail database
// inside a world-readable directory and never noticed.
//
// The check is on what is ON THE DISK afterwards, not on what the two calls
// returned. chmod on a FAT or an SMB mount can report success and change
// nothing, so the return value answers a weaker question than the one that
// matters.
namespace PrivatePath {

enum class Status {
    Ready,      // exists and no group/other bit is set
    NotCreated, // could not be created at all -- nothing can work here
    NotPrivate, // exists, but other local users can reach into it
};

// Creates `path` if it is missing, then tightens it to 0700. Safe to call on
// every startup: an existing directory from an older build gets tightened
// too, which is the case that matters most.
Status ensureDirectory(const QString& path);

// 0600 on an existing file. Returns false if the file cannot be made
// unreadable to other local users -- including when it does not exist, since
// a caller asking this question wants a protected file, not an absent one.
[[nodiscard]] bool ensureFile(const QString& path);

// Whether nothing outside the owner can read, write or traverse `path`.
// False for a path that does not exist.
[[nodiscard]] bool isPrivate(const QString& path);

} // namespace PrivatePath
