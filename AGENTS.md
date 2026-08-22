# KyPost — Linux Qt Client

## 1. Project identity

KyPost is a **relay-only** Qt/C++/QML email client: there is no IMAP or
SMTP anywhere on-device, and the backend at `mail.urlxl.com` is the sole
transport for mail, contacts, and push. It targets **Qt6/Kirigami KF6
only** across two surfaces from one codebase — **Linux Desktop** (KDE
Plasma primary, packaged as a Flatpak) and **KDE Mobile** (Plasma Mobile,
same Flatpak, mobile UI root). **Ubuntu Touch (Lomiri) support is
deferred** — Qt5 is EOL and this codebase no longer builds against it; see
Section 4 for the re-check trigger. It is the fourth sibling client after
the Android app (`~/git/kypost-android`) and the SwiftUI macOS/iOS app
(`~/git/kypost-for-Mac`). The authoritative design source for this repo
is `Linux_QT_Client_Plan.md` at the repo root — read it before making any
architectural decision; this file only summarizes and carries forward the
rules most likely to be violated by accident (note: that plan document
predates the Qt5-drop decision and still describes a dual-Qt/Ubuntu Touch
target — this file is the current source of truth on that point).

## 2. Repo layout map

Full detail (including per-subdirectory breakdowns) lives in
`Linux_QT_Client_Plan.md`'s "Repo layout" section — this is a terse pointer
map, not a duplicate:

```
core/       — libkypostcore: models/net/db/stores/domain/theme, QtCore+Network+Sql only
app/        — main.cpp, push/ (KUnifiedPush glue, Qt6-only), platform/ (SecureStore backends), qml/ (MobileRoot, DesktopRoot, pages, components)
tests/      — QtTest, stubbed HttpClient; ctest-driven
packaging/  — flatpak/ (Flatpak manifest + desktop file), dbus/ (session D-Bus activation .service), and click/ (Clickable manifest + apparmor)
po/         — gettext catalogs
docs/       — local tooling/setup notes (see docs/SETUP.md); docs/PARITY.md is the authoritative Android-parity matrix
```

## 3. Build instructions

A single out-of-tree build directory, Qt6-only:

