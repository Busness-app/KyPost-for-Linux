#!/usr/bin/env bash
#
# Proves that every security guard listed in tests/guards.tsv is load-bearing:
# remove it, and the named test must go red.
#
# WHY THIS EXISTS, rather than doing it by hand each time.
#
# A test that stays green when its guard is deleted is measuring something
# else, and there is no way to tell that apart from a passing test by looking
# at a passing test. So the guards get neutralised one at a time and the tests
# are required to fail.
#
# Doing that by hand went wrong twice on 2026-08-23. Both times the
# replacement text did not match the source -- once because it ignored an
# if-init-statement -- so nothing was neutralised, the test passed, and the
# guard went into a commit message as "proven by neutralisation" when nothing
# had been proven. The fix is the check on line "did the file actually
# change": a neutralisation that cannot be applied is a FAILURE here, not a
# quiet pass.
#
# It edits tracked source files and puts them back. It refuses to start on a
# dirty tree, restores every file on every exit path including Ctrl-C, and
# verifies the restore before finishing.
#
# Slow by construction -- one incremental build per guard. Run it before a
# release, or after touching anything in the table.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
MANIFEST="$REPO_ROOT/tests/guards.tsv"

BACKUP_DIR=""
CURRENT_FILE=""

restore() {
    if [ -n "$CURRENT_FILE" ] && [ -n "$BACKUP_DIR" ] && [ -f "$BACKUP_DIR/current" ]; then
        cp "$BACKUP_DIR/current" "$REPO_ROOT/$CURRENT_FILE"
        CURRENT_FILE=""
    fi
}

cleanup() {
    restore
    [ -n "$BACKUP_DIR" ] && rm -rf "$BACKUP_DIR"
    # The last word on whether this left the tree alone. Says so rather than
    # trusting the trap: a half-restored checkout that looks clean is worse
    # than one that admits it is not.
    if [ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]; then
        echo
        echo "WARNING: the working tree is not clean. Check 'git status' and 'git checkout' the listed files." >&2
    fi
}
trap cleanup EXIT INT TERM

if [ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]; then
    echo "the working tree has uncommitted changes; refusing to edit source files" >&2
    exit 1
fi
if [ ! -f "$MANIFEST" ]; then
    echo "no manifest at $MANIFEST" >&2
    exit 1
fi
if [ ! -d "$BUILD_DIR" ]; then
    echo "no build directory at $BUILD_DIR (set BUILD_DIR=... to point elsewhere)" >&2
    exit 1
fi

BACKUP_DIR="$(mktemp -d)"
checked=0
failed=0

echo "proving each guard is load-bearing; a guard whose test stays green is a finding"
echo

while IFS= read -r line || [ -n "${line:-}" ]; do
    case "$line" in ''|'#'*) continue ;; esac

    # Cut the four fields by hand instead of letting `read -r a b c d` do it.
    # Tab is an IFS *whitespace* character, so bash collapses a run of tabs
    # into one separator: a row whose replacement is empty -- "delete this
    # line", which is a legitimate neutralisation -- came out as a three-field
    # row with an empty target, and the loop below skipped it without a word.
    # One guard in this manifest was going unverified that way while the
    # summary still counted the rest as a clean run. A row that cannot be
    # parsed is now a failure, like every other kind of not-actually-proven.
    file="${line%%$'\t'*}"
    rest="${line#*$'\t'}"
    search="${rest%%$'\t'*}"
    rest="${rest#*$'\t'}"
    replace="${rest%%$'\t'*}"
    target="${rest#*$'\t'}"

    if [ -z "$target" ] || [ "$target" = "$replace" ] || [ "$search" = "$file" ]; then
        echo "MALFORMED ROW  $line"
        echo "    expected exactly four tab-separated fields"
        failed=$((failed + 1))
        continue
    fi

    ctest_target="${target%%:*}"
    test_name="${target##*:}"
    path="$REPO_ROOT/$file"

    if [ ! -f "$path" ]; then
        echo "MISSING FILE  $file  ($test_name)"
        failed=$((failed + 1))
        continue
    fi

    # The check the hand-run version did not have. A search string that is
    # absent, or present more than once, means the manifest has drifted from
    # the source -- and a neutralisation nobody can apply proves nothing.
    # -x: whole lines. Without it a deeper-indented copy of the same
    # statement counts as a match, because the shallower text is a substring
    # of it -- which is how two distinct guards looked like one ambiguous one.
    occurrences="$(grep -x -F -c -- "$search" "$path" || true)"
    if [ "$occurrences" != "1" ]; then
        echo "NOT APPLICABLE  $file  ($test_name)"
        echo "    the guard line appears $occurrences times; expected exactly 1"
        echo "    looked for: $search"
        failed=$((failed + 1))
        continue
    fi

    cp "$path" "$BACKUP_DIR/current"
    CURRENT_FILE="$file"

    # Whole-line replacement via python so the text is matched literally --
    # no regex metacharacters to escape, and the indentation is part of it.
    SEARCH="$search" REPLACE="$replace" python3 - "$path" <<'PY'
