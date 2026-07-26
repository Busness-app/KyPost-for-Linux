# Remaining Llama Naming Cleanup (kypost-Linux) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the remaining "llama"/"Llama" naming from the kypost-Linux Qt/C++/QML client — the `llamacore` CMake target and its migration-codegen variables, ~35 `llama_add_test`/`llama_add_app_test` CMake test registrations, the `X-LLAMA-CUSTOM` vCard property, a handful of test-fixture strings, a real (not cosmetic) QML import bug, and stale docs/comments — while leaving the already-completed `llamamail.db` → `kypost.db` migration and everything else untouched.

**Architecture:** Pure rename/refactor plus one genuine bug fix (the QML import). No behavior change anywhere else.

**Tech Stack:** C++20, Qt6/QtTest, CMake, QML.

## Global Constraints

- New naming: `llama` → `kypost` (CMake targets/vars), `Llama` → `KyPost` (identifiers, comments), matching the app's existing `KyPost` branding — confirmed by the app already shipping a `kypost.db` (renamed from `llamamail.db` in a prior session, with a working one-time on-disk migration you should NOT touch or duplicate).
- **`X-LLAMA-CUSTOM` vCard property (Task 3): rename outright, no backward-compat shim** — confirmed with the user that no meaningfully-important previously-exported vCards depend on the old property name surviving.
- **`app/qml/pages/MfaApproval.qml:4`'s `import com.urlxl.LlamaMail 1.0` (Task 4) is a real bug, not a rename target** — every sibling QML page correctly imports `com.urlxl.mail 1.0`; this one file was left on a module name that was never actually registered under that name. Fix it to match its siblings exactly (`com.urlxl.mail 1.0`), not to some new `com.urlxl.KyPost`-style name.
- Every rename in this plan is scoped to an explicit file list or a narrowly-matched pattern — no blind repo-wide `llama`→`kypost` regex, since the substring appears in generated build output (`build-local/`, excluded throughout) and in historical planning docs (`docs/superpowers/plans/*.md`, `Linux_QT_Client_Plan.md` — left untouched as a historical record, same convention used elsewhere in this rebrand).
- Build/verification baseline: `cmake -B build -S . && cmake --build build && ctest --test-dir build` must pass after every task. Use a **fresh** `build/` directory (not `build-local/`, which is a separate, gitignored local build dir with its own stale CMake cache from before this plan — reusing it risks stale generated `MigrationSql.h` content masking a real CMake variable-rename bug).

---

## Task 1: Rename the `llamacore` CMake target and migration-codegen variables

**Files:**
- Modify: `core/CMakeLists.txt`
- Modify: `core/db/MigrationSql.h.in`
- Modify: `core/db/Database.cpp`
- Modify: `app/CMakeLists.txt`
- Modify: `app/push/NotificationDispatcher.h` (comment only)

**Interfaces:**
- Produces: CMake library target `kypostcore` (was `llamacore`), CMake variables `KYPOST_MIGRATION_FILES`/`KYPOST_MIGRATION_FUNCTIONS`/`KYPOST_MIGRATION_TABLE_ENTRIES`/`KYPOST_MIGRATION_COUNT`, generated header symbols `KyPostMigrationSqlFn`/`kKyPostMigrationCount`/`kKyPostMigrationSql` — consumed by Task 2 (`tests/CMakeLists.txt` links against `llamacore` by name in ~35 places).

- [ ] **Step 1: Rename the CMake library target and its migration-codegen variables in `core/CMakeLists.txt`**

