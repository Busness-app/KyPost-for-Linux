# KyPost — Linux Qt Client Plan (Linux Desktop + KDE Mobile via Flatpak)

> **Renamed 2026-07-25.** This document predates the Llama Mail → KyPost
> rename and still used the old product name, the old `llamalabels://` deep
> link scheme, the old `libllamacore` library name, and old sibling-repo
> paths throughout. Those are corrected inline below. Ubuntu Touch / Click
> is a dropped decision, not a pending target — see AGENTS.md §4.

> **Superseded for parity purposes, 2026-08-22.** Feature parity with
> `kypost-android` is tracked in **`docs/PARITY.md`**, which is the
> acceptance checklist for parity work. This plan is kept as the original
> architectural design record; where it disagrees with `docs/PARITY.md` or
> `AGENTS.md` on what exists today, it is the one that is stale. In
> particular, its description of mail sync (`since` handling) is wrong —
> see AGENTS.md §4's 2026-08-22 correction.

Fourth sibling after Android (`~/git/kypost-android`) and SwiftUI macOS/iOS
(`~/git/kypost-for-Mac`). **This plan supersedes both `Kirigami_llama_mail.md` (2026-07-11 draft,
pre-rename filename) and the short-lived `KDE_Client_Plan.md`.** One Qt codebase, two packages,
two UI roots:

| Target | Toolkit | Packaging | Qt | UI root |
| --- | --- | --- | --- | --- |
| Linux Desktop (KDE Plasma primary) | Kirigami (KF6) | Flatpak (`org.kde.Platform` 6.x) | Qt 6 | Desktop |
| KDE Mobile (Plasma Mobile) | Kirigami (KF6) | same Flatpak | Qt 6 | Mobile |
| Ubuntu Touch (Lomiri) | Kirigami (KF5, bundled in the click) | Clickable → OpenStore | Qt 5.15 | Mobile |

