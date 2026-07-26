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
docs/       — local tooling/setup notes (see docs/SETUP.md)
```

## 3. Build instructions

A single out-of-tree build directory, Qt6-only:

```sh
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

A change is not verified until this builds cleanly and `ctest` is green.

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
  **Corrected 2026-07-25:** this entry used to read "Full-snapshot refresh
  (`since=0`); delta/cursor mail sync stays v2." Both halves were wrong.
  Delta/cursor sync *is* implemented (`MailRepository::refreshFolder` reads
  `CursorStore::mailCursor()` and handles `isDelta`/`removed`/`cursor`), and
  `since=0` does **not** mean full snapshot — per the wire contract, `since`
  present *at all*, even `0`, puts the endpoint into delta mode. A full
  snapshot requires omitting `since` entirely, which is what
  `forceFullResync` does.
- **License: GPL-2.0** (fine with Qt LGPL / KDE).
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
- **`mail.urlxl.com` sits behind Cloudflare** (bare `urlxl.com` → 530) and
  needs a real, non-default User-Agent set on QNAM for every request from
  day one — a bare/default UA gets blocked.
- **Deployment lag.** The backend deploys via a separate Docker pipeline on
  a remote host; a commit landing in the backend repo does not mean it is
  live yet. Verify the relevant backend commits are actually deployed to
  `mail.urlxl.com` before running any live end-to-end test — this has
  previously produced a "committed but 404s live" window.
- **CI Pipline Failes** Before commiting any change check the CI pipline code.
  EVERY Push to main not ment to fix the CI pipeline has broken the CI pipeline.
