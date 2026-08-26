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
the Android app (`~/busness.app/kypost-android`) and the SwiftUI macOS/iOS
app (`~/busness.app/kypost-for-Mac`); the relay is
`~/busness.app/kypost-server`. **This file and `docs/PARITY.md` describe
what exists today.** `Linux_QT_Client_Plan.md` at the repo root is the
original architectural design record and is worth reading for the
reasoning, but it predates the Qt5-drop, the parity matrix and the
client-side OpenPGP reversal; where it disagrees with this file or
`docs/PARITY.md`, it is the stale one. It says so itself in its own
header — do not resolve a conflict in its favour.

## 2. Repo layout map

Full detail (including per-subdirectory breakdowns) lives in
`Linux_QT_Client_Plan.md`'s "Repo layout" section — this is a terse pointer
map, not a duplicate:

```
core/       — libkypostcore: models/net/db/stores/domain/theme/version, QtCore+Network+Sql only
app/        — main.cpp, push/ (KUnifiedPush glue, Qt6-only), platform/ (SecureStore backends), qml/ (MobileRoot, DesktopRoot, pages, components)
tests/      — QtTest, stubbed HttpClient; ctest-driven. tests/qml/ is the QML suite (QmlTests); tests/guards.tsv is the security-guard manifest (§5a)
packaging/  — flatpak/ (manifest, desktop file, metainfo, notifyrc, icons), dbus/ (session D-Bus activation .service), click/ (empty .gitkeep placeholder — Ubuntu Touch is deferred, there is no Clickable manifest)
po/         — gettext catalogs
scripts/    — build-sqlcipher.sh, verify-guards.sh, verify-version.sh, verify-pgp-against-relay.sh
docs/       — local tooling/setup notes (see docs/SETUP.md); docs/PARITY.md is the authoritative Android-parity matrix; docs/THREADING.md records the async-controller constraints
```

## 3. Build instructions

A single out-of-tree build directory, Qt6-only:

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

A change is not verified until this builds cleanly and `ctest` is green.

`RelWithDebInfo` is part of the command, not decoration: the hardening flags
in `CMakeLists.txt` include `_FORTIFY_SOURCE=3`, which does nothing without
`-O`. A build with no configuration verifies unfortified code. Use
`-DCMAKE_BUILD_TYPE=Debug -DKYPOST_SANITIZE=ON` for the sanitizer build, which
CI runs as a second pass over the same tree.

**Add `-DKYPOST_SQLCIPHER_ROOT=...` before believing a green `ctest`.** Without
it, configure prints `SQLCipher: not configured` and the encryption tests
`QSKIP` rather than fail — so the suite goes green having never exercised the
at-rest encryption path, and `scripts/verify-guards.sh` reports the guards on
that path as `NOT RUN`. `./scripts/build-sqlcipher.sh /tmp/sqlcipher` builds one
with the SONAME and the column-metadata flag §6h requires; both CI and the
Flatpak manifest do exactly this.

`ctest` covers both the C++ tests and the QML suite (`QmlTests`, Qt Quick
Test, added 2026-07-26 — see `tests/qml/`). The QML tests run against fake
`com.kysecurity.mail` singletons (`tests/qml/FakeSingletons.h`) and load their
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
- **License: MIT** (fine with Qt LGPL / KDE).
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
- ~~**No PGP private key on this client, ever.**~~ **REVERSED 2026-08-22 at
  the maintainer's direction.** The rule stood from 2026-07-25 to
  2026-08-22 and read: "It handles public keys and protection-mode
  *awareness* only. Encrypted mail the server cannot read is explained to
  the user and routed to webmail — never decrypted here." Kept here rather
  than deleted, because a reversed decision is worth more than a vanished
  one: anything written against the old rule (the webmail hand-off in
  `MailController`, `pgpComposeStateOf`, the wording in
  `PgpMessagePresentation`) was correct under it and is now legacy, not
  precedent. See §4a for what replaces it.
- **Hostile Location Protection toggles by relaunching the process.**
  `main.cpp`'s composition root is one unbroken chain of stack locals from
  `Database` down through every DAO, repository, controller and QML
  singleton; there is no supported way to re-point it at a different database
  at runtime. `AppRelauncher` spawns the replacement only *after*
  `app.exec()` returns, so `KDBusService(Unique)` has released
  `com.kysecurity.mail` first. Don't "fix" that ordering.