New sibling repo: `~/git/kypost-Linux` (kypost-for-Mac is Xcode-centric; don't nest).
Suggested app IDs: `com.kysecurity.mail` (Flatpak/Flathub), `kypost.urlxl`
(click/OpenStore).

## UI architecture: unified-style app, two roots (like the iOS/macOS app)

Mirror exactly how the Swift app is structured — one codebase, shared
components/pages/theme, and **two explicit root layouts** instead of one
fully-adaptive tree:

- **`MobileRoot.qml`** — used by Plasma Mobile AND Ubuntu Touch, pixel-identical
  on both. Bottom tab bar (Inbox / Compose / Contacts / Settings) + stacked
  page navigation, matching Android and iOS `MainTabView`. Swipe gestures on
  list rows (left = Archive, right = Delete — Android/iOS parity), folder
  picker as a toolbar menu, six-action row in the email detail footer.
- **`DesktopRoot.qml`** — the `MacRootView` equivalent: persistent sidebar
  (Inbox → Inbox subfolders → keywords → Drafts/Junk/Sent/Trash → Archive →
  Archive subfolders) + email list column + detail pane, with a toggle to
  collapse the preview pane (parity with `macShowPreviewPane`). Keyboard
  shortcuts via QML `Shortcut` actions: Ctrl+N compose, Ctrl+R refresh, Del
  delete. Settings opens as a separate dialog with the same four panes as
  `MacPreferencesView` (Connection/Appearance/Keywords/Notifications).
- **Everything below the root is shared**: all pages (EmailDetail, Compose,
  Contacts, Settings panes, Themes, KeywordSettings, Pairing, MfaApproval),
  all components (EmailListRow, KeywordTabBar, Avatar, EmptyState, StatusBadge,
  ThemedButton), one ThemeManager, one navigation router. Pages take no
  platform branches; the root decides how they're arranged.
- **Root selection**: the click build hardcodes Mobile. The Flatpak picks at
  startup — Mobile when Kirigami reports mobile mode (`QT_QUICK_CONTROLS_MOBILE`
  / Plasma Mobile session), Desktop otherwise, with a manual override in
  Settings → Appearance for convertibles/testing.

## What changed since the 2026-07-11 draft plan

1. **UnifiedPush landed** (backend commits `cb96ae0`, `510eba6`, `21a20b8` in
   `~/git/kypost-server`; Android commits `4782ce7`, `8ef0398` in
   `~/git/kypost-android`; doc `kypost-server/UNIFIEDPUSH_IMPLEMENTATION.md`).
   The old plan's "polling-only v1" is gone — push is in v1 where the platform
   allows it (see the push matrix below). The backend explicitly anticipated
   these clients: *"KDE Mobile / Ubuntu Touch clients call the same
   `/api/notifications/native/register` endpoint with `transport=unifiedpush`."*
2. **The backend grew features** (all shipped for the Mac client):
   - All `/api/inbox/actions` verbs are real: `move`, `read`, `archive`,
     `spam`, `delete` (delete moves to Trash; expunges when the mailbox IS Trash).
   - Attachments: `GET /api/mail/attachments`, `GET /api/mail/attachment`
     (messageId = IMAP UID), `attachments:[{name,mimeType,dataBase64}]` on send
     (25 MB decoded cap, 40 MB body limit). Contract in
     `kypost-android/Mobile_Mail_Relay.md`.
   - Send supports `mode: "plain" | "html" | "markup"`.
   - Registration dedupe: server upserts by pushToken+platform; clients send
     their stored `deviceId` on re-registration (otherwise every register call
     appended a duplicate device row).
3. **Known live-system gotchas to inherit, not rediscover:**
   - Re-registration silently 401s once the short-lived `pairingToken` expires —
     token rotation after pairing never reaches the server (latent, unfixed).
     Don't re-register on every launch and assume it worked.
   - `mail.urlxl.com` sits behind Cloudflare (bare `urlxl.com` → 530). Set a
     real User-Agent on QNAM from day one.
   - Deployment lag: the backend deploys via the user's Docker pipeline on a
     remote host. **Verify the UnifiedPush commits are actually deployed to
     `mail.urlxl.com` before any live E2E** — the attachments endpoints had the
     same "committed but 404s live" window.

## Locked decisions carried over (do not relitigate)

- **Relay-only.** No IMAP/SMTP anywhere. `mail.urlxl.com` is the sole
  transport. Search is local-cache-only.
- **Wire contracts come from `kypost-android`** (`Mobile_Mail_Relay.md`,
  `Mobile_Contact_Sync.md`) and, for post-July-2026 features, kypost-for-Mac's
  `Data/Networking/*` Swift clients (relay-only, live-verified, test-locked).
  Never guess shapes — guessed shapes caused live 400s twice. Verified inventory:
  - `GET /api/inbox?sub&hash&limit&mailbox&since` → `{tabs, byTab, cursor, delta, removed}`
  - `GET /api/inbox/folders?parent=INBOX|Archive` → `{parent, folders:[{path, deletable}]}`
    (full paths; `/` and `.` are both hierarchy delimiters)
  - `POST /api/inbox/actions` `{action: move|read|archive|spam|delete, messageIds, mailbox, targetMailbox}`
  - `POST /api/mail/send` — `to`/`cc`/`bcc` are **comma-joined strings**, plus
    `subject`, `body`, `mode`, optional `attachments[]`
  - `GET /api/contacts/sync?since` (pull) / `POST {baseCursor, changes:[ContactDto]}`
    (push); ContactDto: `fn`, `emails[]`/`phones[]` as `{label,value}`, `rev`;
    response `{cursor, tooOld, changed, deleted}`; `tooOld` → reset cursor + wipe cache
  - `POST /api/mfa/push/respond` — auth via `X-Kypost-Device-Id`/
    `X-Kypost-Device-Secret` headers (`RelayAuth`), same as every other
    authenticated endpoint; JSON body is just `{challengeId, approve}`
  - `POST /api/notifications/native/deregister` — auth via the same device
    headers, empty body, `{"ok": true}` on success; lets a device remove
    itself from the account's paired-devices list without a session
- **The theme palettes are a binding contract** with web `theme.ts` / Android
  `AppTheme.kt` / kypost-for-Mac's `Style/AppTheme.swift` (easiest transcription
  source — copy values, don't approximate). Currently **15**;
  `AppTheme::themeNames()` is the source of truth (this said "13" until
  2026-07-25 — two were added later and the count went stale). Fonts: Space
  Grotesk + IBM Plex Mono (OFL; bundle the TTFs).
- **StandardFolder wire names**: `INBOX`, `Drafts`, `Junk`, `Sent`, `Trash`,
  `Archive`; display name splits on both `/` and `.`.
- 90-second foreground refresh cadence. Full-snapshot refresh (`since=0`);
  delta/cursor mail sync stays v2.
- Port the Mac suite's test coverage (159 tests) — it encodes every contract fix.
- License GPL-3.0-only (fine with Qt LGPL / KDE).

## Push: the UnifiedPush contract

### Registration

`POST /api/notifications/native/register` with:

```json
{
  "subscriberId": "...",
  "pairingToken": "...",
  "deviceToken": "https://ntfy.sh/<topic>",
  "deviceId": "<stored id, on re-registration>",
  "platform": "linux",
  "transport": "unifiedpush",
  "deviceName": "<hostname / device model>"
}
```

Response gains `"deviceSecret": "<raw hex>"` on every successful call — the
per-device pairing secret, minted fresh each time and returned only in this
response, never carried in the pairing deep link. Persist it unconditionally
(`DevicePairing::deviceSecret`), overwriting any prior value: every
successful register invalidates the previous secret. Ongoing authenticated
requests (mail, contacts sync, groups, App Pull, PGP QR mint, MFA respond,
deregister) send it back as `X-Kypost-Device-Id`/`X-Kypost-Device-Secret`
headers (`RelayAuth`) — the old account-wide `subscriberHash` scheme was
removed from the backend entirely, no fallback.

- `deviceToken` **is the UnifiedPush endpoint URL**, not an opaque token.
- `transport: "unifiedpush"` is mandatory for us: `normalizeNativeTransport`
  falls back to platform-derived routing when transport is empty, and
  `normalizeNativePlatform` maps any non-ios/android platform (including
  "linux") to **"android" → FCM relay**, which would fail. An unknown
  transport string also silently becomes "fcm" — spell it exactly.
- The server validates the endpoint against SSRF
  (`ValidateUnifiedPushEndpointURL`): **https only, no private/loopback/
  link-local hosts, re-checked at dial time, redirects disabled.** Consequence:
  a self-hosted ntfy on localhost/LAN is **rejected at registration** — all
  push testing needs a public endpoint (hosted ntfy.sh works, no account).
- The register response echoes the stored `transport`; persist what the server
  says, not what we sent.

### Delivery

The backend POSTs the payload **directly** to the endpoint (no Cloudflare relay
in the path). The distributor/subscriber hands the app the raw bytes:

```json
{
  "title": "<sender>",
  "body": "<subject>",
  "data": {
    "messageId": "...", "sender": "...", "subject": "...",
    "senderName": "...", "emailSubject": "...",
    "Keywords": "comma,separated", "title": "...", "body": "...",
    "url": "/read"
  }
}
```

(Envelope from `UnifiedPushSender.Send` in
`backend/internal/processor/native_sender.go`; `data` keys from
`buildNativePushData` in `poller.go`. Note the capital-K `Keywords`.)

Parse the **envelope**, feed `data` into the ported PushPayloadParser
(`messageId` required; `senderName`/`emailSubject`/`Keywords` drive the
notification and dedupe).

> ⚠️ **Android reference bug — do not copy it:** `KyPostUnifiedPushService.onMessage`
> decodes the message as a flat `Map<String,String>`, but the backend sends the
> nested envelope above; the nested `data` object makes the decode throw →
> `getOrNull()` → **every UnifiedPush mail notification is silently dropped**
> on Android as committed. Parse `{title, body, data}` here; fixing Android is
> a separate task in `kypost-android`.

- 404/410 from the endpoint ⇒ backend marks the device **stale and deletes the
  registration** (same trap as APNs BadDeviceToken). On distributor/topic
  change, re-register immediately or pushes stop and the device row vanishes →
  full re-pair required.
- **MFA challenges are NOT sent over UnifiedPush** (public endpoint URLs,
  unencrypted payloads — `dispatchPushChallenge` filters
  `transport == "unifiedpush"` devices). MFA approval works only via the
  polling path while the app runs, on every Linux target. Lifting this needs
  RFC 8291 encryption on the backend; KUnifiedPush already supports
  RFC 8291/8292 client-side, so surface the client's Web Push crypto keys
  behind a `ponytail:` when the backend catches up.

### Per-platform push matrix

| Platform | Delivery mechanism | Background wake | v1 status |
| --- | --- | --- | --- |
| Plasma Desktop | KUnifiedPush distributor (host daemon) via D-Bus | D-Bus activation by distributor | In |
| Plasma Mobile | same | same (power management still maturing upstream) | In |
| Any desktop w/o distributor | 90 s polling of the relay | none — timer only, app must be running | In (fallback) |
| ~~Ubuntu Touch~~ | ~~embedded ntfy subscriber~~ | — | Deferred; tier removed 2026-07-26 |
| Ubuntu Touch v2 | UnifiedPush → lomiri-push-service bridge (community WIP) | lomiri push wake | v2, watch upstream |

**KUnifiedPush path (Flatpak):** link `KUnifiedPush::Connector`
([KDE/kunifiedpush](https://github.com/KDE/kunifiedpush)) — create a Connector,
receive the endpoint URL, register with our backend, connect `messageReceived`.
Bundle the connector library in the Flatpak (small KDE lib, not in the
runtime); the **distributor daemon runs on the host** (not flatpak-able today).
The distributor D-Bus-activates the app on push — the app must be D-Bus
activatable (`DBusActivatable=true`; Flatpak exports the `.service` file) and,
when woken for a push, show a KNotification without raising a window.

**~~Embedded ntfy subscriber~~ — REMOVED 2026-07-26.** This section specified a
middle tier in which the client acted as its own distributor: generate a
≥128-bit random topic, register `https://ntfy.sh/<topic>` as the deviceToken,
and long-poll `https://ntfy.sh/<topic>/json?since=<last>` over QNAM. It was
built (`core/net/NtfySubscriber`, `app/push/NtfyTopicProvisioner`) and then
cut, because both things it was for had expired:

- It was written as "the whole UT v1 story" — Ubuntu Touch has no UnifiedPush
  distributor. **UT is now deferred** (see the Qt5-drop decision in
  `AGENTS.md` §1/§4), so there is no target that needs a client-side
  distributor substitute.
- As a general desktop fallback it was redundant with the polling tier below
  it, which covers the same "no distributor installed" case with **no third
  party involved at all**. Because the subscriber was foreground-only, its
  entire remaining benefit over polling was notification latency while the
  app was already open on screen.

What that latency cost: the relay POSTs `{"title","body"}` — for mail, the
sender and subject — to whatever URL is registered as the deviceToken, so
every notification passed in the clear through ntfy.sh by default, with no UI
to point it elsewhere. The topic doubled as a bearer credential (anyone who
learned it could read the stream), which in turn required rotate-on-re-pair
machinery of its own. Deleting the tier deleted all of that. Distributor-tier
users were never affected either way — that path never used any of it.

**Transport state machine (shared):** distributor present → KUnifiedPush;
else → 90 s polling. The downgrade is surfaced in Settings → Notifications
and through the same syncError channel the pairing UI shows. Even with push,
keep the full PushRepository seq-dedupe/cursor machinery from the Mac port —
pushes and polls race, and dedupe-by-seq is what makes that safe.

## Stack decisions

- **Language split**: C++20 core library (`libkypostcore`) using **only
  QtCore/QtNetwork/QtSql** — this subset compiles identically under Qt 5.15 and
  Qt 6, confining the dual-Qt problem to QML and platform glue. QtDBus/
  KUnifiedPush/KNotifications/lomiri glue lives in the app layer, never in core.
- **QML dual-version strategy**: Qt 5.15 supports unversioned imports, so
  `import org.kde.kirigami as Kirigami` parses on both. Rules: no KF6-only
  types (no `Kirigami.Delegates`), no removed-in-KF6 types (`BasicListItem`) —
  the style guide forces custom row delegates anyway. One small `Compat` QML
  singleton for anything that genuinely diverges. Type registration via
  classic `qmlRegisterType` (works on both; skip `QML_ELEMENT` codegen).
  CMake dual-build via `QT_MAJOR_VERSION` switch + ECM.
- **Persistence**: SQLite via `QSqlDatabase`, hand-written DAOs mirroring
  EmailDAO/ContactDAO/PushNotificationDAO. No ORM.
- **Secrets** (sub/hash/deviceId/pairing credentials), one
  `SecureStore` interface, two backends:
  - Flatpak: QtKeychain → Secret Service portal (bundle QtKeychain).
  - Ubuntu Touch: no Secret Service — AppArmor-confined app data dir, 0600
    perms. `ponytail:` upgrade if UT grows a keyring.
- **HTML mail rendering**: QtWebEngine `WebEngineView` on both. Qt 6 Flatpak:
  QtWebEngine is **not** in `org.kde.Platform` 6 — use the
  `io.qt.qtwebengine.BaseApp` baseapp (verify name/branch on Flathub in
  Phase 1). UT ships QtWebEngine 5.15 (it powers Morph). Reuse the themed CSS
  scaffold from `EmailDetailView.swift` / Android `EmailDetailActivity`,
  palette colors injected as hex; intercept navigation →
  `Qt.openUrlExternally`; plain-text bodies render as mono `Text`.
- **Deep links** (`kypost://native-pair?...` — renamed from `llamalabels://`):
  - Flatpak: `.desktop` with `MimeType=x-scheme-handler/kypost;`,
    `KDBusService(Unique)` routes URLs into the running instance.
  - Ubuntu Touch: `urls` hook in the click manifest (url-dispatcher).
  - Paste-link pairing is the universal fallback; camera QR scan is v2.
- **Notifications**: KNotifications on Plasma (portal-aware in Flatpak) with
  tap-through navigation mirroring PushNotificationDispatcher's `onNavigate`
  (mail → open message, MFA → approval page). UT: Lomiri's notification API is
  push-service-bound, so v1 relies on in-app banners/badges while running.

## v1 scope (relative to the current Mac client)

In: pairing (paste-link + deep-link), inbox with keyword tabs + folder tree +
subfolders, full mail actions (read/archive/junk/delete/move; swipes on
mobile, context menus + shortcuts on desktop), HTML mail viewing, plain-text
compose (reply/reply-all/forward), **attachment viewing + download** (portal
file chooser on Flatpak, ContentHub export on UT), contacts list/detail with
sync + reconciliation, settings (Connection/Appearance/Keywords/Notifications),
15 themes, MFA approval (polling-driven), **push per the matrix above**.

> **Scope status, 2026-07-25.** The v1/v2 split below is the original plan and
> is kept for the reasoning, but it no longer describes the shipped app. Since
> shipped from the "Out (v2)" list: **compose attachments, rich-text/HTML
> compose, server drafts, delta/cursor mail sync**, plus server-side folder
> management, PGP protection-mode awareness, app lock, credential PIN gate,
> TOFU certificate pinning and Hostile Location Protection (none of which
> appear below at all). Still genuinely out: camera QR *pairing*,
> MFA-over-push (backend-gated — see AGENTS.md §4), UT background push,
> drag-and-drop. `MfaApproval.qml` referenced elsewhere in this document was
> deleted on 2026-07-25; see TESTING.md for why.

Out (v2): compose attachments, rich-text/HTML compose, server drafts, camera
QR pairing, delta/cursor mail sync, encrypted push → MFA-over-push
(backend-gated), UT background push (bridge-gated), drag-and-drop,
multi-select, pop-out windows.

## Repo layout

```
kypost-Linux/
  CMakeLists.txt              # ECM + Qt5/Qt6 dual (QT_MAJOR_VERSION switch)
  core/                       # libkypostcore — QtCore/Network/Sql only, compiles Qt5+Qt6
    models/                   # Email, Contact, KeywordSettings, MfaChallenge, PushNotification, StandardFolder
    net/                      # HttpClient (QNAM wrapper, stub-injectable, real UA), RelayMailSource,
                              # ContactSyncClient, NativeRegistrationClient (transport-aware),
                              # MfaResponseClient
    db/                       # Database bootstrap + EmailDao, ContactDao, PushDao
    stores/                   # SecureStore iface + file backend, settings stores (QSettings), cursor stores
    domain/                   # MailRepository, KeywordRepository, ContactSyncRepository+Reconciliation,
                              # PushRepository (seq dedupe/cursor), SendEmail/ApproveMfa use cases,
                              # DeviceRegistrationService, PollingScheduler, TransportStateMachine
    theme/                    # 13 palettes + ThemeManager (QObject → QML)
  app/
    main.cpp                  # DI graph, root selection, deep-link routing, push-activation entry
    push/                     # KUnifiedPush glue (Qt6 only), envelope parser, KNotifications dispatcher
    platform/                 # SecureStore keychain backend (Flatpak), UT specifics, Compat shims
    qml/
      MobileRoot.qml          # bottom tabs + page stack — Plasma Mobile + Ubuntu Touch, identical
      DesktopRoot.qml         # sidebar + list + detail — MacRootView equivalent
      pages/                  # InboxPage, EmailDetailPage, ComposePage, ContactsPage, ContactDetailPage,
                              # SettingsPage, ThemesPage, KeywordSettingsPage, PairingPage, MfaApprovalPage
      components/             # EmailListRow, KeywordTabBar, Avatar, EmptyState, StatusBadge, ThemedButton
  tests/                      # QtTest, stubbed HttpClient — port the Mac suite's coverage
  packaging/
    flatpak/com.kysecurity.mail.yaml (+ metainfo, .desktop, icons)
    click/ (clickable.yaml, manifest.json, apparmor, .desktop, urls hook)
  po/                         # gettext catalogs seeded from Localizable.xcstrings keys
```

## Phases

**Phase 1 — Two skeletons that ship, one of which pushes.**
CMake workspace building `libkypostcore` + a hello-Kirigami window under BOTH
Qt 6 (Flatpak: `org.kde.Platform` 6.x + QtWebEngine baseapp + bundled
KUnifiedPush/QtKeychain) and Qt 5.15 (Clickable `ubuntu-sdk-20.04`, Kirigami/ECM
built via `clickable.yaml` `libraries:`). Prove early, on real targets:
(a) KUnifiedPush Connector gets an ntfy.sh endpoint from inside the Flatpak
sandbox; (b) a manual `curl -d '{"title":"t","body":"b"}' <endpoint>` reaches
the app; (c) **D-Bus activation delivers a push with the app closed**;
(d) `NtfySubscriber` receives the same POST on a UT device/emulator in
foreground; (e) deep links open/focus the app on both (`x-scheme-handler` /
`urls` hook). Also in this phase: check whether UT's Qt 6 framework has landed
(it would erase the dual-Qt burden — check before writing compat code), and
confirm the UnifiedPush commits are deployed to `mail.urlxl.com`.

**Phase 2 — Core data.** Models, SQLite schema + DAOs, settings/cursor stores,
SecureStore (both backends). DAO round-trip tests. Runs under both Qt majors in CI.

**Phase 3 — Relay networking.** HttpClient wrapper, then each client against
the verified contracts, translated from kypost-for-Mac's `Data/Networking/*`.
Test-lock the register body (with `transport`/`deviceId`), send comma-joining,
action verbs, attachments endpoints, and the push envelope (nested `data`,
capital-K `Keywords`) — every locked shape corresponds to a past live failure.

**Phase 4 — Domain + push plumbing.** MailRepository (DAO-cached snapshots),
KeywordRepository tab computation, ContactSyncRepository + reconciliation
(`tooOld` reset+wipe), PushRepository (seq dedupe, cursor; synthesized ms-epoch
seqs for push-mode arrivals, as on Mac), DeviceRegistrationService (registers
endpoint; re-registers on endpoint change; does **not** re-register blindly on
launch given the expired-pairingToken 401), TransportStateMachine
(distributor → polling; the embedded-subscriber tier this phase originally
built was removed 2026-07-26, see the push section above). Port
MailTests/PushTests/ContactSyncTests/NetworkingTests coverage.

**Phase 5 — Theme system + components.** Transcribe the 13 palettes from
`Style/AppTheme.swift` (copy values — binding contract). ThemeManager as a
QML-exposed singleton; STYLE_GUIDE.md components as custom delegates (which is
also what makes the KF5/KF6 delegate divergence irrelevant); bundle the two
font families via QFontDatabase.

**Phase 6 — Screens + both roots.** All shared pages, then `MobileRoot`
(bottom tabs, swipe actions, folder picker menu — verified identical on Plasma
Mobile and UT) and `DesktopRoot` (sidebar with the macOS folder order, list
column, collapsible detail pane, shortcuts, Settings dialog with the four
MacPreferencesView panes). EmailDetail: HTML-sniff → WebEngineView with the
themed scaffold, six-action row, attachment chips → portal save / ContentHub.
Compose (plain text, reply/reply-all/forward). Contacts (read-only header card
→ edit flow). Pairing page (paste link + deep link → register → done). MFA
approval page. Settings → Notifications shows the active transport.

**Phase 7 — Platform integration.** Flatpak: KDBusService single-instance,
KNotifications tap-through, push-activation cold-start path (parse payload →
notify → exit or stay resident), polling pause/resume on
`Qt.application.state`. UT: lifecycle handling (aggressive suspension —
refresh + subscriber reconnect on resume), ContentHub export, in-app MFA/mail
banners. Live E2E against `mail.urlxl.com` on all three targets: pair,
register a real ntfy.sh endpoint, receive a real mail push (distributor path
on Plasma, subscriber path on UT), verify MFA arrives via polling only,
verify a deleted ntfy topic gets the device row cleaned up server-side and
the app detects it and prompts re-pair.

**Phase 8 — Packaging & polish.** App icon (reuse the Dark Matter envelope;
regenerate to hicolor sizes + click icon), AppStream metainfo, gettext
catalogs seeded from `Localizable.xcstrings` keys, Flathub-ready manifest +
finish-args audit (wayland/fallback-x11, `--share=network`, UnifiedPush D-Bus
names, notification/secret/filechooser portals), OpenStore-ready click
(apparmor: networking, content_exchange), TESTING.md checklist mirroring the
Mac one, CI (GitHub Actions: flatpak-builder + `clickable build` in Docker,
tests under both Qt majors).

## Risks / verify-early list

1. **UT framework reality check** (Phase 1): confirm current stable is still
   Qt 5.15/`ubuntu-sdk-20.04` and Kirigami KF5 + ECM build cleanly as Clickable
   libraries. If UT's Qt 6 framework has landed, the dual-Qt burden mostly
   evaporates — check before writing any compat code.
2. **Distributor availability** is the Flatpak push story:
   kunifiedpush-distributor is a host daemon, not flatpak-able today. Confirm
   presence on Plasma Mobile images and desktop distro packages; the embedded
   subscriber + polling fallbacks and honest Settings messaging are
   first-class, not afterthoughts. README states the push tiers explicitly.
3. **D-Bus activation from the sandbox** (Phase 1 item c). If blocked, Flatpak
   degrades to "push while running" — decide explicitly, don't discover it in
   Phase 7.
4. **No UnifiedPush distributor exists on Ubuntu Touch** — the
   UnifiedPush→lomiri-push bridge is community WIP (UBports forum). v1 UT push
   is foreground-only by design; don't promise more in UI copy. Watch the
   bridge for v2 background wake.
5. **Payload envelope mismatch**: the committed Android receiver parses the
   nested envelope as flat and silently drops every UP notification. Until a
   client has received a live UnifiedPush mail notification end-to-end, treat
   the envelope as unverified-live and pin it with tests against the backend
   source, not the Android client. Fixing Android is a separate `kypost-android`
   task.
6. **SSRF validation vs local testing**: private/loopback endpoints are
   rejected at registration (and re-checked at dial time). All push testing
   uses public ntfy.sh topics.
7. **Stale-endpoint deletion**: 404/410 ⇒ server deletes the device row (same
   trap as APNs BadDeviceToken). Re-register immediately on topic/distributor
   change; make re-pair recovery a tested path.
8. ~~**ntfy topic = bearer secret** on the embedded-subscriber path.~~ Moot
   since 2026-07-26 — that path is gone, and no push credential is minted
   client-side any more. The distributor endpoint is still a bearer secret,
   but KUnifiedPush issues and rotates it, not this app.
9. **QtWebEngine availability**: confirm `io.qt.qtwebengine.BaseApp` branch
   matches the chosen `org.kde.Platform` version; confirm WebEngine is
   linkable from a click. Fallback `Text.RichText` is an emergency option only.
   Resolved for the Flatpak target (2026-07-17): a real, published
   `branch/6.10` (and `6.11`) of `io.qt.qtwebengine.BaseApp` exists on
   Flathub — confirmed live via `flatpak remote-info --user flathub
   io.qt.qtwebengine.BaseApp//6.10` — matching `org.kde.Platform//6.10`
   exactly. Earlier notes in this doc claiming it "tops out at branch 6.4"
   were stale/wrong. Consumed via the manifest's documented `base`/
   `base-version` keys (see real published examples on the identical
   runtime/sdk stack, e.g. `flathub/org.kde.cantor`), not a from-source
   Chromium rebuild. Click/Ubuntu Touch WebEngine availability remains
   unverified (deferred along with the rest of the UT target).
10. **KF5/KF6 QML drift**: budget for the Compat singleton; build and run both
    targets every phase, not at the end.
11. **Cloudflare + QNAM**: set a real User-Agent before the first live request.
12. **MFA-over-push is backend-gated** (needs RFC 8291 server-side;
    KUnifiedPush client support exists). MFA stays polling-driven everywhere.
13. **Deployment lag**: no live E2E until the UnifiedPush backend commits are
    deployed on the user's Docker host.

## Post-audit cleanup backlog (2026-07-17)

1. **Fix Flatpak packaging.** ~~`packaging/flatpak/com.kysecurity.mail.yaml`
   currently fails to build (`Could NOT find Qt6WebEngineQuick`, see risk #9)
   and isn't exercised by CI~~ — **Resolved (2026-07-17):** the manifest now
   declares `base: io.qt.qtwebengine.BaseApp` / `base-version: 6.10` (see
   risk #9); no from-source module needed. Also separately found and fixed
   during this pass: the manifest had no `--talk-name` for the UnifiedPush
   distributor's D-Bus bus name (confirmed live: the real distributor here
   owns `org.unifiedpush.Distributor.kde`), so the Distributor push tier
   could never register from inside the sandbox — added
   `--talk-name=org.unifiedpush.Distributor.*`. Remaining: a
   `flatpak-builder` CI job (manual/nightly trigger, not every push/PR given
   real build cost) is still not wired up.
2. **Fix `ContactListModel::SyncedRole`.** ~~The `rev != 0` heuristic
   (`app/contacts/ContactListModel.h`) needs 30+ lines of comments to justify
   itself~~ — **Resolved (2026-07-17):** replaced with
   `ContactSyncRepository::isPending()`/`pendingUids()` querying the real
   pending-change table directly; `ContactsController::isSynced(uid)` exposes
   it to QML, replacing `ContactDetail.qml`'s duplicated formula too.
