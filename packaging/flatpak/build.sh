#!/bin/sh
# Wraps the Flatpak build documented in README.md's "### Flatpak" section and
# duplicated in .github/workflows/ci.yml's flatpak-build job. Without this
# script, building locally means copy-pasting those commands by hand, and a
# fresh machine that's missing flatpak/flatpak-builder or the flathub remote
# fails with a cryptic error instead of a clear one -- this adds those checks
# up front.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root="$script_dir/../.."
manifest="$script_dir/com.kysecurity.mail.yaml"

# cd to the repo root so build_dir lands there as plain "build-flatpak",
# matching .gitignore's /build*/ pattern and the README/CI convention --
# otherwise a caller invoking this script from elsewhere would scatter build
# output outside the repo.
cd -- "$repo_root"
build_dir="build-flatpak"

for bin in flatpak flatpak-builder; do
    if ! command -v "$bin" >/dev/null 2>&1; then
        echo "build.sh: '$bin' not found on PATH -- install it via your distro's package manager first" >&2
        exit 1
    fi
done

# --if-not-exists makes this a safe no-op when flathub is already registered,
# so it's simpler to run unconditionally than to check first.
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo

flatpak-builder --user --force-clean --install-deps-from=flathub \
    "$build_dir" "$manifest"

echo "build.sh: built $manifest into $build_dir"