- **MFA over push is a non-goal on Linux.** Settled 2026-08-23 by the user:
  Linux cannot do MFA push. Not a backend oversight to wait out -- the server
  excludes it deliberately, and `MFATransportEligible`'s own comment gives the
  reason: an MFA challenge carries sign-in metadata (IP address, user agent,
  and the match digits themselves), and UnifiedPush delivers through an
  unencrypted public broker such as ntfy.sh. `normalizeNativeTransport` maps
  platform `linux` to that transport, so this client is excluded by
  construction. Those devices stay fully usable for mail notifications.

  Earlier wording here said the filter was "explicitly temporary" and told
  readers to "confirm the server filter is gone before building any MFA UI".
  That misdescribed a privacy control as an oversight, and pointed at a
  condition nobody should be waiting for. What would actually have to change
  is end-to-end encryption of the push payload itself (RFC 8291 is what web
  push uses); that is a UnifiedPush-ecosystem change, not a KyPost one.

  `MfaApproval.qml` was deleted for this reason on 2026-07-25, and
  `MfaController`, `MfaResponseClient` and `MfaChallenge` followed on
  2026-08-24. They were dead -- nothing constructs a challenge for them to
  answer -- but dead-and-compiled reads as finished machinery awaiting a
  reconnect, which is the mistake this note exists to prevent. Do not
  reintroduce them. If UnifiedPush ever gains RFC 8291 payload encryption and
  the relay drops the filter, recover the implementation from git rather than
  keeping a stub warm against a change nobody is waiting for.
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

## 4a. Client-side OpenPGP (decision reversed 2026-08-22)

Client-side decryption and signing are now in scope.

**Custody model, revised 2026-08-23: use the server's authenticated ECDH
device-enrolment ceremony, then delegate durable custody to the user's
`gpg-agent` via GPGME.** KyPost generates a non-persistent P-256 key, publishes
its public point, and shows the same time-bucketed verification code as
webmail. Webmail unlocks the account key locally and seals it to that device
key. KyPost briefly handles the decrypted key bytes only to import them into
GnuPG, then zeroises both the envelope plaintext and the ECDH private key. It
never receives the account password or OpenPGP passphrase. After import, keys
stay where the user keeps them, pinentry handles passphrases, and hardware
tokens and smartcards continue to work without a second custody system.

Two consequences worth being explicit about, because they read as weaknesses
and are the accepted cost of the model:

  * The feature needs a working GnuPG. Without one, `engineAvailable()` is
    false and the app says so once rather than reporting every message as
    undecryptable.
  * In a Flatpak it needs `--socket=gpg-agent`. Without that grant, gpgme
    talks to a gpg inside the sandbox with an empty keyring — so the
    permission goes in with the change that wires decryption into the mail
    path, not before it, and not without saying why.

Most of what follows therefore does not apply to this implementation: there
is no key at rest here to protect, and no passphrase to keep out of a
`QString`. They stay because they are the rules for anything that DOES hold
key material, and reversing the custody decision later must not also quietly
delete the constraints that come with it.

The rules below apply to any implementation, under either model. They are
written now, before the code, because every one of them is easier to design
in than to retrofit.

- **Private key material never touches `settings.ini`, the SQLite database,
  or any file this app writes in the clear.** Not even the encrypted
  database: it is decrypted for the whole session and is the wrong home for a
  key whose whole point is to be unavailable while the app is idle.

- **Key material and passphrases never live in `QString`.** It is implicitly
  shared and copy-on-write, so a passphrase in one is duplicated by every
  assignment and cannot be reliably cleared — `QString::clear()` frees a
  buffer it may not be the only owner of. Anything holding secrets needs a
  type that owns its buffer and zeroises on destruction, and it needs to
  exist before the first line of PGP code, not after.

- **A decryption failure is never rendered as a message.** Wrong key, corrupt
  payload, unsupported algorithm and "no key for this recipient" are four
  different answers with four different things for the user to do, and none
  of them is showing the ciphertext or an empty body as though it were the
  mail. `PgpMessagePresentation` already draws this distinction for the
  server-side modes; the client-side ones must not collapse it.

- **A PGP payload is attacker-controlled input.** It arrives from whoever
  sent the mail. Parsing it needs a bound on the decrypted size (the wire
  bound in `HttpClient` does not cover expansion) and it must not be able to
  make this app allocate or recurse on the sender's say-so.