```sh
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

A change is not verified until this builds cleanly and `ctest` is green.

`ctest` covers both the C++ tests and the QML suite (`QmlTests`, Qt Quick
Test, added 2026-07-26 — see `tests/qml/`). The QML tests run against fake
`com.urlxl.mail` singletons (`tests/qml/FakeSingletons.h`) and load their
components from the shipped `app/qml/qml.qrc`, so a component missing from
the resource bundle fails there rather than at runtime.

## 4. Locked decisions (do not relitigate)

Carried forward verbatim (in substance) from `Linux_QT_Client_Plan.md`'s own
"Locked decisions carried over" section:

- **Relay-only.** No IMAP/SMTP anywhere. `mail.urlxl.com` is the sole
  transport. Search is local-cache-only.
- **Wire contracts come from the backend Go source**, plus `kypost-android`'s
  `RelayModels.kt`/`ContactSyncModels.kt` and `kypost-for-Mac`'s
  `Data/Networking/*` (relay-only, live-verified, test-locked clients).
  **Never guess shapes** — guessed shapes have caused live 400s before.
- **The theme palettes are a binding contract**, sourced from
  `kypost-for-Mac/Style/AppTheme.swift`'s 7-field `ThemePalette` — not
  the web's 16-field `theme.ts`. Copy values, don't approximate.
  (Currently 15; `AppTheme::themeNames()` is the source of truth. This entry
  said "13" until 2026-07-25 — `Patina Ky`/`Polished Ky` were added later and
  the count was never updated, so don't trust a hardcoded number here again.)
- **StandardFolder wire names**: `INBOX`, `Drafts`, `Junk`, `Sent`, `Trash`,
  `Archive`; display name splits on both `/` and `.`.
- **90-second foreground refresh cadence.**
  **Corrected again 2026-08-22 — read this one, the 2026-07-25 correction
  was itself wrong and cost this client delta sync entirely.**

  `since` is **always sent**. `since=0` means "I have no cursor: send the
  whole window and tell me where it ends", and it is what `forceFullResync`
  now does. The authority is `kypost-server`'s
  `backend/internal/api/server_inbox.go`:

  ```go
  cursorSync := strings.TrimSpace(r.URL.Query().Get("since")) != ""
  ...
  "delta":   since > 0,
  "cursor":  result.Cursor,
  ```

  Sending `since` at all selects the **cursor protocol**; its **value**
  decides whether the window comes back partial. `cursor` is returned *only*
  on that path.

  The 2026-07-25 entry said the opposite — that a full snapshot required
  omitting `since`, because "`since` present at all, even `0`, puts the
  endpoint into delta mode". `planRefresh` was written to match, so it
  omitted `since` both on a forced resync **and on every cold start with no
  stored cursor**. That took the classic path, which returns no cursor, so
  nothing was ever persisted, so the next refresh had no cursor and omitted
  `since` again. The client never once entered delta mode: it re-downloaded
  the full 500-message window, bodies included, every 90 seconds, for the
  life of the profile. `server_inbox.go:290` names this exact conflation as a
  bug the backend had already fixed on its own side, and `kypost-android` has
  always sent `since=0` (`RelayMailSource.kt`'s `sinceValue()`).

  Two things follow, both now enforced by tests:
  - The returned cursor is persisted on **both** branches, not just when
    `delta` is true. A full window is the only way a first sync acquires a
    cursor at all.
  - Mail cursors are keyed per **(subscriberId, folder)**, matching Android's
    `MailCheckpoint`. The relay diffs a window per mailbox, so one global
    cursor asked Archive for a diff against a window only INBOX ever had.

  Do not "simplify" this back into an omitted `since`. Two rounds of this
  entry have now been wrong in opposite directions; the Go source above is
  the tiebreaker, not this file.
- **License: GPL-3.0-only** (fine with Qt LGPL / KDE).
- **Qt5/Ubuntu Touch (Clickable) support is dropped, not merely paused.**
  Qt5 is EOL upstream, so this codebase now hard-requires Qt6 and has no
  Qt5 build path at all (previously a dual-Qt CMake toggle selected between
  two separate build trees; that toggle and both trees are gone — see
  Section 3 for the current single build directory). This was a deliberate
  call, not an oversight: Ubuntu Touch is deferred until UBports ships a
  usable Qt6/KF6 track (the 24.04-2.0 beta, slated ~2026-07-20, or the
  further-out 26.04-1.x/Qt 6.9 line) — see Section 1. Re-check when UT
  ships that track; the plan is to target it directly with this same Qt6
  codebase, no separate build path.

Added 2026-07-25 (parity work with `kypost-android`):

- **No Flathub, ever.** Flathub bans applications that use AI and KyPost's
  backend does, so the app is permanently ineligible. Distribution is a
  self-hosted signed OSTree repo published to `gh-pages` by
  `.github/workflows/flatpak.yml` — see `docs/DISTRIBUTION.md`. Don't spend
  effort on Flathub-specific manifest compliance.
- **No PGP private key on this client, ever.** It handles public keys and
  protection-mode *awareness* only. Encrypted mail the server cannot read is
  explained to the user and routed to webmail — never decrypted here.
- **Hostile Location Protection toggles by relaunching the process.**
  `main.cpp`'s composition root is one unbroken chain of stack locals from
  `Database` down through every DAO, repository, controller and QML
  singleton; there is no supported way to re-point it at a different database
  at runtime. `AppRelauncher` spawns the replacement only *after*
  `app.exec()` returns, so `KDBusService(Unique)` has released
  `com.urlxl.mail` first. Don't "fix" that ordering.
- **MFA over push is a non-goal until the backend changes.** kypost-server
  derives `transport: "unifiedpush"` from `platform: "linux"` and filters
  unifiedpush devices out of every MFA challenge, so a Linux device is never
  notified. `MfaApproval.qml` was deleted for this reason; `MfaController` is
  kept. Confirm the server filter is gone before building any MFA UI.
- **Biometric unlock and window-content protection are non-goals** — no
  portable Linux equivalent (no `FLAG_SECURE` analogue on Wayland).

Added 2026-07-26:

- **No client-side push fallback beyond polling.** The push pipeline is two
  tiers: a system UnifiedPush distributor, else 90 s polling of the relay.
  The embedded ntfy subscriber that used to sit between them (client acts as
  its own distributor, registers `https://ntfy.sh/<topic>` as its
  deviceToken) was removed — `core/net/NtfySubscriber`,
  `app/push/NtfyTopicProvisioner`, `SettingsStore::pushServerBaseUrl` and the
  `ntfy-topic` SecureStore key all went with it. It was written as the
  Ubuntu Touch story (deferred) and was otherwise redundant with polling,
  which covers the same no-distributor case without sending the sender and
  subject of every mail through a third party. Don't reintroduce it; if
  no-distributor latency ever needs improving, shorten the poll interval.
  Reasoning in full: `core/domain/TransportStateMachine.h`.

## 5. Single-Qt rules

Qt5/KF5 support was dropped (Qt5 is EOL); Ubuntu Touch is deferred until it
ships its own Qt6/KF6 track — see Section 1 and the locked decision in
Section 4. There is no longer a KF5/KF6 divergence to guard against, so
most of what used to live in this section (dual-Qt QML-type restrictions, a
`Compat` singleton escape hatch) is moot and has been removed. One rule
carries forward unchanged, independent of the Qt5 decision:

- `core/`'s **QtCore/QtNetwork/QtSql-only boundary** is the rule most likely
  to be accidentally violated in a single commit: nothing in `core/` may
  pull in QtDBus, QtGui/QtQuick, KUnifiedPush, KNotifications, KF6::I18n, or
  any Lomiri glue. That code belongs in `app/`. Check `core/`'s own
  CMakeLists/includes before adding a dependency there.
  - Practical consequence seen in the PGP work: user-facing strings cannot
    live in `core/`, because `i18n()` needs KF6::I18n. The *decision* stays
    in core (`core/domain/PgpMessageState.h`); its *wording* lives in
    `app/mail/PgpMessagePresentation.h`.
  - **`core/` links one non-Qt library: OpenSSL's libcrypto**, PRIVATE, for
    `core/security/CredentialCipher.cpp`. Qt exposes no AEAD anywhere in its
    public API, and hand-rolling encrypt-then-MAC would be worse than linking
    what Qt already links for TLS. No OpenSSL type appears in any public
    header. `libssl-dev` is in the CI apt list; `org.kde.Platform` already
    ships it, so the Flatpak needs no module.

## 6. Ponytail, lazy senior dev mode

Use the smallest correct change.

1. Reuse what already exists.
2. Prefer stdlib and native platform APIs.
3. Add dependencies only when they remove meaningful code.
4. Fix shared root causes, not one caller.
5. If a shortcut has a limit, mark it with `ponytail:` and name the upgrade path.

Non-trivial logic must include one runnable check (unit test or minimal self-check).

## 6b. Rules added by the 2026-07-26 hostile review

These are not style preferences. Each one is here because breaking it
already produced a real defect in this repo.

- **Never discard a `bool`/`std::optional` returned by a store, a
  filesystem call, or a network service.** `SecureStoreKeychain::set()`
  returns false on any machine with no Secret Service provider running, and
  every caller ignoring it is how "pairing succeeded" could coexist with an
  empty keychain, and how the credential gate could report itself ON with
  the device secret still in plaintext. `QFile::write`, `QDir::mkpath` and
  `QProcess::startDetached` are the same story.
- **Do not use a Qt signal to order an operation whose result you need.**
  A signal returns `void`. `AppLockManager` used to announce seal/unseal
  through `credentialGateChanged` and could not learn whether it happened;
  that is now the `CredentialSealer` interface
  (`core/security/CredentialSealer.h`). If the caller must branch on
  success, inject a collaborator, not a connection.
- **Security controls are enforced in C++, not only in QML.** QML is a
  presentation layer. The PIN policy lives in `core/security/PinPolicy.h`
  and `AppLockManager` enforces it; `Settings.qml` explains it.
- **Every `Window` that can display mail or contact data instantiates
  `components/LockOverlay.qml`.** DesktopRoot creates real top-level
  Windows for pop-outs; an overlay anchored to the main window does not
  cover them. This shipped as a total app-lock bypass.
- **Anything QML-side that is a security control needs a QML test**
  (`tests/qml/`, ctest target `QmlTests`). The lock bypass above shipped
  because 73 C++ tests could not see a single `.qml` file.
- **Guard network-calling invokables with `ReentrancyGuard`**
  (`core/util/ReentrancyGuard.h`). `HttpClient` runs a nested `QEventLoop`,
  so QML keeps delivering clicks while a call is suspended. Where a guarded
  method legitimately calls another, split out an unguarded `*Internal`
  body rather than removing the guard.
- **Migrations are transactional.** `Database::open()` wraps each migration
  and its `user_version` bump in one transaction; statement splitting goes
  through `splitSqlStatements()`, not `QString::split(';')`.

## 6c. Rules added by the 2026-07-26 adversarial review

Same standard as 6b: each one is here because breaking it produced a real
defect.

- **Never persist a rotated device secret without asking the credential
  gate first.** The relay mints a new `deviceSecret` on every successful
  register, and re-registration runs unattended (endpoint rotation, tier
  change to Distributor) on essentially every launch. `PairingStore::save()`
  used to write it in plaintext regardless, next to a sealed blob and a
  `credentialPinGateEnabled` flag that both still claimed it was protected --
  a silent, persistent downgrade of the gate, while locked. `save()` is now
  sealed-aware, and anything that rotates the secret **must** check
  `PairingStore::canResealDeviceSecret()` *before* its network call
  (`DeviceRegistrationService::pair()` returns
  `RegistrationOutcome::CredentialsLocked` without contacting the server).
  Checking afterwards is not equivalent: by then the server has already
  retired the old credential.
- **Sealed is not the same as locked.** `sealDeviceSecret()` runs from
  Settings with the app unlocked and the user present; it keeps the session
  copy and key, and `lockDeviceSecret()` is what drops them. Asserting
  "load() is empty right after sealing" is wrong and used to break mail sync
  for the rest of the session with no lock screen to explain it.
- **The wipe path checks every result.** `wipeAllTables()`,
  `SecurityWipe::*`, `PairingStore::clear()` and `AppLockStore::clear()` all
  return `bool` and all can genuinely fail (no Secret Service, locked
  wallet). An unchecked wipe relaunches into a state that merely *looks*
  wiped. `PairingStore::clear()` returns `bool` for this reason -- don't
  revert it to `void`.
- **`core/` owns error *values*, never error *wording*.** A user-facing
  English sentence in `core/` cannot be translated (no KF6::I18n there) and
  travels straight to the UI. `NetworkError::CertificateMismatch` carries
  the fact; `components/StatusBanner.qml` carries the sentence. A pinned-cert
  mismatch also needs a persistent surface, not just a per-request error: it
  aborts every request forever, and re-pairing is the only recovery.
- **A locked app must not render mail content into a desktop notification.**
  The notification server draws it, so `LockOverlay.qml` cannot cover it.
  `NotificationDispatcher::setContentHidden()` is driven from
  `AppLockManager::locked` in `main.cpp`.
- **Validate QR/scan targets after resolving, and require TLS off
  loopback.** `isSafeQrTarget()` resolves the host, but Qt resolves it again
  when connecting -- an attacker's nameserver can answer differently the
  second time. TLS is what closes that; the address blocklist alone does
  not. Private/unique-local/multicast addresses are blocked too, loopback
  deliberately is not. DNS lookups on this path are bounded
  (`kResolveTimeoutMs`), never `QHostInfo::fromName()`.

## 6d. Rules added by the 2026-07-27 security audit (run 2)

Same standard as 6b and 6c: each one is here because breaking it produced a
real defect. The theme of this round is that the previous round's fixes were
applied to the *instance* that was reported rather than to the *class*.

- **`Text.AutoText` is banned. Every `Text` bound to wire or sender data sets
  `textFormat: Text.PlainText`.** QML's default runs
  `Qt::mightBeRichText()` and hands markup-shaped strings to the StyledText
  parser, which supports `<img src>` and fetches it over the *QML engine's*
  `QNetworkAccessManager`. That is a different network stack from
  `EmailDetail.qml`'s WebEngineView, so `settings.autoLoadImages` and
  `RemoteContentInterceptor` cannot see it: an `<img>` tag in a Subject header
  beaconed when the inbox list laid out, before the message was opened, while
  the UI said "Images are hidden to protect your privacy." An earlier fix set
  `textFormat` on two labels in `PgpScanContactKey.qml` and left the other
  ~25 sites; they are all set now. `tests/qml/tst_PlainTextRendering.qml`
  holds the line.
- **Security state is read from the authority that owns it, never inferred
  from a side effect.** `PairingStore` decided whether the credential gate was
  on by looking for its own sealed blob. `clear()` removes the blob and cannot
  reach `applock.credentialPinGateEnabled`, so unpair-then-re-pair wrote the
  device secret in plaintext under a flag still claiming protection; a failed
  keychain read did the same, because `SecureStore::get()` collapses "absent"
  and "read failed" into one `std::nullopt`. The gate is now flag-OR-blob, so
  both directions fail closed.
- **A precondition checked before a blocking call must be *captured*, not
  re-read after it.** `HttpClient` runs a nested `QEventLoop`, so a window
  minimise reaches `AppLock.lockNow()` mid-request. `DeviceRegistrationService`
  checked `canResealDeviceSecret()`, blocked, and then failed `save()` against
  a session key that had been dropped in between -- and responded by clearing
  the entire pairing, including the TOFU pin, after the relay had already
  rotated the secret. It now snapshots the sealing key first
  (`PairingStore::sealingKeySnapshot()`).
- **The TLS pin is per-reply and per-origin.** `QNetworkReply::encrypted`
  fires once per *connection*, so a pooled keep-alive reuse never fires it and
  a shared "last SPKI seen anywhere" member held whatever host handshook most
  recently -- letting a scanned QR code decide what the next unattended
  re-registration pinned as the relay's key. Read it from
  `HttpResult::peerSpkiSha256`. Enforce it only on the pinned origin: applying
  it to the deliberately cross-server PGP QR fetch raised a false "your mail
  server is being impersonated" banner on any third-party scan. And clear it
  (`HttpClient::clearCertificatePin()`) wherever the trust anchor is discarded
  or re-established, or the banner's own "unpair and pair again" advice cannot
  be followed without restarting the process.
- **Redirects are refused by default.** Qt's `NoLessSafeRedirectPolicy`
  follows cross-host redirects and strips nothing: every redirect status
  forwards `X-Kypost-Device-Secret`, and 307/308 forward the body. All four
  verbs now default to same-origin-only; pass a `RedirectValidator` to widen
  it deliberately.
- **Validate a response before persisting anything from it.** A 200 carrying
  any JSON object counted as a successful registration -- `ok` was parsed and
  read by nobody, and an empty `deviceId`/`deviceSecret` was stored over a
  working one. Combined with `reg`'s path being unconstrained (origin was
  checked, path was not), one deep link naming the user's *own* server could
  POST to `/api/health` and destroy the pairing.
- **Anything shown to authorize a security decision is displayed in its
  unambiguous form.** `QUrl::host()` defaults to `FullyDecoded`, which decodes
  punycode, and Qt applies no confusable-script policy. The pairing confirm
  dialog now shows `QUrl::FullyEncoded` and brackets IPv6 literals.
- **`QQuickOverlay` stacks above the lock gate.** Popups and
  `Kirigami.OverlaySheet`s left open when the app locks stay visible *and*
  clickable over the PIN screen, and being modal, their dimmer swallows clicks
  aimed at it. Both roots now close their root-scope popups on
  `AppLock.locked`, and `PairingController::removePairing()` -- which
  deregisters the device and destroys the credential -- carries the same
  `m_appLocked` guard its two siblings already had. Every PIN verification
  goes through `AppLockManager::verifyPinRateLimited()`, so the Settings
  prompts are subject to the lockout and the wipe threshold too.
- **Strip C0 controls from any externally-supplied filename.** `QFile::open()`
  passes the path to `open(2)`, which truncates at a NUL, while
  `QFile::exists()`/`remove()` reject the same string -- so a sender-chosen
  `Invoice.desktop\0.pdf` satisfied the ephemeral-attachment extension gate
  and landed as `Invoice.desktop`, and every cleanup call silently no-op'd.
- **Values used as indices or loop bounds are bounded on BOTH sides.**
  `PRAGMA user_version` was bounded only above and then indexed a
  function-pointer array; negative values called whatever sat before it, and
  `INT_MAX` made `version + 1` signed-overflow UB, which let the optimizer
  drop the migration loop and open an unmigrated database.
- **Hostile Location Protection is decided before anything touches the disk.**
  The pre-rename database migration ran first, so the memory-only mode copied
  the entire legacy database out to `kypost.db` on every launch before
  deleting it again. The legacy paths are now hoisted into `legacyDbPaths` and
  every wipe path names them.

## 6e. Rules added by the 2026-08-22 mail-sync correctness pass

Same standard as 6b–6d. The theme of this round is that a *documented*
contract was wrong, and every piece of code and every test faithfully
implemented the wrong thing — so the tests all passed while delta sync had
never once run.

- **A wire contract is settled by reading the server, not by reading this
  file.** §4's `since` entry had been "corrected" once already and was still
  wrong, in the opposite direction. Three layers agreed with it —
  `planRefresh`, `RelayMailSource`'s struct comment, and two tests that
  asserted `since` was absent — which made the wrong answer look
  triple-confirmed. `kypost-server`'s `server_inbox.go` settles it. When an
  entry here describes someone else's protocol, go and check.

- **A sync cursor is a promise that everything up to it was applied.** Never
  advance one past a write you did not confirm landed. `applyRefresh`
  discarded the `bool` from `insertOrReplace`/`deleteById`/
  `replaceFolderSnapshot` and then stored the cursor unconditionally: a
  failed write meant the relay would never mention those messages again, so
  they were gone from the device permanently, silently. It now returns
  `MailRepositoryOutcome::CacheWriteFailed` and leaves the cursor where it
  was. (This is 6b's "never discard a `bool`" rule, and here is what
  discarding one actually costs.)

- **A cursor is scoped to whatever the server scopes it to.** The relay diffs
  a window per mailbox, so one global `mailCursor()` asked Archive for a diff
  against a window only INBOX ever had. Keys are `(subscriberId, folder)`,
  percent-encoded — QSettings reads `/` as a group separator and IMAP folder
  paths contain it by design.

- **Finish a `QSqlQuery` before any DDL that touches what it read.** Qt keeps
  the `sqlite3_stmt` open after `exec()`, and SQLite answers `SQLITE_LOCKED`
  to a `DROP`/`ALTER` while a cursor is live on the same connection. A
  `PRAGMA user_version` left unfinished for the whole of `Database::open()`
  made migration 006's `DROP TABLE emails` fail — which `qFatal`s the app on
  every launch. `Database::open()` now finishes each migration statement, and
  the pragma queries, before the loop.

- **`QString()` binds as SQL NULL, not `''`.** `folder` became `NOT NULL` and
  half the PRIMARY KEY in migration 006, so a default-constructed
  `Email::folder` turned into a constraint violation — a silently dropped
  message, because most callers don't inspect the returned `bool`. Coalesce
  at the bind, and remember that `WHERE col = :x` with a null `QString`
  matches nothing, ever. A test that "passed" against a NULL folder was
  passing vacuously.

- **A test that asserts on a file must prove the file was written.** The
  first version of `wipeEverythingErasesTheSyncCursors` read `cursors.ini`
  after wiping and found none of the secrets it was looking for — because
  QSettings had not flushed it yet and the file did not exist at all. Seed
  through an object you then destroy, and assert the pre-state.

- **Anything with its own file has its own wipe path.** `cursors.ini`
  survived every wipe: `CursorStore::reset()` existed and had no caller
  anywhere in the app. It named the subscriber and every synced mailbox.
  `LocalDataWipeResult` gained a field for it, because a wipe that reports
  success while leaving a file behind is the failure mode that class exists
  to prevent.

## 6f. Rules added by the 2026-08-22 keychain-hang fix

- **A nested `QEventLoop` needs an exit that does not depend on the thing you
  are waiting for.** `SecureStoreKeychain::runBlocking` had exactly one:
  `QKeychain::Job::finished`. QKeychain does not emit that signal when its
  underlying D-Bus call gives up (measured, not assumed), so the loop had
  nothing left to quit it and the application hung forever at startup, before
  any window existed and with nothing in the journal. Every other nested loop
  in this repo was already guarded — `HttpClient` by `transferTimeoutMs`,
  `PgpQrTargetValidator` by its own `QTimer` — so this was the last one.
  Adding a new one without a timer is a regression.

- **Measure where a block actually is before claiming to have fixed it.** The
  first diagnosis of the above was "add a timeout and it is bounded". Timing
  `start()` and `exec()` separately showed the first ~25 s are spent inside a
  *synchronous* D-Bus call (Qt's default 25000 ms) with the thread not
  processing events, so no `QEventLoop` timer is delivered until it returns.
  The timer guarantees termination; it does not and cannot make this fast.
  A fix described in terms of the wrong mechanism would have had the next
  person tuning the timeout down and wondering why nothing improved.

- **Startup code that touches the secret store stops at the first failure.**
  Each blocked call costs ~25 s and `main()` runs before any window exists.
  The canary used to push on through `set` → `get` → `remove` → `contains`,
  paying that floor repeatedly to re-learn an answer it already had.

- **When forcing a timing path in a test, verify the forcing is
  deterministic.** `timeoutMs=1` looked like it would force the timeout and
  was in fact racy — 20 runs gave 15 `Absent` and 5 `Failed`, which would
  have been a flaky test asserting a security-critical mapping.
  `timeoutMs=0` was 20/20. Run the experiment; do not reason about which
  millisecond wins.

## 6g. Rules added by the 2026-08-22 stale-reply pass

- **A reply may outlive the pairing that authorised it, and every write must
  assume it did.** No table in this schema has a subscriber column: `emails`,
  `contacts`, `groups` and `push_notifications` are per-DEVICE. Pairing a
  different account purges them (`LocalDataWipe::wipeCachedAccountData`), but
  a request already in flight completes *after* that purge and writes its rows
  back in behind it — the previous account's mail, in the new account's inbox,
  readable by whoever is now holding the machine. So an apply step compares
  `PairingStore::stillCurrent(identity)` against the identity its plan
  captured, and writes nothing when it has moved. Done at every
  place a relay reply reaches local storage: `MailRepository::applyRefresh`,
  `PushRepository::pullOnce`, `FolderRepository::applyList`/`applyMutation`,
  `GroupsRepository::applyRefresh`, `ContactSyncRepository::applySync`,
  `ContactPhotoRepository::photoPathFor`, attachment downloads, and the cached
  PGP compose state and recipient preflight. **Adding a new one is adding this
  check** -- a plan that does not carry a `PairingIdentity` is a plan that
  cannot answer the question.

- **The identity is (subscriberId, deviceId) — never the device secret.** The
  credential gate re-saves the pairing on every lock and unlock, so keying on
  `deviceSecret` would discard every legitimate reply that spanned one. It
  includes `deviceId` because re-registering the *same* account mints a fresh
  registration, and the previous one's replies have no claim on it.

- **An unpaired store is not "unchanged".** `stillCurrent()` returns false
  when nothing is paired, even if the identity handed to it is also empty.
  There is no account to file a reply under, so the only correct action is to
  throw it away.

- **Re-reading a store to COMPARE is not the TOCTOU that re-reading it to USE
  is.** `MailRepositoryTest` previously asserted `applyRefresh` performed no
  store reads at all, which conflated the two. Every value the apply step
  *uses* — the cursor's owner especially — still comes from the plan. The one
  read added here exists solely to answer "is this still ours", which the plan
  by construction cannot answer about itself.

- **Discarding a stale reply is not an error to report.** `PairingChanged` is
  its own outcome and `MailController` shows nothing for it. "Refresh failed"
  would be a lie shown to someone who has just paired successfully.

- **Prove the guard by removing it.** All four tests added here were run
  against the un-guarded code first and confirmed to fail. A concurrency test
  that has never been seen to fail is not a test.

- **A stale reply can DELETE as easily as it can leak.** `applySync`'s tooOld
  branch drops every contact and clears the cursor; `applyList` does a
  snapshot replace. Run against a reply belonging to a pairing this device no
  longer has, those destroy the NEW account's data on the previous account's
  say-so. The guard is not only about keeping data in, and an apply step that
  deletes needs it more than one that inserts.

- **The pending queue is not the server's to invalidate.** `applySync`
  discards the reply but leaves `PendingContactChangeDao` untouched: those are
  local edits nobody has accepted yet, and dropping them because the account
  changed would silently throw away the user's own work.

- **An attachment is not just a file on disk.** `storeDownloadedAttachment`'s
  ephemeral branch hands the file straight to the desktop to open, so a stale
  attachment reply does not merely leave the previous account's data around --
  it displays it to whoever is using the machine now. Refused before the write,
  not after.

- **A stale answer about keys must clear, never persist.** The PGP compose
  state decides whether sign/encrypt are offered at all, and the recipient
  preflight warns which addresses have no key. Carrying either across an
  account change would either hide controls from someone entitled to them or
  show a false all-clear -- so the bootstrap answer is left unfetched (the next
  compose open asks again) and the preflight list is cleared.

## 7. DOX framework

### Core Contract

- AGENTS.md files are binding contracts for their subtree.
- Read from root to nearest AGENTS.md before editing.
- The nearest AGENTS.md controls local details; parent docs keep global rules.

### Update After Editing

- Run a DOX pass for every meaningful change.
- Update nearest owning AGENTS.md when behavior, responsibilities, or verification changes.
- Keep Child DOX Index entries current and delete stale rules.

### User Preferences

- Best-effort 90-second keyword refresh policy (foreground cadence; background catch-up on resume).
- Relay-only: never add IMAP/SMTP client code.
- DOX hierarchy scope is app-only.

### Child DOX Index

(none — no subdirectory AGENTS.md files exist yet)

## 8. Known live-system gotchas

- **Re-registration silently 401s once the pairing token expires.** Token
  rotation after pairing never reaches the server (latent, unfixed
  server-side). Don't re-register on every launch and assume it worked —
  handle the 401 explicitly.
  **Handled 2026-07-26:** both `reregisterIfPaired()` call sites in
  `main.cpp` go through a `reregisterAndReport` lambda that inspects the
  `std::optional<NativeRegistrationResult>` and sets
  `PairingController::reregistrationRejected`, which both QML roots bind to
  a persistent banner (`components/StatusBanner.qml`). Before that, both
  sites discarded the result and the only symptom was push silently
  stopping behind a green "Paired" badge.
- **`mail.urlxl.com` sits behind Cloudflare** (bare `urlxl.com` → 530) and
  needs a real, non-default User-Agent set on QNAM for every request from
  day one — a bare/default UA gets blocked.
- **Deployment lag.** The backend deploys via a separate Docker pipeline on
  a remote host; a commit landing in the backend repo does not mean it is
  live yet. Verify the relevant backend commits are actually deployed to
  `mail.urlxl.com` before running any live end-to-end test — this has
  previously produced a "committed but 404s live" window.
- **The Flatpak pipeline was red from its first run (2026-07-26) until
  2026-08-22 — 40 consecutive failures, never once green.** Four independent
  faults, stacked, each only reachable once the one above it was fixed:
  1. `appstreamcli compose` could not read the scalable icon, because
     ubuntu-24.04's appstream 1.0.2 decodes via gdk-pixbuf and the runner had
     no librsvg loader. Fixed by installing `librsvg2-common`.
  2. `flatpak build-finish` rejected `--socket=pipewire`; that socket type
     needs flatpak 1.15.4 and the runner has 1.14.6. Now spelled
     `--filesystem=xdg-run/pipewire-0`, which is the same grant and works on
     every version.
  3. **A real defect in the shipped artifact**: qtkeychain and zxing-cpp
     resolved `CMAKE_INSTALL_LIBDIR` to `lib64`, but a flatpak's linker path
     is `/app/lib`. Any bundle this workflow had published would have failed
     to launch anywhere. Both modules now pass `-DCMAKE_INSTALL_LIBDIR=lib`.
  4. The launch smoke test had no D-Bus session bus, which
     `KDBusService(Unique)` requires. Now wrapped in `dbus-run-session`.

  Two lessons worth more than the four fixes. **A permanently-red job hides
  everything behind it**: the smoke test that caught fault 3 — a bug in what
  users would have installed — had never executed once. And **when a tool
  reports an issue name with no detail, make it talk before theorising**:
  `appstreamcli compose` is invoked by flatpak-builder without
  `--print-report`, so for a month the log said `file-read-error` and nothing
  else. A failure-only diagnostic step that re-runs compose with
  `--print-report=full` now lives in the workflow; it named the icon in one
  run. Do not delete it.

  Still outstanding: `FLATPAK_GPG_PRIVATE_KEY` is not configured, so
  `steps.mode.outputs.publish` is false and nothing is published even now that
  the build is green. Green build != shipped app; see docs/DISTRIBUTION.md.

- **The Secret Service costs ~25 s on its first call when a collection is
  locked.** Measured on a live gnome-keyring, 2026-08-22: `QKeychain::Job`
  `start()` returns instantly, the first `QEventLoop::exec()` blocks for
  ~25.0 s (Qt's default D-Bus call timeout), and `finished` is then never
  emitted at all. Later calls in the same process are instant. Two
  consequences worth remembering: a first-run test against a real keyring
  legitimately takes ~25 s and is not hung, and no `QEventLoop` timer can
  shorten that window because the thread is not processing events during it.
  `SecureStoreKeychain` bounds it; see AGENTS.md §6f.
- **CI Pipline Failes** Before commiting any change check the CI pipline code.
  EVERY Push to main not ment to fix the CI pipeline has broken the CI pipeline.