Run:
```bash
cd /home/yoshi/git/kypost-Linux
sed -i \
  -e 's/add_library(llamacore STATIC/add_library(kypostcore STATIC/' \
  -e 's/\bLLAMA_MIGRATION_FILES\b/KYPOST_MIGRATION_FILES/g' \
  -e 's/\bLLAMA_MIGRATION_FUNCTIONS\b/KYPOST_MIGRATION_FUNCTIONS/g' \
  -e 's/\bLLAMA_MIGRATION_TABLE_ENTRIES\b/KYPOST_MIGRATION_TABLE_ENTRIES/g' \
  -e 's/\bLLAMA_MIGRATION_COUNT\b/KYPOST_MIGRATION_COUNT/g' \
  -e 's/llamaMigrationSql\$\{migration_number\}/kypostMigrationSql${migration_number}/g' \
  -e 's/target_include_directories(llamacore/target_include_directories(kypostcore/g' \
  -e 's/target_link_libraries(llamacore/target_link_libraries(kypostcore/' \
  core/CMakeLists.txt
```

- [ ] **Step 2: Rename the corresponding symbols in the generated-header template**

Run:
```bash
sed -i \
  -e 's/@LLAMA_MIGRATION_FUNCTIONS@/@KYPOST_MIGRATION_FUNCTIONS@/' \
  -e 's/\bLlamaMigrationSqlFn\b/KyPostMigrationSqlFn/g' \
  -e 's/\bkLlamaMigrationCount\b/kKyPostMigrationCount/g' \
  -e 's/@LLAMA_MIGRATION_COUNT@/@KYPOST_MIGRATION_COUNT@/' \
  -e 's/\bkLlamaMigrationSql\b/kKyPostMigrationSql/g' \
  -e 's/@LLAMA_MIGRATION_TABLE_ENTRIES@/@KYPOST_MIGRATION_TABLE_ENTRIES@/' \
  core/db/MigrationSql.h.in
```

- [ ] **Step 3: Update the two usages of the renamed generated symbols in `core/db/Database.cpp`**

Run:
```bash
sed -i \
  -e 's/\bkLlamaMigrationCount\b/kKyPostMigrationCount/g' \
  -e 's/\bkLlamaMigrationSql\b/kKyPostMigrationSql/g' \
  core/db/Database.cpp
```

Note: `core/db/Database.cpp:29`'s `QStringLiteral("llama_db_%1")` connection-name string is a separate, unrelated identifier (a Qt SQL connection name, internal-only, never persisted or exposed) — leave it for now, it's folded into Task 4's misc-string cleanup, not this task.

- [ ] **Step 4: Update the `app/CMakeLists.txt` link reference**

Run:
```bash
sed -i 's/^  llamacore$/  kypostcore/' app/CMakeLists.txt
```

