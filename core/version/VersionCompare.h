#pragma once

#include <QString>

// Dotted-numeric version comparison for the update notice. A port of the
// server's ghrelease.IsNewer/parseVersion, kept identical in behaviour so the
// two channels cannot disagree about what "newer" means.
namespace VersionCompare {

// True only when `latest` is a strictly newer N.N.N version than `installed`.
//
// A leading "v" is stripped from either side: release tags are written
// "v0.3.0" and KYPOST_VERSION is written "0.2.0".
//
// Anything that is not three non-negative integers is REFUSED rather than
// parsed best-effort, and a refusal returns false. Best-effort parsing of a
// tag like "v0.1-alpha" produces a confident wrong answer, and the failure
// mode of a wrong answer here is either nagging a current user forever or
// silently never telling a stale one.
bool isNewer(const QString& latest, const QString& installed);

} // namespace VersionCompare