import os, sys
path = sys.argv[1]
search = os.environ["SEARCH"]
replace = os.environ["REPLACE"]
# Whole-line replacement, so indentation is part of the identity of the line
# and a deeper-indented sibling is a different guard rather than the same one.
lines = open(path).read().split("\n")
hits = [i for i, line in enumerate(lines) if line == search]
assert len(hits) == 1, f"search line matched {len(hits)} lines, expected 1"
lines[hits[0]] = replace
open(path, "w").write("\n".join(lines))
PY

    # Belt and braces: confirm the bytes on disk actually moved. If a future
    # edit makes the replacement a no-op, this catches it rather than
    # reporting a guard as proven.
    if cmp -s "$path" "$BACKUP_DIR/current"; then
        echo "NOT NEUTRALISED  $file  ($test_name)"
        echo "    the replacement left the file unchanged"
        restore
        failed=$((failed + 1))
        continue
    fi

    if ! cmake --build "$BUILD_DIR" --target "$ctest_target" -j"$(nproc)" > "$BACKUP_DIR/build.log" 2>&1; then
        # A guard whose removal does not compile is still evidence the code
        # depends on it, but it is not evidence the TEST does -- so it is not
        # counted as proven.
        echo "DOES NOT BUILD  $test_name  (guard removal broke the build; not proven)"
        restore
        failed=$((failed + 1))
        continue
    fi

    set +e
    QT_QPA_PLATFORM=offscreen "$BUILD_DIR/tests/$ctest_target" "$test_name" > "$BACKUP_DIR/run.log" 2>&1
    status=$?
    set -e

    if [ "$status" -ne 0 ]; then
        echo "proven          $test_name"
        checked=$((checked + 1))
    elif ! grep -q '^PASS   : ' "$BACKUP_DIR/run.log"; then
        # QtTest exits 0 for a SKIPPED test, so a zero status can mean the test
        # never ran at all -- a build without SQLCipher skips the whole
        # migration case in initTestCase(). Reporting that as "measuring
        # something else" sends the next reader hunting a defect in a test that
        # is fine. Still not proven, still counted against the run: a guard
        # this build cannot speak to is a guard nobody has checked.
        echo "NOT RUN         $test_name"
        echo "    the test was skipped in this build, so the guard is unproven here, not disproven."
        failed=$((failed + 1))
    else
        echo "STILL GREEN     $test_name"
        echo "    the guard was removed and this test did not notice. It is measuring something else."
        failed=$((failed + 1))
    fi

    restore
done < "$MANIFEST"

echo
echo "restoring and rebuilding"
cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null 2>&1

echo
if [ "$failed" -ne 0 ]; then
    echo "$checked guard(s) proven, $failed NOT proven -- see above" >&2
    exit 1
fi
echo "$checked guard(s) proven load-bearing"
