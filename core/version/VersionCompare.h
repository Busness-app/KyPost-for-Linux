#pragma once

#include <QString>

// Dotted-numeric version comparison for the update notice. A port of the
// server's ghrelease.IsNewer/parseVersion, but deliberately STRICTER: the
// server pads short component counts and tolerates signs/whitespace, while
// this parser requires exactly three digit-only parts. A tag the server
// forwards but this client cannot parse (e.g. "0.4" or "0.3.0-rc1", both
// forms the server accepts) must read on screen as "unknown", never as
// "current" -- see isValid()'s comment.
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

// True when `version` parses as three non-negative dotted integers (a
// leading "v" allowed). Callers use this to decide whether an unparseable
// `latest` should be discarded rather than displayed: isNewer() alone cannot
// distinguish "parsed and not newer" from "could not parse", and the UI
// must never read the second case as "you are current".
bool isValid(const QString& version);

} // namespace VersionCompare