Confirm this matched — run `grep -n "llamacore\|kypostcore" app/CMakeLists.txt` first if the exact indentation shown above doesn't match; the brief's job is the correct end state (`app`'s executable target links `kypostcore` instead of `llamacore`), not this exact sed pattern if the file's whitespace differs.

- [ ] **Step 5: Update the comment in `app/push/NotificationDispatcher.h`**

Run:
```bash
sed -i 's/\bllamacore\b/kypostcore/' app/push/NotificationDispatcher.h
```

- [ ] **Step 6: Verify zero remaining `llamacore`/`LLAMA_MIGRATION`/`LlamaMigrationSqlFn`/`kLlamaMigration` references in these 5 files**

Run: `grep -n -i "llama" core/CMakeLists.txt core/db/MigrationSql.h.in core/db/Database.cpp app/CMakeLists.txt app/push/NotificationDispatcher.h`
Expected: only `core/db/Database.cpp:29`'s `"llama_db_%1"` connection-name string (explicitly deferred to Task 4 above) and possibly the `llama_db_%1`-adjacent context — no `llamacore`, `LLAMA_MIGRATION_*`, `LlamaMigrationSqlFn`, `kLlamaMigration*` should remain.

- [ ] **Step 7: Fresh build (this changes the library target name, so a stale `build/` cache from before this task could mask a real link error — start clean)**

Run:
```bash
rm -rf build
cmake -B build -S . && cmake --build build
```
Expected: clean build, no link errors. (Full `ctest` run happens in Task 2, once `tests/CMakeLists.txt`'s own `llamacore` references are also updated — until then, `tests/CMakeLists.txt` still says `target_link_libraries(${test_name} PRIVATE llamacore Qt6::Test)`, which would now fail to link since the target is renamed. If Step 7's build fails specifically because of `tests/CMakeLists.txt` referencing the old `llamacore` name, that's expected and not a Task 1 bug — note it and proceed; Task 2 fixes it.)

- [ ] **Step 8: Commit**

```bash
git add core/CMakeLists.txt core/db/MigrationSql.h.in core/db/Database.cpp app/CMakeLists.txt app/push/NotificationDispatcher.h
git commit -m "linux: rename llamacore CMake target and migration codegen vars to kypostcore/KYPOST_MIGRATION_*"
```

---

## Task 2: Rename the `llama_add_test`/`llama_add_app_test` CMake helper functions and all call sites

**Files:**
- Modify: `tests/CMakeLists.txt` (2 function definitions, ~35 call sites, 4 comment references to `llamacore`)

**Interfaces:**
- Consumes: `kypostcore` target from Task 1 — every `llama_add_test`/`llama_add_app_test` call links against it.
- Produces: CMake functions `kypost_add_test(test_name)` and `kypost_add_app_test(test_name)`.

- [ ] **Step 1: Confirm current call-site count**

Run: `grep -c "llama_add_test\|llama_add_app_test" tests/CMakeLists.txt`
Expected: a count in the high 30s/low 40s (2 function definitions + ~35 call sites + a few comment mentions).

- [ ] **Step 2: Rename the function definitions, every call site, and the `llamacore` link references, in one pass**

This is a single repo-scoped file, so a plain rename of every occurrence of the two function names and the `llamacore` target name is safe and correct here (unlike the general codebase, `tests/CMakeLists.txt` has no other meaning for these exact tokens):

```bash
sed -i \
  -e 's/\bllama_add_app_test\b/kypost_add_app_test/g' \
  -e 's/\bllama_add_test\b/kypost_add_test/g' \
  -e 's/\bllamacore\b/kypostcore/g' \
  tests/CMakeLists.txt
```

(`llama_add_app_test` is renamed first so the substring `llama_add_test` inside it doesn't get partially matched by the second pattern in a way that breaks the compound name — verify this landed correctly in Step 3.)

- [ ] **Step 3: Verify the two function names and all call sites are consistent**

Run: `grep -n "kypost_add_test\|kypost_add_app_test\|llama_add" tests/CMakeLists.txt | head -20`
Expected: every line shows `kypost_add_test`/`kypost_add_app_test`, nothing shows `llama_add_test`/`llama_add_app_test`. Spot-check the function *definitions* (`function(kypost_add_test test_name)` and `function(kypost_add_app_test test_name)`) are intact and distinct from each other.

- [ ] **Step 4: Verify zero remaining `llama` references in this file**

Run: `grep -n -i "llama" tests/CMakeLists.txt`
Expected: no output.

- [ ] **Step 5: Fresh build and full test suite**

Run:
```bash
rm -rf build
cmake -B build -S . && cmake --build build && ctest --test-dir build
```
Expected: clean build, all tests pass (should match the pre-rebrand baseline test count — confirm via `ctest --test-dir build` output, e.g. "100% tests passed, N/N Tests Passed" matching what Task 4 of the deep-link plan reported: 58/58 at that point, likely higher now if more tests were added since).

- [ ] **Step 6: Commit**

```bash
git add tests/CMakeLists.txt
git commit -m "linux: rename llama_add_test/llama_add_app_test CMake helpers to kypost_add_test/kypost_add_app_test"
```

---

## Task 3: Rename the `X-LLAMA-CUSTOM` vCard property

**Files:**
- Modify: `core/vcard/VCardContact.cpp`
- Modify: `tests/core/vcard/VCardContactTest.cpp`

**Interfaces:**
- Produces: vCard custom-field property name `X-KYPOST-CUSTOM` (was `X-LLAMA-CUSTOM`), used both when writing (`VCardContact.cpp`'s export path) and parsing (its import path).
- Confirmed with the user: direct rename, no dual-read backward-compat shim for previously-exported vCards using the old property name.

- [ ] **Step 1: Rename every occurrence in the implementation file**

Run:
```bash
cd /home/yoshi/git/kypost-Linux
sed -i 's/X-LLAMA-CUSTOM/X-KYPOST-CUSTOM/g' core/vcard/VCardContact.cpp
```

This covers: the two comments at lines ~231/488 explaining the property, the `labelParam` comment at ~266, the write path at ~491 (`QStringLiteral("X-LLAMA-CUSTOM")`), and the parse-path comparison at ~605 (`name == QStringLiteral("X-LLAMA-CUSTOM")`).

- [ ] **Step 2: Rename the three test assertions**

Run:
```bash
sed -i 's/X-LLAMA-CUSTOM/X-KYPOST-CUSTOM/g' tests/core/vcard/VCardContactTest.cpp
```

- [ ] **Step 3: Verify zero remaining references and consistent read/write pairing**

Run: `grep -n "X-LLAMA-CUSTOM\|X-KYPOST-CUSTOM" core/vcard/VCardContact.cpp tests/core/vcard/VCardContactTest.cpp`
Expected: every hit says `X-KYPOST-CUSTOM`; confirm both the write-path `QStringLiteral(...)` call and the parse-path `name ==` comparison in `VCardContact.cpp` use the identical new string (a mismatch between the two would mean the app can no longer read back its own freshly-written custom fields — this is the one thing in this task that would be a real regression, not just a rename).

- [ ] **Step 4: Build and run the vCard test specifically, then the full suite**

Run:
```bash
cmake --build build --target VCardContactTest
ctest --test-dir build -R VCardContactTest
ctest --test-dir build
```
Expected: `VCardContactTest` passes on its own, then the full suite still passes.

- [ ] **Step 5: Commit**

```bash
git add core/vcard/VCardContact.cpp tests/core/vcard/VCardContactTest.cpp
git commit -m "linux: rename vCard custom property X-LLAMA-CUSTOM -> X-KYPOST-CUSTOM"
```

---

## Task 4: Fix the MfaApproval.qml import bug and rename remaining misc strings

**Files:**
- Modify: `app/qml/pages/MfaApproval.qml`
- Modify: `core/db/Database.cpp` (the connection-name string deferred from Task 1)
- Modify: `tests/app/platform/SecureStoreKeychainTest.cpp`
- Modify: `tests/core/stores/SecureStoreFileTest.cpp`
- Modify: `tests/core/db/DatabaseTest.cpp`
- Modify: `tests/app/push/PushPayloadParserTest.cpp`

**Interfaces:**
- Fixes a real bug: `MfaApproval.qml` was importing a QML module name (`com.urlxl.LlamaMail 1.0`) that was never actually registered under that name anywhere in this codebase — every sibling QML page correctly imports `com.urlxl.mail 1.0`. This is not a rename, it's making this one file consistent with how the module is actually registered everywhere else.

- [ ] **Step 1: Fix the QML import bug**

Run: `grep -n "^import" app/qml/pages/MfaApproval.qml`
Expected: shows `import com.urlxl.LlamaMail 1.0` among the import lines, and — for comparison — `grep -n "^import" app/qml/pages/Pairing.qml` (or any sibling page) should show `import com.urlxl.mail 1.0`.

Run:
```bash
sed -i 's/import com\.urlxl\.LlamaMail 1\.0/import com.urlxl.mail 1.0/' app/qml/pages/MfaApproval.qml
```

- [ ] **Step 2: Rename the Qt SQL connection-name string deferred from Task 1**

Run:
```bash
sed -i 's/QStringLiteral("llama_db_%1")/QStringLiteral("kypost_db_%1")/' core/db/Database.cpp
```

This is an internal-only Qt SQL connection identifier (never persisted, never exposed to users or other processes) — a direct rename is safe with no compatibility concern.

- [ ] **Step 3: Rename the remaining test-fixture strings**

Run:
```bash
sed -i 's/llama-mail-securestore-test-%1/kypost-securestore-test-%1/' tests/app/platform/SecureStoreKeychainTest.cpp
sed -i 's#etc/llama-mail-should-not-be-written#etc/kypost-should-not-be-written#' tests/core/stores/SecureStoreFileTest.cpp
sed -i 's/llama-test\.sqlite/kypost-test.sqlite/' tests/core/db/DatabaseTest.cpp
sed -i 's/Llama Mail Test Notification/KyPost Test Notification/g' tests/app/push/PushPayloadParserTest.cpp
```

These are all self-contained test fixtures (temp file names, path-traversal-guard test strings, fabricated JSON payloads used to test the client's own parsing logic) — none of them assert against real backend output or persisted user data, so a direct rename is safe.

- [ ] **Step 4: Verify zero remaining references in the 6 touched files**

Run: `grep -n -i "llama" app/qml/pages/MfaApproval.qml core/db/Database.cpp tests/app/platform/SecureStoreKeychainTest.cpp tests/core/stores/SecureStoreFileTest.cpp tests/core/db/DatabaseTest.cpp tests/app/push/PushPayloadParserTest.cpp`
Expected: no output.

- [ ] **Step 5: Build and run the full test suite**

Run:
```bash
cmake --build build && ctest --test-dir build
```
Expected: clean build, all tests pass, including `DatabaseTest`, `SecureStoreKeychainTest`, `SecureStoreFileTest`, `PushPayloadParserTest`.

- [ ] **Step 6: Manual confirmation of the QML fix (best-effort, no display available)**

If a display/Qt runtime is available in this environment, launch the app and navigate to the MFA approval flow to confirm `MfaApproval.qml` still loads without a "module not found" QML error. If no display is available (likely, in a headless session), note this in your report as unverified-at-runtime — the fix itself (matching every sibling page's working import exactly) is low-risk, but a QML import typo would only surface as a runtime error, not a compile error, since QML isn't compiled by `cmake --build`.

- [ ] **Step 7: Commit**

```bash
git add app/qml/pages/MfaApproval.qml core/db/Database.cpp tests/app/platform/SecureStoreKeychainTest.cpp tests/core/stores/SecureStoreFileTest.cpp tests/core/db/DatabaseTest.cpp tests/app/push/PushPayloadParserTest.cpp
git commit -m "linux: fix MfaApproval.qml's wrong module import, rename remaining llama test/internal strings"
```

---

## Task 5: Update documentation and sibling-repo comment references

**Files:**
- Modify: `AGENTS.md`
- Modify: `README.md`
- Modify: `TESTING.md`
- Modify: `po/extract-messages.sh`
- Modify: `public/pwa-icon.svg`
- Modify: `core/net/HttpClient.h`
- Modify: `core/net/NativeRegistrationClient.h`
- Modify: `core/net/NativeRegistrationClient.cpp`
- Modify: `core/net/NetworkError.h`
- Modify: `core/domain/MailRepository.h`
- Modify: `core/domain/ContactSyncRepository.h`
- Modify: `core/domain/PushRepository.h`
- Modify: `core/models/MfaChallenge.h`

**Interfaces:**
- None — doc/comment-only changes, no code behavior affected.

- [ ] **Step 1: Rewrite `AGENTS.md`'s title and body prose**

Run: `sed -n '1,15p;50,60p' AGENTS.md` to see current content, then hand-edit (not blind sed — this is prose, same convention used for the backend plan's AGENTS.md updates):
- Title `# Llama Mail — Linux Qt Client` → `# KyPost — Linux Qt Client`
- Body mentions of "Llama Mail is a **relay-only**..." → "KyPost is a **relay-only**..."
- Sibling-repo path references `~/git/llama-mobile` → `~/git/kypost-android`, `~/git/llama-Mail-for-Mac` → `~/git/kypost-for-Mac`
- `libllamacore` (in the directory-layout description) → `libkypostcore`
- Any remaining `llama-mobile`/`llama-Mail-for-Mac` mentions in the "Wire contracts come from..." section → `kypost-android`/`kypost-for-Mac`

- [ ] **Step 2: Rewrite `README.md:74`'s `libllamacore` description**

Run:
```bash
sed -i 's/libllamacore/libkypostcore/' README.md
```

- [ ] **Step 3: Update `TESTING.md`'s two mentions**

Run: `grep -n -i "llama" TESTING.md`
Expected: line 4 (`"formerly \"Llama Mail\""` — historical framing, already past-tense, can stay as a historical note or be tightened; your call, minor either way) and line 395 (`"not the old llama wordmark"` — also historical/comparative, referring to an already-completed icon rebrand). Both are safe to leave as historical record (same convention as other completed-rebrand docs elsewhere in this project) or lightly tidy — do not spend more than a quick pass here, these aren't load-bearing.

- [ ] **Step 4: Rewrite `po/extract-messages.sh`'s copyright holder**

Run:
```bash
sed -i 's/--copyright-holder="Llama Mail"/--copyright-holder="KyPost"/g' po/extract-messages.sh
```

- [ ] **Step 5: Rewrite `public/pwa-icon.svg`'s title/description**

Run:
```bash
sed -i \
  -e 's/<title id="title">Llama Mail<\/title>/<title id="title">KyPost<\/title>/' \
  -e 's/A stylized monogram icon for Llama Mail\./A stylized monogram icon for KyPost./' \
  public/pwa-icon.svg
```

- [ ] **Step 6: Rewrite the 8 sibling-repo comment references in `core/`**

Run:
```bash
sed -i 's/llama-Mail-for-Mac/kypost-for-Mac/g' core/net/HttpClient.h core/net/NativeRegistrationClient.h core/net/NativeRegistrationClient.cpp core/net/NetworkError.h core/domain/MailRepository.h core/domain/ContactSyncRepository.h core/domain/PushRepository.h
```

(`core/models/MfaChallenge.h` was listed in the original survey but double-check it — `grep -n "llama-mobile\|llama-Mail-for-Mac" core/models/MfaChallenge.h` — and apply the same substitution if it's `llama-mobile` there instead: `sed -i 's/llama-mobile/kypost-android/' core/models/MfaChallenge.h`.)

- [ ] **Step 7: Verify zero remaining references in the 13 touched files**

Run: `grep -n -i "llama" AGENTS.md README.md po/extract-messages.sh public/pwa-icon.svg core/net/HttpClient.h core/net/NativeRegistrationClient.h core/net/NativeRegistrationClient.cpp core/net/NetworkError.h core/domain/MailRepository.h core/domain/ContactSyncRepository.h core/domain/PushRepository.h core/models/MfaChallenge.h`
Expected: no output (TESTING.md is intentionally excluded per Step 3's note — its two historical mentions are acceptable to leave).

- [ ] **Step 8: Build (docs/comments don't affect compilation, but confirm nothing else broke)**

Run: `cmake --build build && ctest --test-dir build`
Expected: clean build, all tests still pass.

- [ ] **Step 9: Commit**

```bash
git add AGENTS.md README.md TESTING.md po/extract-messages.sh public/pwa-icon.svg core/net/HttpClient.h core/net/NativeRegistrationClient.h core/net/NativeRegistrationClient.cpp core/net/NetworkError.h core/domain/MailRepository.h core/domain/ContactSyncRepository.h core/domain/PushRepository.h core/models/MfaChallenge.h
git commit -m "linux: update docs and sibling-repo comment references for kypost rename"
```

---

## Task 6: Final full-repo sweep

**Files:** none (verification only)

- [ ] **Step 1: Full case-insensitive sweep, excluding generated/historical content**

Run:
```bash
cd /home/yoshi/git/kypost-Linux
grep -rni "llama" . \
  --exclude-dir=.git --exclude-dir=build --exclude-dir=build-local \
  | grep -v 'docs/superpowers/\|Linux_QT_Client_Plan\.md\|TESTING\.md'
```
Expected: no output.

- [ ] **Step 2: Fresh full build and test suite**

Run:
```bash
rm -rf build
cmake -B build -S . && cmake --build build && ctest --test-dir build
```
Expected: clean build, 100% tests passed.
