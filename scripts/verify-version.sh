#!/usr/bin/env bash
#
# Keeps the three places a version is written from drifting apart:
#
#   CMakeLists.txt   project(KyPost VERSION x.y.z)  -- source of truth, reaches
#                    C++ as KYPOST_VERSION and ends up in the User-Agent header
#   metainfo.xml     <release version="x.y.z"/>     -- what Discover shows, and
#                    what decides whether a user is offered the update at all
#   git tag          v<x.y.z>[-suffix]              -- what the release is named
#
# CMakeLists.txt already says "keep it in step with the <release> entries".
# This is that sentence, enforced. Without it a tagged release ships a binary
# reporting the previous version and an AppStream entry nobody notices is
# stale until someone asks why `flatpak update` found nothing.
#
# Runs on every CI build (CMake vs metainfo -- catches drift while it is still
# cheap to fix) and additionally checks the tag on `v*` release builds.
#
# Usage: verify-version.sh [tag]
#   tag defaults to $GITHUB_REF_NAME when the ref is a tag, else no tag check.

set -euo pipefail

cd "$(dirname "$0")/.."

cmake_version=$(sed -n 's/^project(KyPost VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)
[ -n "$cmake_version" ] || { echo "no project(KyPost VERSION ...) in CMakeLists.txt" >&2; exit 1; }

metainfo=packaging/flatpak/com.kysecurity.mail.metainfo.xml
# Newest first: AppStream requires <releases> in descending version order, so
# the top entry is the one being released.
release_version=$(sed -n 's/.*<release version="\([^"]*\)".*/\1/p' "$metainfo" | head -1)
[ -n "$release_version" ] || { echo "no <release version=...> in $metainfo" >&2; exit 1; }

fail=0

if [ "$cmake_version" != "$release_version" ]; then
  echo "version drift: CMakeLists.txt says $cmake_version, $metainfo says $release_version" >&2
  fail=1
fi

tag="${1-}"
if [ -z "$tag" ] && [ "${GITHUB_REF_TYPE-}" = "tag" ]; then
  tag="${GITHUB_REF_NAME-}"
fi

if [ -n "$tag" ]; then
  # v0.2.0 and v0.2.0-rc1 both release CMake version 0.2.0; the suffix marks a
  # prerelease of it, not a different version. Anything else is a typo.
  case "$tag" in
    "v$cmake_version" | "v$cmake_version"-*) ;;
    *)
      echo "tag $tag does not release version $cmake_version" >&2
      echo "expected v$cmake_version or v$cmake_version-<prerelease>" >&2
      fail=1
      ;;
  esac
fi

[ "$fail" -eq 0 ] || {
  echo "Bump all three together: CMakeLists.txt, $metainfo, and the tag." >&2
  exit 1
}

echo "version ok: $cmake_version${tag:+ (tag $tag)}"
