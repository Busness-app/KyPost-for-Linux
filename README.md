<p align="center">
  <img src="kypost.png" alt="KyPost" width="160" height="160">
</p>

<h1 align="center">KyPost</h1>

<p align="center">
  A relay-only email client for KDE Plasma and Plasma Mobile, built with Qt6/Kirigami.
</p>

---

## What is this?

KyPost is the Linux desktop and KDE Mobile client for a relay-based mail service. It does not
use IMAP or SMTP. The KyPost relay backend carries all mail, contact, and push-notification
traffic. One codebase targets two UI surfaces:

- **Linux Desktop** — KDE Plasma, packaged as a Flatpak, with a 3-column sidebar, list, and
  detail layout.
- **KDE Mobile** — Plasma Mobile, the same Flatpak, with a bottom-tab push-navigation layout.

KyPost is a sibling client to an Android app and to a SwiftUI macOS/iOS app. All three clients
use the same Go relay backend.

## Features

- **Inbox, message detail, and an HTML composer** — reply, reply-all, forward, and send, with
  attachments and drafts. A sandboxed `WebEngineView` renders each email body with JavaScript
  and remote images disabled.
- **Server-side folders** — the standard mailboxes and any subfolder. The client can create,
  rename, and delete these folders.
- **Contacts** — synced list and detail views. The client creates and edits contacts offline
  and queues the changes. It also handles group membership and duplicate entries.
- **PGP support** — the client exchanges public keys through QR codes. Scan or show a key with
  the camera, then confirm the fingerprint out of band. The client marks encrypted mail that it
  cannot read and gives a route to webmail. The client never holds a private key.
- **Compose autocomplete** — type a name or an address in Compose, then select a synced
  contact.
- **Push notifications over [UnifiedPush](https://unifiedpush.org/)** — two tiers with
  fallback: the system distributor, then 90-second polling. Mail still arrives when no
  UnifiedPush distributor is installed. Only the push server of your own distributor sees a
  notification.
- **Security** — an optional PIN lock with a lockout and a wipe after repeated failures. You
  can encrypt the pairing credential behind that PIN. The client pins the TLS certificate on
  first use. Hostile Location Protection writes nothing to disk.
- **15 themes** — copied byte for byte from the design system that every sibling client shares.
- **Device pairing** — paste a link, or use a `kypost://` deep link.
- **Localized** — the code wraps every user-facing string for translation (`po/`).

## Installing

KyPost has its own signed Flatpak remote:

```sh
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --if-not-exists kypost https://yoshiofthewire.github.io/KyPost-for-Linux/kypost.flatpakrepo
flatpak install kypost com.urlxl.mail
```

The normal `flatpak update` command then installs new versions. The Flathub remote supplies the
`org.kde.Platform` runtime. KyPost itself is not on Flathub and never will be. Flathub bans
applications that use AI, and the KyPost backend uses AI. Each
[release](https://github.com/Yoshiofthewire/KyPost-for-Linux/releases) also attaches a
single-file `.flatpak` bundle, but a bundle gives you no automatic updates. For more
information, see [`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md).

## Building

KyPost needs Qt6. The project dropped Qt5 and Ubuntu Touch support. [`AGENTS.md`](AGENTS.md)
gives the reason and the conditions for a new review. Use one out-of-tree build directory:

```sh
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

Dependencies, with Arch package names: `qt6-base`, `qt6-declarative`, `qt6-webengine`,
`kirigami` (KF6), `knotifications` (KF6), `kdbusaddons` (KF6), `ki18n` (KF6), `qtkeychain-qt6`,
`kunifiedpush`, `zxing-cpp`. For the Ubuntu and KDE neon archive equivalents, see
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

### Flatpak

```sh
flatpak-builder --user --force-clean --install-deps-from=flathub \
  build-flatpak packaging/flatpak/com.urlxl.mail.yaml
flatpak-builder --run build-flatpak packaging/flatpak/com.urlxl.mail.yaml kypost
```

This manifest is the packaging target for Linux Desktop and for Plasma Mobile. The Click and
Ubuntu Touch directory (`packaging/click/`) is a deliberate empty placeholder. It stays empty
until UBports releases a Qt6/KF6 track.

CI builds this manifest for every pull request. For every push to `main`, CI publishes a signed
OSTree repository to the `gh-pages` branch
([`.github/workflows/flatpak.yml`](.github/workflows/flatpak.yml)). The `flatpak remote-add`
command above uses that published repository.

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

The `core/` boundary permits Qt Core, Network, and Sql only. A new dependency in `core/` breaks
this rule most easily. See `AGENTS.md` Section 5.

[`Linux_QT_Client_Plan.md`](Linux_QT_Client_Plan.md) is the authoritative design source. It
holds the architecture decisions, the wire contracts, the push-transport state machine, and a
current list of known risks and gaps. [`TESTING.md`](TESTING.md) is the manual verification
checklist. `AGENTS.md` summarizes the rules that a change breaks most easily.

## License

GPL-3.0-only — see [`LICENSE.txt`](LICENSE.txt).
