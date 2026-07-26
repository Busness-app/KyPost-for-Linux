# Security hardening: app lock, Hostile Location Protection, and related mitigations

Date: 2026-07-22
Status: Partially implemented (2026-07-25). Features 1 (app lock) and 3
(credential PIN gate) plus TOFU certificate pinning are built and tested;
Feature 2 (Hostile Location Protection) is not started. Inline corrections
below are marked "Correction (implementation, ...)".

## Goal

Port the same three user-facing security settings, in kypost-Linux's own terms:

1. **Require Unlock to Open** — an app PIN gating the app's UI. (No biometric
   convenience unlock on this platform — see "No biometric equivalent"
   below; this is an explicit divergence from Android, not an oversight.)
2. **Hostile Location Protection** — a mode with no on-device cache of mail,
   contacts, groups, or attachments.
3. **Require unlock to receive push/MFA** — off-by-default, gates the device
   credential itself at the cost of background push/MFA while locked.

Plus: window-content protection on sensitive screens where the desktop
environment supports it, and TOFU certificate pinning for the relay
connection. `allowBackup`/`dataExtractionRules` have no Linux equivalent —
see "Additional hardening" for what does.

Same starting point as Android: kypost-Linux has no on-device PGP private
key. `core/net/RelayMailSource.cpp` talks to `/api/mail/...` and receives
mail bodies already decrypted server-side; the client only ever handles PGP
**public** keys (`app/pgp/`) and an identity-status check. Hostile Location
Protection is "don't cache already-decrypted content," not "don't lose key
material," exactly as on Android.

## Current architecture (relevant pieces) — kypost-Linux, verified by reading the code

- **Data layer**: `core/db/Database.h`/`.cpp` opens one `QSqlDatabase`
  (`QSQLITE` driver) per process, under a uniquely-generated connection name
  (`kypost_db_<N>`, via an atomic counter — Qt requires unique connection
  names, this repo already handles that), and runs `core/db/migrations/*.sql`
  idempotently via `PRAGMA user_version`. Every DAO
  (`EmailDao`, `ContactDao`, `GroupDao`, `FolderDao`, `PushDao`,
  `PendingContactChangeDao`, `NativeContactLinkDao`, all in `core/db/`) takes
  a `QSqlDatabase&` in its constructor and holds it as a bound C++ reference
  for its whole lifetime — not a pointer, not reseatable. `app/main.cpp`
  constructs `Database database`, then every DAO against `database.handle()`,
  then every `core/domain` repository against those DAOs, then every
  QML-facing controller against those repositories, then registers each
  controller as a QML singleton via `qmlRegisterSingletonInstance`. This is
  one unbroken reference/pointer chain from `Database` to the QML engine.
  Qt's SQLite driver does support an in-memory database via the special name
  `:memory:` on `QSqlDatabase::setDatabaseName()` — `Database::open(":memory:")`
  works today, unmodified. The whole app is single-threaded (no `QThread`,
  `moveToThread`, or `QtConcurrent` anywhere in `app/` or `core/` — confirmed
  by search); every network call blocks the GUI thread synchronously
  (`core/net/HttpClient.cpp`'s `waitForReply()`), an explicitly accepted
  tradeoff called out repeatedly in the code as "Phase 6 global constraint
  2." So there is no cross-thread `QSqlDatabase` issue to worry about, but
  there is a **harder** live-swap problem than Android's Room: Android's
  ViewModels hold a Room `Database` instance via DI and a process restart
  sidesteps re-wiring; here, swapping the on-disk file for `:memory:` (or
  back) means destroying and reconstructing **the entire composition graph
  in `main()`**, not just one object — C++ references cannot be reseated,
  and `qmlRegisterSingletonInstance` binds a fixed pointer for the QML
  engine's lifetime with no supported "re-point this singleton" API.
  **A live swap is not a workaround away here — it would require
  restructuring the composition root from references to an indirection
  layer (smart pointers behind a provider), which is out of scope for this
  feature. A full process relaunch is the only viable approach, more firmly
  than on Android.**

- **Credential storage**: `core/stores/SecureStore.h` is the
  backend-agnostic interface (`set`/`get`/`remove`/`contains`), with two
  implementations found and read in full:
  - `app/platform/SecureStoreKeychain.{h,cpp}` — backed by the platform
    Secret Service over D-Bus via QtKeychain (`qt6keychain/keychain.h`).
    Each call blocks synchronously on a local `QEventLoop` tied to
    `QKeychain::Job::finished`. Lives in `app/`, not `core/`, by an explicit
    existing repo rule ("core/ must never link" QtDBus-adjacent
    infrastructure). **This is the only implementation `main.cpp` actually
    wires up in production** — `SecureStoreKeychain secureStore("com.urlxl.mail")`
    is constructed there with a comment stating outright: *"SecureStoreFile
    is for tests/UT only."*
  - `core/stores/SecureStoreFile.{h,cpp}` — writes each key as its own file
    under a caller-supplied directory, chmod'd `0600` (owner read/write
    only) immediately after creation and before any content is written.
    **This has no encryption of any kind — it is plaintext protected only
    by Unix file permissions.** Confirmed by reading the full file: no
    `QCryptographicHash`, no crypto library, nothing. This is meaningfully
    **weaker** than both `SecureStoreKeychain` (Secret Service, typically
    encrypted at rest by the desktop keyring) and Android's
    `EncryptedSharedPreferences` (AES-256-GCM, Keystore-backed). It is not
    wired as an automatic runtime fallback anywhere — there is no code path
    today that falls back to `SecureStoreFile` if Secret Service is
    unavailable (e.g. headless Linux, minimal window managers with no
    keyring daemon running); a `SecureStoreKeychain` call would simply fail
    (`QKeychain::Job::error() != NoError`) in that environment. **This
    matters directly for feature 3**: if this repo ever does add a
    file-based fallback for headless environments, that fallback would need
    its own encryption before a credential-PIN-gate wrapping it means
    anything — wrapping an OS-encrypted secret with a second PIN-derived
    layer adds real defense-in-depth; wrapping an already-unencrypted file
    with a PIN-derived layer is the *only* encryption on that path, which
    changes the value proposition. Flagged for whoever owns headless-Linux
    support, not addressed further here (out of scope, no such fallback
    exists to design against yet).