- **The account-identity rules in §6g apply unchanged.** A decrypted body is
  cached mail like any other: it belongs to the account that fetched it, and
  a reply that outlives its pairing is discarded.

- **Enrolment must be authenticated.** However a key reaches this device, the
  path that brings it must not be one a webpage, a mail message or a
  malicious QR can drive on its own. `PgpQrTargetValidator` exists because
  the public-key path already had this problem.

### 4b. Recipient public keys go into the user's own GnuPG keyring

Decided 2026-08-23 by the user, after the three options were put to them.

Encrypting to a recipient requires gpg to hold their key. The alternatives
were an ephemeral keyring per send (contained, but gpg keeps no record, so a
recipient's key changing under them is invisible) and a pinning store of our
own (detects that, but is a second and weaker copy of key-change rules gpg
already models). The keyring was chosen: it is what every other mail client
does, gpg owns the record, and a changed key surfaces in the user's own
tooling rather than only inside this app.

The consequences are binding on anything that touches `core/pgp/OpenPgpKeyImporter`:

- **Sending mail modifies the user's keyring.** That is a side effect on state
  this app does not own, so importing must stay idempotent -- gpg merges, so a
  resend is a no-op -- and must never delete.
- **The fingerprint the relay claims is checked in a scratch GNUPGHOME first.**
  A key whose computed fingerprint does not match the claim never enters the
  real keyring, not even briefly. Importing and then deleting would mean this
  code deletes from a keyring it did not create, and an interrupted run would
  leave the bad key behind.
- **"gpg merges rather than replaces" is measured, not assumed.**
  `OpenPgpKeyImporterTest` imports a second key and a newer copy of an existing
  one and re-reads the keyring, because the whole custody decision rests on it.
- **This does not make the relay trusted.** The fingerprint check catches a
  relay whose key and whose claim about that key disagree; it cannot catch one
  that lies consistently. Persisting into the keyring gives the user a chance
  to notice. Say "a chance", never "prevents".

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
  - **`core/` links exactly three non-Qt libraries, all PRIVATE, and no type
    from any of them appears in a public header**: OpenSSL's libcrypto
    (`core/security/CredentialCipher.cpp` — Qt exposes no AEAD anywhere in
    its public API, and hand-rolling encrypt-then-MAC would be worse than
    linking what Qt already links for TLS), libargon2 (the memory-hard KDF
    behind the same seal), and gpgme — **the C API, never the `gpgmepp` C++
    wrapper**, which broke CI: KDE neon's `libgpgmepp-dev` wants gpgme >= 2.0
    while Ubuntu noble carries 1.x, and CI layers both archives. A fourth,
    SQLCipher, is linked PUBLIC and only when `KYPOST_SQLCIPHER_ROOT` is set;
    §6h is why it cannot be an ordinary distro package. Adding a fifth is a
    decision, not a build tweak. `org.kde.Platform` ships libssl, so the
    Flatpak needs no module for it; libargon2, sqlcipher, qtkeychain,
    zxing-cpp and kunifiedpush are all built from source in the manifest.

### 5a. A guard is not proven until removing it turns a test red

`scripts/verify-guards.sh` does that for every guard in `tests/guards.tsv`:
neutralises one at a time, requires the named test to fail, and puts the file
back. Run it after touching anything in the table, and add an entry when you
add a guard whose failure means reading somebody else's mail, sending
plaintext, or handing a message to the wrong account.

It exists because doing this by hand is unreliable in a specific way. Twice on
2026-08-23 a hand-applied neutralisation did not match the source -- once
because the replacement ignored an if-init-statement -- so nothing was
neutralised, the test passed, and the guard went into a commit message as
"proven by neutralisation" having proven nothing. A neutralisation that cannot
be applied is indistinguishable from a passing test unless something checks,
so the script checks: an absent or ambiguous search line is a FAILURE, and so
is a replacement that leaves the bytes unchanged.

Search lines are matched WHOLE, indentation included. A deeper-indented copy
of the same statement is a different guard, and substring matching reported
the two as one ambiguous entry.

A test that SKIPPED is reported as `NOT RUN`, not as proven and not as still
green. QtTest exits 0 for a skip, so a build without SQLCipher — which skips
the whole migration case in `initTestCase()` — looked exactly like a guard
that had been removed without the test noticing. It counts against the run
either way: a guard this build cannot speak to is a guard nobody has checked.

The first run found two real things, which is the argument for having it:
`oneMissingRecipientKeyFailsTheWholeMessage` proved only the pre-flight key
lookup and stayed green when the post-encryption `invalid_recipients` check was
removed -- a second, uncovered branch, now tested with an expired key. And the
decrypt path's `stillCurrent` check cannot be proven from outside at all,
because the read-time guard already refuses to hand the plaintext out; that is
recorded in the manifest rather than papered over.

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
- ~~**Guard network-calling invokables with `ReentrancyGuard`.**~~
  **Superseded — `core/util/ReentrancyGuard.h` no longer exists.** The rule
  was correct while controllers blocked: `HttpClient` runs a nested
  `QEventLoop`, so QML kept delivering clicks into a method suspended
  half-way through its own state changes. Every controller is now
  asynchronous and nothing can re-enter what is never suspended, so the
  guard was deleted with no users left. **The obligation it carried did not
  go away**: a new network-calling invokable must publish a `busy`/`inFlight`
  property, set synchronously before it returns, and coalesce repeat calls —
  and a new nested `QEventLoop` anywhere needs an exit that does not depend
  on the thing it is waiting for (§6f). `docs/THREADING.md` is the record of
  which constraints decided that shape, and names the two GUI-thread blocking
  calls that remain.
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

- **"Could not read it" is not "it is not there", and the last place that
  conflation lived was `PairingStore::load()`.** `sub` decides paired-vs-not,
  and paired-vs-not decides whether a registration is a REPLACEMENT and
  therefore whether the previous account's cache is purged. `loadChecked()`
  reports Unreadable separately and `PairingController` refuses to register on
  it. Worth knowing what this did and did not fix: the case was already
  refused before the request, because `credentialGateEnabled()` fails closed
  and `beginPair()` then cannot reseal. The first write-up of this change
  claimed it closed a live leak; measuring it showed otherwise. What it
  actually buys is an error message that names the keyring rather than telling
  a user with no lock to "Unlock KyPost first", and a protection that is
  stated here instead of falling out of an unrelated check two layers down.

- **A wipe that is not recorded before it starts cannot be known to have
  failed.** The wipe-after-ten-attempts path relaunches either way, and a
  wipe killed part-way writes nothing at all -- the code that would have
  logged it is the code that did not run. `WipeTripwire` goes down before the
  first byte is erased and comes up only after a wipe that reported every
  component gone; anything else leaves it armed, and finding it at startup
  means "do it again". Retrying is safe because every step of `LocalDataWipe`
  is idempotent.

- **Recovery runs before anything reads the state the recovery changes.**
  A completed recovery erases the stored PIN, so it happens above
  `AppLockManager`'s construction in `main()`. Built first, it would be
  holding the pre-wipe answer to "is there a lock".

- **`Item.visible` reports EFFECTIVE visibility, so a component may not
  compute its own visibility from it.** `StatusBanner` read
  `visible: height > 0` at the root while each row read
  `implicitHeight: visible ? h : 0`. A row inside an invisible root reports
  visible=false whatever its own binding says, so it contributed no height,
  so the root stayed invisible. A closed loop with a stable answer of "never
  show anything" -- every warning that component carries, including the
  certificate-mismatch banner, was unreachable. Measured, not deduced: the
  three tests in `tst_IncompleteWipeBanner.qml` fail against the old binding
  in both the already-true-at-creation case and the false-to-true case. Ask
  the MODEL what is active; never the laid-out geometry.

- **Making a protection configurable means deciding what the "off" setting
  actually switches off.** The wipe-after-N-attempts threshold and the
  per-process "refuse further guesses" floor were the same constant. Exposing
  the threshold without splitting them would have meant a user choosing
  "never erase this device" also switched off the only rate limit an attacker
  cannot defeat by moving the system clock forward. They are now
  `LockoutPolicy::shouldWipe(attempts, threshold)` and
  `shouldRefuseForSession(attempts)`; the erase is the user's to decline, the
  rate limit is not. `AppLockManagerTest::switchingTheEraseOffKeepsTheRateLimit`
  fails if they are ever rejoined.

- **Security policy goes in `AppLockStore`, never `SettingsStore`.**
  settings.ini is a plain text file, so anything stored there can be changed
  by whoever has file access -- which is the access level these policies exist
  to survive. Same reasoning that already put `lockEnabled` behind the secret
  store, now also applied to the erase threshold. Read it back defensively
  too: absent, unparseable and out-of-range all resolve to the DEFAULT, never
  to the permissive value, so an unreadable keyring is not mistaken for the
  user asking for the erase to stop.

- **"Immediately" must not be implemented as a zero-delay timer.** The
  background-lock grace period defaults to 0, and that path locks
  synchronously inside `lockAfterGrace()`. A `QTimer::start(0)` would satisfy
  any test that merely waits and then checks, while leaving the app unlocked
  for the remainder of the current event-loop pass -- the exact window "lock
  immediately" exists to close, on the path almost every user is on.
  `theDefaultGraceLocksSynchronouslyWithNoTimer` spins no event loop between
  the call and the assertion, and fails if this is ever changed.

- **An explicit lock cancels a pending one.** `lockNow()` stops the grace
  timer BEFORE its early return, so a timer started while the lock was on
  cannot fire after the user has turned the lock off. A user who asks to lock
  is not asking to lock in five minutes.

## 6h. Rules added by the 2026-08-22 SQLCipher spike

- **`PRAGMA key` against ordinary SQLite is not an error.** It is an
  unrecognised pragma: silently ignored, and `QSqlQuery::exec()` returns
  TRUE. A build linked against stock libsqlite3 runs the entire encryption
  path, reports success at every step, and writes a database readable in a
  text editor -- with every layer above believing the mail on that disk is
  encrypted. Measured, in exactly that configuration. So `Database::open()`
  never trusts the pragma that sets the key: it asks `PRAGMA cipher_version`
  afterwards and refuses to open if the answer is empty.

- **SQLCipher must carry the SONAME `libsqlite3.so.0`.** Qt's stock SQLite
  driver plugin has a DT_NEEDED on that name. Given a library with it, the
  plugin drives SQLCipher unchanged -- no vendored Qt driver, no private Qt
  SQL headers, no custom plugin, none of which this repo needs and all of
  which the first design assumed. Without it (Debian's renamed
  `libsqlcipher.so.0`, or a build with no SONAME at all) the system SQLite is
  mapped alongside ours and TWO sqlite implementations live in one process;
  ours happens to win by symbol interposition, which is not something to
  ship. Verified both ways with `LD_DEBUG=libs`, counting `calling init`
  lines.

- **`SQLITE_ENABLE_COLUMN_METADATA` is required.** Qt's driver imports
  `sqlite3_column_table_name16`. A SQLCipher built without it makes Qt fail
  to load its own driver and report "Driver not loaded", which says nothing
  about the actual cause. `scripts/build-sqlcipher.sh` checks for the symbol
  before it exits.

- **Linking a library does not make it load.** Debian and Ubuntu pass
  `-Wl,--as-needed` by default. Nothing in this codebase references a sqlite3
  symbol -- every SQL call goes through Qt's driver plugin, not through us --
  so the linker dropped the `libsqlite3.so.0` DT_NEEDED as unused. RUNPATH
  survived, pointing at a library that was never mapped; Qt's plugin resolved
  its own sqlite from the system, and `PRAGMA key` became the silent no-op
  above. CI failed on exactly this while the same commit passed on a distro
  that does not default to `--as-needed`. `Database::encryptionAvailable()`
  now makes one real call into the library, which is what keeps it. Verified
  by adding the flag locally and watching the DT_NEEDED disappear and come
  back.

- **Prove a dependency is what it claims at configure time, not at runtime.**
  `check_library_exists(... sqlite3_key ...)` fails the build with a message
  naming the path, rather than leaving `Database::open()` to refuse
  everything later with no clue as to which prefix was wrong.

- **A wrong-length raw key is refused, not passed on.** SQLCipher treats
  anything that is not exactly 32 raw bytes in `x''` form as a passphrase and
  runs PBKDF2 over it -- so a truncated key still opens a database, just a
  different one, silently, under a different key.

- **`grep -q` under `set -o pipefail` fails on success.** grep exits the
  moment it matches, the producer gets SIGPIPE, and the pipeline reports
  failure. `scripts/build-sqlcipher.sh` reported "built without
  SQLITE_ENABLE_COLUMN_METADATA" against a library that had the symbol. Use
  plain `grep ... > /dev/null`, which reads its input to the end.

- **`sqlcipher_export` does not copy `PRAGMA user_version`.** Left at 0, the
  converted database looks brand-new to `Database::open()`, every migration
  is replayed over rows that already have the changes, and the open fails --
  on a database that is otherwise perfectly good. Copied explicitly, and
  `DatabaseEncryptionMigrationTest` fails in four places if that line is
  removed.

- **Losing the mail is worse than leaving it unencrypted for another
  launch.** Every failure path in `DatabaseEncryptionMigration` is written
  that way round: the encrypted copy goes to a separate file, is verified
  table by table against the original, is put in place and re-opened, and
  only THEN is the plaintext securely deleted. A marker is armed before
  anything changes, so an interrupted run is repaired on the next launch --
  including the one moment between the two renames where the profile has no
  database at its usual path.

- **Never mint a key because the keyring did not answer.** The obvious
  `loadOrCreate()` shape, built on `SecureStore::get()`, does exactly that
  against a locked keyring -- and writes the new key over the real one, after
  which the database is unopenable by anyone including its owner, forever.
  `DatabaseKeyStore::existing()` reports Unreadable and Corrupt as their own
  answers for this reason alone.

## 6i. Rules added by the 2026-08-24 security-boundary pass

Same standard as 6b–6h. The theme of this round is that all three sites
carried a comment promising the property the code did not provide, which is
worse than no comment: the next reader stops looking.

- **`gpgme_op_import()` imports EVERY key in the blob.** It reports them as a
  linked list, and `importInto()` read only the head of it — under a comment
  saying that was what stopped bundle smuggling. A relay could return the
  expected key followed by keys of its own choosing and the whole lot went
  into the user's real keyring. The full status list is now walked in the
  scratch GNUPGHOME and anything other than one primary fingerprint is
  refused before the real keyring is touched. **Compared by fingerprint, not
  by counting entries** — a single secret key legitimately reports its
  fingerprint twice. Measured against gpgme, not assumed.

- **A partial secret-store outage fails the whole load.** Only `sub` went
  through `SecureStore::read()`; the other seven fields used `get()`, which
  collapses "the store could not be consulted" into an empty string. A Secret
  Service that answered the first request and failed on the certificate pin
  produced a pairing that loaded successfully **with no pin**, configured
  `HttpClient` with no TOFU enforcement, and — since only mutations drop the
  cache — was never re-read for the rest of the session. Every field uses
  `read()`; any `Failed` fails the load and nothing is cached. This is §6d's
  "absent is not unreadable" rule applied to the whole record rather than to
  the one field somebody reported.

- **Hostile Location Protection erases BEFORE it relaunches, and refuses the
  mode if that erase fails.** The failure used to be logged and discarded: the
  replacement process came up memory-only, showed the protection as on, and
  the mail it was supposed to have destroyed was still on the disk.
  `AppLockManager` now erases through an injected callable and, on false,
  reverts the setting, raises the wipe-incomplete banner, and does not
  relaunch. Deliberately **not** the existing `WipeTripwire`: its recovery
  path calls `wipeEverything()`, which would turn a failed transition into an
  unpaired device. Startup aggregates its own cleanup through
  `SecurityWipe::eraseOnDiskProfile()` and feeds a failure into the same
  banner rather than dropping three return values on the floor.

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
  day one — a bare/default UA gets blocked. Any non-default product token
  works; the version inside it is not part of the rule, so bumping
  `KYPOST_VERSION` does not affect access.
- **Cloudflare rotates the TLS leaf every ~90 days with a NEW KEY, so the
  TLS pin is anchored to the ISSUER, not the leaf.** Measured 2026-08-24:
  the leaf `CN=urlxl.com` was valid 2026-07-20 → 2026-10-18, while its
  issuer (Google Trust Services `WE1`) runs to 2029-02-20; both edge IPs
  served the same key, so this is rotation, not per-PoP variance. A
  leaf-SPKI pin therefore fired the "server is being impersonated" banner
  roughly quarterly with no attacker involved, and the only remedy on
  offer was a "reconnect" button — i.e. it was training users to click
  through the one dialog that exists to stop a real impersonation.
  `HttpClient::pinnedSpkiFromChain()` now pins `chain[1]`, guarded by a
  check that it really did issue `chain[0]` so a reordered chain cannot
  silently anchor to the root. Two things this does NOT do: catch
  mis-issuance by that same CA, and protect the Cloudflare↔origin leg —
  Cloudflare terminates TLS, so the pinned key is Cloudflare's and the
  plaintext is visible to it regardless. Devices paired before the change
  carry a leaf pin, which is still accepted; those get exactly one banner
  at the next rotation, and Reconnect re-anchors them to the issuer.
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
