<p align="center">
  <img src="kypost.png" alt="KyPost" width="160" height="160">
</p>

<h1 align="center">KyPost</h1>

<p align="center">
  A relay-only email client for KDE Plasma and Plasma Mobile, built with Qt6/Kirigami.
</p>

---

## What is this?

KyPost is the Linux desktop and KDE Mobile client for a relay-based mail service. It speaks
no IMAP or SMTP — every bit of mail, contacts, and push-notification traffic goes through the
KyPost relay backend. One codebase targets two UI surfaces:

- **Linux Desktop** — KDE Plasma, packaged as a Flatpak, sidebar/list/detail 3-column layout.
- **KDE Mobile** — Plasma Mobile, same Flatpak, bottom-tab push-navigation layout.

It's a sibling client to an Android app and a SwiftUI macOS/iOS app, all talking to the same
Go relay backend.

## Features

- **Inbox, message detail, and an HTML composer** — reply, reply-all, forward, and send with
  attachments and drafts, with email bodies rendered via a sandboxed `WebEngineView` (JS and
  remote images disabled).
- **Server-side folders** — the standard mailboxes plus any subfolders, which can be created,
  renamed and deleted from the client.
- **Contacts** — synced list/detail views, local create/edit with offline queueing, group
  membership, and dedupe.
- **PGP-aware** — key exchange over QR (scan or share a public key via camera, with
  out-of-band fingerprint confirmation), and clear handling of end-to-end encrypted mail this
  client cannot read, with a route to open it in webmail. No private key is ever held here.
- **Compose autocomplete** — type a name or address in Compose and pick from synced contacts.
- **Push notifications over [UnifiedPush](https://unifiedpush.org/)** — a three-tier fallback
  (system distributor → embedded `ntfy` subscriber → 90s polling), so mail arrives promptly
  whether or not a UnifiedPush distributor is installed.
- **Security** — an optional PIN lock with lockout and wipe-on-repeated-failure, an option to
  encrypt the pairing credential behind that PIN, trust-on-first-use TLS certificate pinning,
  and Hostile Location Protection (nothing cached on disk at all).
- **15 themes**, transcribed byte-for-byte from the design system shared across every sibling
  client.
- **Device pairing** via a pasted or `kypost://` deep link.
- **Localized** — every user-facing string is wrapped for translation (`po/`).

## Installing

KyPost is distributed from its own signed Flatpak remote:

```sh
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --if-not-exists kypost https://yoshiofthewire.github.io/KyPost-for-Linux/kypost.flatpakrepo
flatpak install kypost com.urlxl.mail
```

Updates then come through `flatpak update` as normal. The Flathub remote supplies the
`org.kde.Platform` runtime; KyPost itself is not on Flathub and never will be — Flathub bans
applications that use AI, and KyPost's backend does. Single-file `.flatpak` bundles are also
attached to each [release](https://github.com/Yoshiofthewire/KyPost-for-Linux/releases), but
installing one gives you no automatic updates. See [`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md).

## Building

Qt6-only (Qt5/Ubuntu Touch support was dropped; see [`AGENTS.md`](AGENTS.md) for why and when
that's revisited). A single out-of-tree build directory:

```sh
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

Dependencies (Arch package names shown; see [`.github/workflows/ci.yml`](.github/workflows/ci.yml)
for the Ubuntu/KDE-neon-archive equivalents): `qt6-base`, `qt6-declarative`, `qt6-webengine`,
`kirigami` (KF6), `knotifications` (KF6), `kdbusaddons` (KF6), `ki18n` (KF6), `qtkeychain-qt6`,
`kunifiedpush`, `zxing-cpp`.

### Flatpak

```sh
flatpak-builder --user --force-clean --install-deps-from=flathub \
  build-flatpak packaging/flatpak/com.urlxl.mail.yaml
flatpak-builder --run build-flatpak packaging/flatpak/com.urlxl.mail.yaml kypost
```

This is the packaging target for both Linux Desktop and Plasma Mobile. Click/Ubuntu Touch
packaging (`packaging/click/`) is an intentionally empty placeholder until UBports ships a
Qt6/KF6 track.

CI builds this manifest on every PR and publishes a signed OSTree repository to the
`gh-pages` branch on every push to `main`
([`.github/workflows/flatpak.yml`](.github/workflows/flatpak.yml)); that published repo is
what the `flatpak remote-add` above points at.

## Architecture

```
core/       — libkypostcore: models, SQLite DAOs, stores, relay networking, domain
              repositories, theme data. QtCore/QtNetwork/QtSql only — no QtGui/QtQuick/
              QtDBus/KUnifiedPush/KNotifications.
app/        — main.cpp, push/ (KUnifiedPush + KNotifications glue), platform/ (SecureStore
              backends), pgp/, contacts/, mail/, pairing/, qml/ (MobileRoot, DesktopRoot,
              pages, reusable components)
tests/      — QtTest, stubbed HttpClient/FakeRelayServer; ctest-driven
packaging/  — flatpak/ (manifest, desktop file, D-Bus service, AppStream metainfo),
              click/ (deferred)
po/         — gettext translation catalogs
docs/       — local tooling/setup notes
```

The `core/` boundary (Qt Core/Network/Sql only) is the rule most likely to be broken by
accident when adding a dependency there — see `AGENTS.md` Section 5.

[`Linux_QT_Client_Plan.md`](Linux_QT_Client_Plan.md) is the authoritative design source —
architecture decisions, wire contracts, the push-transport state machine, and a running list
of known risks/gaps. [`TESTING.md`](TESTING.md) is the manual verification checklist. `AGENTS.md`
summarizes the rules most likely to be violated by accident when making a change.

## License

GPL-2.0-only — see [`LICENSE.txt`](LICENSE.txt).