- **Pairing/credential model**: `core/domain/PairingStore.{h,cpp}` is
  structurally the same per-device-secret scheme as Android's
  `SecurePairingStore` — one authoritative store, backed by `SecureStore&`,
  holding seven keys: `sub`, `pairing.serverBaseUrl`,
  `pairing.registrationUrl`, `pairing.pairingToken`, `deviceId`,
  `pairing.deviceName`, and **`pairing.deviceSecret`** — the credential.
  `core/net/RelayAuth.h` sends `deviceId`/`deviceSecret` as
  `X-Kypost-Device-Id`/`X-Kypost-Device-Secret` headers on every
  authenticated relay request — a raw, always-available bearer credential,
  exactly like Android's `deviceSecret`. `pairing.deviceSecret` is
  feature 3's wrap target on this platform, one-to-one with Android's field
  of the same name.

- **Background delivery**: three-tier `core/domain/TransportStateMachine.h`
  (Distributor → EmbeddedSubscriber → Polling), driven by
  `app/push/UnifiedPushConnector.{h,cpp}` (KUnifiedPush, the system
  UnifiedPush client), `core/net/NtfySubscriber.{h,cpp}` (ntfy.sh JSON
  stream), and `core/domain/PushRepository.cpp`'s `pullOnce()` (relay
  polling fallback). `app/pairing/MfaController.{h,cpp}` handles MFA
  challenge responses, reading `sub`/`hash`/`deviceId` straight out of
  `PairingStore` on every `respond()` call. The same "must authenticate
  unattended" tension Android's design describes applies directly: every
  one of these paths reads `pairing.deviceSecret` via `PairingStore` with no
  user necessarily present.

  **One real architectural difference from Android worth calling out**:
  there is no separate lightweight background service process here. This
  app registers `packaging/dbus/com.urlxl.mail.service`
  (`~/.local/share/dbus-1/services/com.urlxl.mail.service` once installed)
  for on-demand D-Bus activation — `KDBusService(KDBusService::Unique)` in
  `main.cpp` claims the well-known bus name, and a UnifiedPush message
  arriving while the app isn't running can cause the **entire process**
  (window, `QQmlApplicationEngine`, `Database`, everything) to cold-start
  just to process one push message, then display a `KNotification`
  (`app/push/NotificationDispatcher.cpp`). Android's push service is a
  narrow, separate component; here, "receive a push while backgrounded" and
  "launch the whole app" are the same event. This has a direct consequence
  for feature 3's UX, spelled out in that section below.

- **Attachment download path**: confirmed unconditional, same pre-existing
  gap as Android. `core/net/RelayMailSource::downloadAttachment()` fetches
  raw bytes over `HttpClient`; `MailController::downloadAttachment()`
  (`app/mail/MailController.cpp`, ~line 322) always resolves
  `QStandardPaths::DownloadLocation`, sanitizes the attacker-influenced
  filename via `QFileInfo(name).fileName()` (already strips path-traversal
  components — a real, pre-existing protection, unrelated to this feature),
  and writes the bytes to disk unconditionally. `app/qml/pages/EmailDetail.qml`
  surfaces this with a literal `"Saved to Downloads"` status line. No
  setting today changes this behavior.

- **Settings screen conventions**: `app/qml/pages/Settings.qml` is a single
  `Item` with a `PillTab`-row pane selector (`Connection` / `Appearance` /
  `Keywords` / `Contacts` / `Notifications` / `General`) driving a
  `StackLayout`. Boolean toggles use paired `PillTab { selected: ... }`
  controls bound to a QML-singleton controller's `Q_PROPERTY` +
  `Q_INVOKABLE` setter (see the existing `General.trayIconEnabled` /
  `General.minimizeToTrayOnClose` pattern in `app/general/GeneralController.h`,
  backed by `core/stores/SettingsStore.h`'s `QSettings`-backed plain
  key/value store). `DesktopRoot.qml` hosts `Settings.qml` in a
  `Kirigami.OverlaySheet`; `MobileRoot.qml` pushes it onto the page stack.
  **A new "Security" pane, seventh in the `PillTab` row, following this
  exact convention, is the natural home for all three toggles** — see the
  concrete recommendation below.

- **App lifecycle hooks**: no Android-style `onStop`/`onStart` exists. What
  the codebase actually has, verified by reading `main.cpp` and
  `DesktopRoot.qml`:
  - `QGuiApplication::applicationStateChanged` (`Qt::ApplicationState`:
    `Active`/`Inactive`/`Hidden`/`Suspended`) — already wired in `main.cpp`
    to `TransportStateMachine::setForegrounded()`. On X11/Wayland desktop
    this fluctuates on ordinary focus loss (alt-tabbing to another app), not
    just on the app being put away — **too noisy to drive an immediate lock
    on Desktop** (it would lock on every window-switch, unusable for a mail
    client someone keeps open alongside other work).
  - Tray minimize: `TrayController::setEnabled()`'s
    `activateRequested` handler calls `m_window->hide()`, and
    `DesktopRoot.qml`'s `onClosing` sets `root.visible = false` when
    `General.trayIconEnabled && General.minimizeToTrayOnClose` — this is a
    genuine "user is done looking at this, it now only lives in the tray"
    signal, much closer to Android's backgrounding than raw focus loss.
  - No screen-lock or suspend hook exists anywhere in the repo today. The
    closest cross-desktop primitives are D-Bus signals: `org.freedesktop.ScreenSaver`'s
    `ActiveChanged(bool)` (session-lock, broadly but not universally
    implemented across desktop environments) and
    `org.freedesktop.login1.Manager`'s `PrepareForSleep(bool)`
    (systemd-logind, suspend/resume, very broadly supported on modern
    Linux). Both would be new `QDBusConnection` subscriptions, but **not a
    new category of dependency** — this app already links QtDBus
    transitively via `KDBusService`, `KStatusNotifierItem`
    (`app/tray/TrayController.cpp`), and `SecureStoreKeychain`'s Secret
    Service calls.
  - No biometric API exists, and none is recommended — see "No biometric
    equivalent" below.

- **TLS/HTTP client setup**: `core/net/HttpClient.{h,cpp}` wraps a single
  injected `QNetworkAccessManager&`, used synchronously by every relay
  client (`RelayMailSource`, `ContactSyncClient`, `MfaResponseClient`,
  `GroupsClient`, `ContactPhotoClient`, `PgpQrClient`, `NativeRegistrationClient`,
  `DeregisterClient`). Confirmed by search: **no `QSslSocket`,
  `sslErrors`, `QSslConfiguration`, `peerCertificate`, or any custom cert
  validation exists anywhere in this repo today.** The app currently relies
  entirely on the OS's default CA trust store; a self-hosted relay with a
  self-signed certificate would already fail to connect. This is the single
  hook point for TOFU pinning — see "Certificate pinning" below.

## No biometric equivalent

Android's design offers PIN entry with an optional biometric convenience
unlock via `BiometricPrompt`, a single unified OS API. **Linux desktop has
no equivalent worth building.** There is no cross-desktop-environment
biometric API comparable to Android's — `fprintd`/PAM fingerprint unlock
exists on some distros with compatible hardware, gated behind polkit, with
no portable Qt-level API and highly inconsistent hardware/DE support.
Building against it would mean a real feature that silently does nothing on
most users' machines, which is the exact failure mode Android's own
`Settings.qml`-equivalent screen (Task-39-era `Settings.qml`) explicitly
avoids elsewhere in this repo (see the "No 'Sync to system contacts' toggle"
comment in `Settings.qml`'s Contacts pane — a toggle that does nothing when
flipped is treated as a defect in this codebase, not acceptable UX).
**Recommendation: PIN-only, full stop, no biometric sub-switch, no
`biometricEnabled` field.** This removes one whole field from the Android
`AppLockStore` shape.

## New module layout

Following the existing `core/` (platform-agnostic) vs. `app/` (Qt-widget /
D-Bus / platform-specific) split this repo already enforces (see
`SecureStoreKeychain`'s own doc comment for the rule):

### `core/security/AppLockStore.h` / `.cpp`
Platform-agnostic PIN policy and lockout state. Takes a `SecureStore&` in
its constructor — **not** `SettingsStore` — for a reason worth stating
explicitly: `SettingsStore` (`core/stores/SettingsStore.h`) is a thin
`QSettings` wrapper over a plain INI file; every field it stores today
(`themeId`, `trayIconEnabled`, keyword visibility, etc.) is
user-editable-if-someone-opens-the-file-in-a-text-editor, which is fine for
those fields but would be a **direct bypass of the whole lock feature** if
`lockEnabled` or `credentialPinGateEnabled` lived there: anyone with OS-level
access to the account's files (the exact access level "Require Unlock to
Open" exists to survive) could flip `lockEnabled=false` in `settings.ini`
and relaunch. Storing these fields via `SecureStore` (i.e.
`SecureStoreKeychain`/Secret Service in production) puts them behind the
same access-control tier as the pairing credential itself, closing that
gap.

- `lockEnabled: bool`
- PIN: `(salt, hash)` where `hash = QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, pinUtf8, salt, 150000, 32)`
  — Qt's own `QPasswordDigestor` (`<QPasswordDigestor>`) provides
  PBKDF2-HMAC-SHA256 natively; same algorithm, iteration count, and output
  size as Android's `PBKDF2WithHmacSHA256(pin, salt, 150_000, 256-bit)`.
  Never the raw PIN.

  **Correction (implementation, 2026-07-25):** this draft said QtCore. It is
  in **QtNetwork** (`Q_NETWORK_EXPORT`, verified against the installed
  headers). Harmless here because `kypostcore` already links `Qt6::Network`,
  but "no external crypto dependency needed" turned out to be true only for
  hashing — the credential gate needs AES-GCM, which Qt does not expose at
  all, so `core/security/CredentialCipher.cpp` links OpenSSL's libcrypto.
  See that file's header comment.
- `failedAttemptCount: int`
- `lockoutUntilEpochMs: qint64`
- `credentialPinGateEnabled: bool`

No `biometricEnabled` field (see above). All five/six fields go through the
same `SecureStore` keys convention `PairingStore` already establishes
(fixed, documented key names — see `SecureStore.h`'s own doc comment
listing the keys it's expected to hold).

`hostileLocationProtectionEnabled` is **deliberately not** part of this
class — same reasoning as Android's "plain, non-secret flag" call: it can
live in `SettingsStore` like every other plain preference, because it is
UI-gated behind `lockEnabled` already being on and protected (see toggle
dependency below) — an attacker who can edit `settings.ini` still can't get
past a protected, `SecureStore`-backed PIN gate to reach anything HLP would
have protected.

### `app/security/AppLockManager.h` / `.cpp`
QObject singleton, mirrors `GeneralController`'s registration shape
(`qmlRegisterSingletonInstance<AppLockManager>("com.urlxl.mail", 1, 0, "AppLock", ...)`).
In-memory only (`bool m_locked`, not persisted) — "locked" means "since
this process started, has the correct PIN been presented," exactly as on
Android.

- On construction: if `AppLockStore::lockEnabled()`, start locked.
- Subscribes to the lock triggers below and sets `locked = true`
  immediately (no grace period) when any fires while `lockEnabled` is on.
- Exposes `Q_PROPERTY(bool locked NOTIFY lockedChanged)` and
  `Q_INVOKABLE bool tryUnlock(const QString& pin)` (verifies against
  `AppLockStore`, applies the lockout policy, clears `locked` +
  `failedAttemptCount`/`lockoutUntilEpochMs` on success).

**Lock triggers wired in `app/security/AppLockManager.cpp`, one class,
mode-aware** (this repo already branches Desktop vs. Mobile behavior at the
`GeneralController::isDesktopMode` boundary — follow that precedent rather
than inventing a second):
- **Desktop mode**: window hidden to tray (`TrayController`'s
  `activateRequested(false)` path / `DesktopRoot.qml`'s `onClosing` when
  minimize-to-tray fires) **and** `org.freedesktop.login1.Manager.PrepareForSleep(true)`
  (suspend) **and**, best-effort, `org.freedesktop.ScreenSaver.ActiveChanged(true)`
  (session lock, where the desktop environment implements it). Deliberately
  **not** `applicationStateChanged` alone — see the noise problem above.
- **Mobile mode** (Plasma Mobile / convergent UI, no tray): `Qt::ApplicationState`
  transitioning away from `Active` — the same signal `TransportStateMachine`
  already listens to, and the correct equivalent here because a mobile
  session genuinely does get suspended/backgrounded the way Android's does.

This is new D-Bus subscription code (`QDBusConnection::sessionBus()`
for both `org.freedesktop.ScreenSaver` and `org.freedesktop.login1`,
system bus for the latter) — lives in `app/security/`, never `core/`, per
the existing `core/`-must-never-link-D-Bus rule.

### `app/qml/pages/Unlock.qml`
Full-screen numeric PIN entry, plain `Item` following the
`EmailDetail.qml`/`Compose.qml` "host decides how to present me" convention
(`signal unlocked()`, no assumption of `pageStack` vs. overlay). Hosted as
an unskippable overlay above `MobileRoot.qml`'s `pageStack` / covering
`DesktopRoot.qml`'s whole 3-column layout whenever `AppLock.locked` is true
— both roots already gate large sections of UI on boolean state (e.g.
`detailMode`), so this is one more `visible: AppLock.locked` binding at the
top of each root, not a structural change.

**Lockout policy** — identical to Android's, ported as-is (no platform
reason to change it): attempts 1–2 plain error/retry; attempt 3 onward
30s/1min/5min/15min/30min-capped delay, computed from the persisted
`lockoutUntilEpochMs` (survives the process actually dying, same as
Android — this is why `AppLockStore` must persist it via `SecureStore`,
not keep it only in `AppLockManager`'s in-memory state); attempt 10 in a
row without an intervening success triggers full wipe-and-reset
(`PairingStore::clear()` + delete the local DB file + reset `AppLockStore`
+ relaunch to first-run/pairing state, reusing the same relaunch helper
Hostile Location Protection needs — see below).

### `app/qml/pages/Settings.qml` — new "Security" pane
Seventh `PillTab`, following the exact existing pattern (pane index 6,
appended to `paneNames`), containing three rows in the same
`PillTab`-toggle idiom `General`'s System Tray section already
establishes:

1. **Require Unlock to Open** (`AppLock.lockEnabled` via
   `AppLockManager`). Turning on prompts a PIN-entry dialog (enter +
   confirm, reusing `Unlock.qml`'s numeric keypad component in a "set PIN"
   mode). "Change PIN" action once enabled. Turning off re-prompts for the
   current PIN first, then clears PIN/lockout state via `AppLockStore`.
2. **Hostile Location Protection** — `enabled: AppLock.lockEnabled`,
   `opacity: enabled ? 1.0 : 0.5` (same disabled-visual convention the
   System Tray section already uses for its second row depending on its
   first). Attempting to interact while disabled shows an inline
   `MutedHint` explanation, not a silent no-op.
3. **Require unlock to receive push/MFA** — same `enabled: AppLock.lockEnabled`
   dependency, off by default. Before enabling, a modal
   (reuse the existing `Popup` pattern `Settings.qml`'s own "Pair This
   Device…" popup already establishes) explains the tradeoff in the same
   plain language as Android's: *"While this is on, new-mail notifications
   and MFA approval requests will only be delivered after you open KyPost
   and unlock it. If your device is compromised while unlocked, this does
   not add protection — it only protects the credential while the app
   itself is locked."*

## Feature 2 in detail: Hostile Location Protection

### Data layer: swap to in-memory
`Database::open(":memory:")` already works, unmodified — Qt's SQLite driver
treats `:memory:` as a private, connection-exclusive in-memory database, and
since this app only ever has one `QSqlDatabase` connection in play at a
time (no threading), there's no cross-connection sharing concern. Every DAO
is unchanged either way, since they only ever see `QSqlDatabase&`, never the
file path.

### The relaunch requirement (stronger here than on Android)
As established above, `main.cpp`'s composition root is a single
reference-chain of stack locals from `Database` down through every DAO,
repository, controller, and QML singleton registration. There is no
supported way to tear down and rebuild any part of that graph without
tearing down and rebuilding **all of it** — no partial "just reopen the DB"
path exists, and none should be built for this feature alone (that would be
a much larger refactor of the composition root, out of scope here). A full
process relaunch is the recommended and only realistic approach:

### Toggling on
1. Persist `hostileLocationProtectionEnabled = true` via `SettingsStore`.
2. Close the current disk-backed `Database` (its destructor already closes
   and removes the QSqlDatabase connection).
3. Delete `kypost.db` and, defensively, `kypost.db-journal`/`kypost.db-wal`/
   `kypost.db-shm` (this repo doesn't currently set an explicit
   `journal_mode` pragma, so don't assume rollback-journal-only — delete
   all four candidate filenames unconditionally, ignoring "doesn't exist"
   errors) from `QStandardPaths::AppDataLocation`.
4. Relaunch (see helper below).
5. On the next launch, `main.cpp` reads the persisted flag before
   constructing `Database` and calls `database.open(":memory:")` instead of
   the real path.

### Toggling off
1. Persist flag = false.
2. Relaunch. `Database::open()` runs against `kypost.db` again — a fresh,
   empty file (the old one was deleted when the flag was turned on, per
   above), migrations run from `user_version = 0`.
3. **This part ports cleanly, unlike feature 3's "no separate catch-up
   needed" claim below**: both `MobileRoot.qml` and `DesktopRoot.qml` already
   call `MailApp.refresh()` (and `DesktopRoot.qml` also `ContactsApp.sync()`)
   from `Component.onCompleted` — i.e. on every fresh launch, unconditionally.
   A relaunch after toggling HLP off **is** a fresh launch, so the existing
   startup-sync behavior repopulates the DB exactly as on any fresh install,
   with no new code needed for this step specifically.

### A genuine relaunch helper needs to be built — it doesn't exist today
Unlike Android (where the Android design assumed a "shared restart-the-app
helper" already existed or was trivial to add), **this repo's only existing
precedent for "the user should restart" is a `MutedHint` in `Settings.qml`'s
General pane**: *"Restart KyPost for interface mode changes to take
effect"* — a manual suggestion the user can simply ignore. Hostile Location
Protection's threat model (wipe-then-relaunch must actually happen, not be
optional) cannot rely on that precedent. **Recommendation: build a real
`app/security/AppRelauncher.h` helper** —
`QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1))`
followed by `qApp->quit()`. One open question to verify empirically before
shipping, not a design blocker: `KDBusService::Unique` claims the
`com.urlxl.mail` well-known bus name for the lifetime of this process:
whether the detached child can cleanly claim that name depends on exact
timing between this process's `quit()` releasing it and the child's
`KDBusService` construction — sequence `qApp->quit()` before spawning the
child if the race proves real; this should be caught by a manual test of
the toggle-on/toggle-off flow before merge, not assumed away here.

### Automatic relaunch applies identically to lockout-triggered wipe
Attempt-10 full wipe-and-reset (from feature 1's lockout policy) reuses this
same `AppRelauncher` helper — one relaunch mechanism serves both cases.

### Attachments
While Hostile Location Protection is on, `MailController::downloadAttachment()`
must not write to `QStandardPaths::DownloadLocation`. Linux has no direct
analogue of Android's disk-free `ContentProvider`-backed pipe (that's an
Android IPC mechanism specifically). The nearest equivalent here: serve the
bytes via a **`QTemporaryFile` opened with `QTemporaryFile::setAutoRemove(true)`
in a `tmpfs`-backed directory** (`QStandardPaths::TempLocation`/`XDG_RUNTIME_DIR`,
which on most Linux distributions is already a `tmpfs` mount, so bytes never
actually reach a spinning-disk or SSD block — a meaningfully different
guarantee than a plain disk-backed temp file, though not byte-for-byte
identical to Android's "never touches disk"), then hand the file's path to
`QDesktopServices::openUrl()` for the OS's normal "open with" flow, and
delete it as soon as the external viewer process exits or, failing a
reliable exit signal, on a short timer. **This is a materially weaker
guarantee than Android's approach and should be documented as such in the
UI copy**, not oversold as equivalently disk-free — recommend the changed
button label read "View (temporary)" rather than Android's plain "View", to
be honest about the caveat. Outside this mode, attachment downloads keep
today's normal "save to Downloads" behavior, unchanged.

## Feature 3 in detail: Require unlock to receive push/MFA

Off by default, its own explicit opt-in with the tradeoff warning above —
same reasoning as Android: this is a materially different tradeoff from
feature 1, not just an extension of it.

### Why this is a real tradeoff here too, with one platform-specific wrinkle
`SecureStoreKeychain` (Secret Service) already protects `pairing.deviceSecret`
against a casual attacker without desktop-session access. But
`TransportStateMachine`'s three tiers, `MfaController::respond()`, and
`DeviceRegistrationService::reregisterIfPaired()` all need to read it
without a user necessarily present at that exact moment — same category of
tension Android describes, and for the same fundamental reason (unattended
operation and secret-at-rest protection are in tension regardless of
platform). The wrinkle: because this app has **no separate background
service** (see "Background delivery" above — a cold push delivery via D-Bus
activation boots the *entire* process), the UX of a locked
credential-PIN-gate during a cold start is more visible here than on
Android. A push arriving while the app isn't running at all will:
1. D-Bus-activate the full process (window, engine, DB — everything).
2. `NotificationDispatcher::notify()` still shows the `KNotification`
   immediately — payload display never depended on the credential (the
   push payload itself carries title/body; only the *subsequent* relay
   calls — polling, MFA respond, re-registration — need `deviceSecret`).
3. `AppLockManager` starts locked (per feature 1's "start locked on
   process init if `lockEnabled`" rule) — the user sees `Unlock.qml`,
   not the mail list, immediately on this cold-started window.
4. Any pull/MFA/registration call attempted before unlock finds the
   wrapped credential unavailable and must no-op gracefully (see below),
   same as Android — but here that no-op is happening inside a window the
   user can already see, not a silent background service, so it should be
   silent to the transport layer's error handling but is inherently
   *visible* as "app opened to a lock screen" rather than invisible the way
   Android's background service failure is. No code change follows from
   this observation, but it's worth stating plainly in this doc so nobody
   is surprised when the credential-gate causes a visible window to pop up
   from a push arrival.

### Implementation
- When `credentialPinGateEnabled` is on, `PairingStore`'s `pairing.deviceSecret`
  value is wrapped with an additional AES-GCM layer (Qt has no built-in
  AES-GCM primitive — this needs a small dependency; recommend `libsodium`'s
  `crypto_aead_aes256gcm_*` if the target CPUs support AES-NI, since it's
  already a common, lightweight, audited dependency, rather than pulling in
  all of OpenSSL/Botan for one primitive; flagged as an open
  implementation choice, not resolved further in this design doc), keyed by
  the same PBKDF2-derived key `AppLockStore`'s PIN hash uses (this setting
  cannot be on without feature 1 also being on, enforced by the same UI
  dependency as Android's).
- On successful unlock (`AppLockManager::tryUnlock()` succeeding), derive
  this key once and hold it in memory only as long as `AppLock.locked` is
  false — same lifetime as the lock state itself.
- The instant `AppLockManager` transitions to locked (any of the triggers
  above firing), drop the derived key from memory. Any
  `PushRepository`/`MfaController`/`DeviceRegistrationService` call made
  while locked finds `pairing.deviceSecret` undecryptable and must fail
  gracefully — return the existing "not paired" / auth-failure path each
  already has (`requirePairing()`-style early return in `MailController`,
  mirrored in the push/MFA callers), not a crash or a new error state.
- **Correction to the Android doc's "no separate catch-up mechanism is
  needed" claim — does not port as-is.** Android's design relies on "the
  existing foreground sync already runs on every foreground transition."
  **This repo has no such thing.** `MobileRoot.qml`/`DesktopRoot.qml` call
  `MailApp.refresh()`/`ContactsApp.sync()` only from `Component.onCompleted`
  (process launch) and on explicit user action (pull-to-refresh, a
  refresh button, re-tapping the active tab) — confirmed by search, no
  `applicationStateChanged`-driven resync exists anywhere. **A new hook is
  required**: connect `AppLockManager::lockedChanged` to a resync call
  (`MailApp.refresh()` + `ContactsApp.sync()` + a `PushRepository::pullOnce()`
  call) whenever it transitions to `false` (i.e. on every successful
  unlock), not just relying on existing behavior. Small, but a real,
  necessary addition this design must call out rather than silently assume
  away.
- When `credentialPinGateEnabled` is off (default), `PairingStore` behaves
  exactly as it does today — Secret-Service-only, always available,
  independent of lock state.

## Additional hardening included this round

### Window-content protection on sensitive screens
Android's `FLAG_SECURE` has no Linux equivalent — confirmed no comparable
Qt or X11/Wayland-portable API exists. The closest available primitive is
compositor-specific: KWin (Plasma) supports marking a window to be excluded
from screenshots/screen-sharing via its own protocols (e.g. the
`org.kde.kwin.ScreenShotClientProtocol` deny-list, or on Wayland generally
the `wp_content_protection`/PipeWire-screencast opt-out some compositors
respect), and there is no single portable Qt API for it — it would mean
compositor-specific code, likely KWin/Plasma-only given this app already
depends on KF6 (`KDBusService`, `KStatusNotifierItem`, `KNotifications`,
`KLocalizedString`), functioning as a no-op elsewhere. **Recommendation:
treat as best-effort, scoped only to `Unlock.qml` (the PIN entry screen
itself — this is the one screen where a shoulder-surfed screenshot has an
unusually high payoff, unlike Android's broader FLAG_SECURE screen list)
via KWin's window-property mechanism where available, and explicitly do
**not** attempt this for the inbox/email-detail/compose/PGP screens this
round** — the effort-to-coverage ratio is much worse on Linux than
Android's single-API `FLAG_SECURE`, and a half-covered "secure screens" set
across a handful of DEs risks being worse than clearly documented
no-coverage. Flagged for a follow-up round if the Plasma-only scope proves
insufficient.

### Backup/data-extraction exclusion
Android's `allowBackup="false"` + `dataExtractionRules` has no direct Linux
package-manager equivalent (there's no OS-level "exclude this app from
device backup" concept the way Android's Auto Backup / `adb backup` work).
What Linux desktop distribution mechanisms **do** offer, worth adopting
instead: if/when this app ships as a Flatpak (`build-flatpak/` already
exists in this tree), Flatpak's own portal-mediated storage sandboxing
already confines the app's writable data to its per-app directory rather
than exposing it network-wide, and common backup tools that respect
`~/.config/backup-exclude`-style conventions (or `CACHEDIR.TAG` for
cache-only directories) can be pointed at
`QStandardPaths::AppDataLocation`/`AppConfigLocation` if a specific backup
tool's exclusion convention is known. This is a much softer, more
tool-dependent mitigation than Android's OS-enforced flag — **recommend
documenting the exclusion paths for `kypost.db` and the Secret-Service
keyring entry in end-user-facing backup guidance**, rather than trying to
build OS-enforced backup exclusion into the app itself, since Linux has no
single mechanism to enforce it against.

### Certificate pinning (TOFU, not a hardcoded pin)
Same rationale as Android — kypost is self-hosted with a per-user server
URL, no single fixed certificate to hardcode, so TOFU is the right shape
here too:
- Hook point: `core/net/HttpClient.cpp`'s `waitForReply()` is the one place
  every relay call already funnels through. Add an
  `sslErrors(QNetworkReply*, const QList<QSslError>&)` handler on the
  shared `QNetworkAccessManager` (constructed once in `main.cpp`, injected
  into `HttpClient`) — this both lets a self-signed/CA-untrusted cert
  through when (and only when) its SPKI hash matches the pinned value, and
  is where non-pinned rejections should hard-fail instead of falling back
  to Qt's default behavior (which today is simply "fail the connection,"
  since nothing calls `ignoreSslErrors()` anywhere in this repo currently
  — confirmed by search).
- At the moment pairing succeeds (`DeviceRegistrationService`'s pairing
  call, in `core/domain/DeviceRegistrationService.cpp`), capture the leaf
  certificate via `reply->sslConfiguration().peerCertificate()`, compute its
  SPKI SHA-256 hash (`QSslCertificate::publicKey().toDer()` through
  `QCryptographicHash::Sha256`), and store it via `PairingStore` alongside
  the rest of the pairing data (an eighth `SecureStore` key, e.g.
  `pairing.pinnedCertSpkiSha256`).
- All subsequent relay requests (every `HttpClient` call) validate the
  presented leaf's SPKI hash against the stored one via the `sslErrors`
  hook above, hard-failing (not silently ignoring) on mismatch.
- Recovery path: a certificate-pin mismatch should surface as a distinct
  `NetworkError` value (`core/net/NetworkError.h` already has a small,
  closed enum of error categories — add one, e.g. `CertificatePinMismatch`,
  rather than folding it into `Transport`), with the UI offering "Clear
  pairing and re-pair" (reusing `PairingController::removePairing()`,
  already exposed to QML in `Settings.qml`'s Connection pane) — needed for
  legitimate cert rotation on the user's own server, exactly as on Android.
- Same threat-model scoping as Android: protects against MITM **after**
  initial pairing; does not and cannot protect the pairing handshake
  itself, which already has to trust whatever server URL the user typed
  in.

## Explicitly out of scope this round

Same list as Android, ported unchanged, plus one Linux-specific addition:

- Duress/panic PIN.
- Root/tamper detection (no meaningful Linux-desktop equivalent exists
  anyway — "tamper detection" on a general-purpose Linux desktop the user
  administers themselves is a different and much fuzzier problem than
  Android's rooted-device detection).
- Clipboard-sensitive flagging for copied fingerprints/pairing codes.
- Secure/overwrite deletion of the old disk-backed `kypost.db` file (plain
  `QFile::remove()` is used — recoverable via forensic disk recovery in
  principle; flagged, not fixed, same as Android).
- Any change to non-Hostile-Location-Protection attachment behavior
  (default "save to Downloads" stays as-is).
- Server-side work implied by the credential-tradeoff discussion (rotating/
  scoped push tokens) — noted as the more fundamental fix, separate repo.
- **Linux-specific addition**: building a `SecureStoreFile`-based
  encrypted fallback for headless/keyring-less environments. Not attempted
  here — flagged above as a prerequisite if that fallback is ever added,
  since feature 3's credential-gate is far less meaningful layered on top
  of `SecureStoreFile`'s current unencrypted-plaintext-plus-permissions
  design than on top of `SecureStoreKeychain`.
- **Linux-specific addition**: window-content protection for any screen
  beyond `Unlock.qml` (see "Window-content protection" above) — scoped down
  from Android's full screen list deliberately, not an oversight.
