# In-app update check (E1), and the disposition of the two reported bugs

Date: 2026-08-25
Status: Designed, not started.

Supersedes parts of
`kypost-server/docs/superpowers/handoffs/2026-08-25-linux-e1-e2.md`. That
handoff is right about E1 and E2 and right about Bug 2. **Its Bug 1 does not
exist** — see "Bug 1" below. Every claim here was verified against the Linux
tree at `e1a1a9c` and the server tree as checked out on 2026-08-25.

## Goal

A Linux user who installs KyPost at launch must find out when a newer release
exists. Flathub is out (it bans AI software), so GitHub releases are the only
channel, and GitHub releases do not auto-update. Without this, an install
stays on its original build indefinitely and is never told otherwise.

**Scope is a notice, not an updater.** A Flatpak cannot update itself, and
nothing here downloads anything or touches the host.

## Verified starting point

| Item | State | Evidence |
| --- | --- | --- |
| E1 — update check | Absent. Zero hits for any update-check symbol in `core/` or `app/`. | grep for `api.github.com`, `releases/latest`, `IsNewer`, `UpdateCheck` |
| E2 — both Flatpak arches | Done. | `.github/workflows/flatpak.yml:96-101` |
| Bug 1 — enrollment grouping | **Does not exist.** | `core/pgp/DeviceEnrollmentCrypto.cpp:156-162`, `app/pgp/PgpEnrollmentController.cpp:107` |
| Bug 2 — `pin=` not honoured | Real. TOFU instead. | no `pin` reference anywhere in `app/` or `core/` |
| Version reaches C++ | Yes, as `KYPOST_VERSION`. | `core/CMakeLists.txt:273` (PUBLIC on `kypostcore`) |
| Version reaches QML | **No.** Nothing exposes it. | grep for `KYPOST_VERSION`/`applicationVersion` under `app/` |
| Settings has an About section | **No.** | `app/qml/pages/Settings.qml` |

The last two are why E1 is more than "port three functions": the notice has no
surface to appear on yet.

## Decision: the client asks its own server, not GitHub

The handoff assumed a straight port of the server's pattern, which would have
the client poll `api.github.com` directly. Rejected.

`AGENTS.md:200-209` records that the embedded ntfy subscriber was removed
specifically to stop routing user data through a third party, ending "Don't
reintroduce it." A recurring GitHub poll is a smaller leak than that one — an
IP address and a `KyPost/0.2.0 (Linux)` User-Agent on a schedule, no mail data
— but it is the same category, and it would put every user's install on a
timetable observable by a party they did not choose.

It also fights the transport. `HttpClient` enforces a per-origin certificate
pin and refuses cross-host redirects by default (`AGENTS.md:515-527`). A call
to `api.github.com` is deliberately cross-origin — the same shape as the PGP
QR fetch that once raised a spurious "your mail server is being impersonated"
banner — so it would need an explicit exception to the pin path.

**Instead the server, which already runs this check hourly for itself,
publishes the answer, and the client asks the one host it is already paired
to.** No new egress destination, and the pin applies to the update check
exactly as it does to every other call.

The cost is that E1 now spans two repos and the client is useless before
pairing. The second is not a real loss: an unpaired KyPost client is inert.

## Server side (`kypost-server`)

New file `backend/internal/api/client_version.go`, mirroring `server_version.go`.

- `linuxClientReleasesURL` — the LIST endpoint
  `https://api.github.com/repos/Busness-app/KyPost-for-Linux/releases`. A
  `var`, not a `const`, only so tests can point it at an `httptest` server —
  the reason `serverReleasesURL` is one. The slug was read from the Linux
  repo's `git remote`, not assumed.
- `linuxClientReleaseMinAge` — **6 hours**, where `serverReleaseMinAge` is 0.
  A server release is installable the moment it is published, so it has
  nothing to wait for. A Linux tag is only useful once `flatpak.yml` has
  finished building *both* bundles, and the `aarch64` build has already died
  twice on Flathub CDN faults (`flatpak.yml:172-180`). The soak window is what
  keeps us from advertising a release whose `aarch64` bundle does not exist.
- `checkForLinuxClientUpdate(ctx)` — calls `ghrelease.Latest` and caches the
  result in a `linuxClientStatus` struct behind a mutex, the same shape as
  `serverVersionStatus`. Called from `StartVersionMonitor`
  (`ollama_version.go:62-74`) alongside `checkForServerUpdate`, so it inherits
  the existing hourly tick rather than adding a second timer.
- **No admin email.** `checkForServerUpdate` mails the admin because the admin
  is the person who applies a server upgrade. A Linux client upgrade is
  applied by the person sitting at the Linux machine, who is generally not the
  admin. Mailing the admin about it would be noise they cannot act on.
- `handleClientVersion` — serves the cache, never performs network I/O, and is
  registered in `server.go`'s route table marked `withDeviceAuth`. Marking is
  not optional: `TestEveryRouteDeclaresItsAuthModel` fails an unmarked route
  (`route_auth_markers.go:1-17`). It resolves the caller through
  `deviceAuthFromRequest`, so this adds no anonymous surface.

Response body:

```json
{ "latestVersion": "0.3.0", "checkedAt": "2026-08-25T12:00:00Z", "error": "" }
```

**It deliberately does not return `upgradeAvailable`.** The server does not
own the client's installed version. Keeping the comparison on the client keeps
the compiled-in constant as the *left-hand side*, which is the property
`server_version.go:12-28` exists to protect: a build tagged 1.0.0 whose
constant still said 0.1.0 would compare itself against every release, conclude
it was permanently out of date, and nag forever.

`ghrelease` itself needs no change. Its package comment already describes it
as deliberately generic across callers; this is the third.

## Client side (`kypost-Linux`)

**`core/version/VersionCompare.{h,cpp}`** — a port of `IsNewer` and
`parseVersion`. Pure, no network, ~25 lines. Two requirements the server's
version handles that are easy to lose in a port:

- strip a leading `v` — the Linux tags are `v0.2.0`, while `KYPOST_VERSION` is
  `0.2.0`, and both sides must reach the same form;
- refuse anything that is not `N.N.N` rather than parsing it best-effort. This
  is not hypothetical: `v0.1-alpha` is in this repo's tag list today.

**`core/net/ClientVersionClient.{h,cpp}`** — `GET api/client-version` through
the existing `HttpClient` with `auth.headerItems()`
(`core/net/RelayAuth.h:22-23`), shaped like `ContactSyncClient::pull`
(`core/net/ContactSyncClient.cpp:210-216`). Same origin as every other call,
so the certificate pin and the same-origin redirect refusal both apply with no
special case.

**`app/update/UpdateCheckController.{h,cpp}`** — a QObject exposed to QML,
shaped like `PgpEnrollmentController`. Properties: `installedVersion` (from
`KYPOST_VERSION`), `latestVersion`, `updateAvailable`, `checkedAt`,
`releaseUrl`. Runs once at startup and on an hourly `QTimer`, only while
paired.

- Failures log and drop. This runs unattended, an unreachable self-hosted
  server is routine, and the next tick retries — the same reasoning
  `checkForServerUpdate` gives for not surfacing its own failures.
- **A 404 is "no information", not an error.** Servers still on 0.3.0 have no
  such endpoint. The About section must read as "unknown", not as a failure,
  or every user on an older server sees a permanent error.

No CMake change is needed: `KYPOST_VERSION` is already `PUBLIC` on
`kypostcore` (`core/CMakeLists.txt:273`), so `app/` links it.

### Where the notice appears

A new **About** section in `Settings.qml`: installed version, latest version,
when it was last checked, and a link to the release page. Plus a `Toast`
(`app/qml/components/Toast.qml`) at startup whenever an update is pending —
each launch, not once per version. A missed one-shot toast otherwise means the
user is never told again, which is the failure E1 exists to prevent.

**Explicitly not `StatusBanner`.** That component is danger-styled and
non-dismissible by design — "Nothing here can be turned off outright — these
are the conditions the user most needs to know about"
(`app/qml/components/StatusBanner.qml:32-33`) — and is reserved for conditions
that stop the app working: a certificate mismatch, a rejected re-registration,
an unencrypted database. Painting a routine "0.3.0 is out" in the same red
strip, unclearable, would devalue the four warnings that genuinely matter.

## Testing

**Server.** A table test on `checkForLinuxClientUpdate` against an `httptest`
server — which is what makes `linuxClientReleasesURL` a `var` — covering: a
newer release, an equal one, one still inside the soak window, a draft, a
prerelease, and an empty release list. Plus the route-auth marker test, which
fails automatically if the route is registered unmarked. Both patterns already
exist in `server_version_test.go`.

**Client.** `VersionCompare` units: `v` prefix stripped, `v0.1-alpha` refused,
equal, older, newer, malformed, and a missing component. `ClientVersionClient`
against a stub `HttpClient`, including the 404-is-not-an-error path. A QML test
for the About section. `tests/` already splits `core`, `app`, and `qml`.

## Release ordering

E1 cannot land as one PR. **The server endpoint must be released before the
client that reads it**, or every field server answers 404. The client's
404-as-unknown handling is what makes the intervening window harmless, so it
is a requirement rather than defensive polish.

The left-hand-side risk the server documents is already covered on this side:
`scripts/verify-version.sh` fails CI when `project(KyPost VERSION ...)`
disagrees with the tag or the metainfo release entries.

## Bug 1 — reported, and not real. No code change.

The handoff reports that Linux displays the enrollment code ungrouped, in
deviation from `PLATFORM_BASELINE.md` §2's normative `4-3-4-3`.

It does not. `formatEnrollmentCode`
(`core/pgp/DeviceEnrollmentCrypto.cpp:156-162`) performs exactly that split:

```cpp
return code.left(4) + QLatin1Char('-') + code.mid(4, 3) + QLatin1Char('-')
    + code.mid(7, 4) + QLatin1Char('-') + code.right(3);
```

and `app/pgp/PgpEnrollmentController.cpp:107` applies it before the value ever
reaches the `verificationCode` property. `Settings.qml:836` renders an
already-grouped string. The handoff grepped QML, saw a raw property binding,
and stopped one hop short of the controller. `git log -S` places the helper in
`e92b16b`, the original enrollment commit, so this was never a regression.

Its two flagged unknowns also resolve:

- **Bucket size is 120 s** — `QDateTime::currentSecsSinceEpoch() / 120`
  (`PgpEnrollmentController.cpp:108`).
- **Crockford input normalisation does not apply to Linux.** This client only
  *displays* the code; there is no entry field for one. Normalising typed
  input is a webmail-side concern.

The work here is to correct `PLATFORM_BASELINE.md` so nobody "fixes" working
code in a security ceremony.

## Bug 2 — real, filed, not fixed for 0.2.0

Filed as [Busness-app/KyPost-for-Linux#50](https://github.com/Busness-app/KyPost-for-Linux/issues/50),
targeted at 0.4.0.

Linux never reads `pin=` from the pairing URI. It parses with `QUrlQuery`,
requires only `sub`/`srv`/`pt`, reads `reg` optionally, and ignores unknown
parameters (`app/pairing/PairingController.cpp:130-148` — note the path is
`app/pairing/`, not `core/pairing/` as the handoff cites). So **pairing is not
broken by the server publishing a pin**, which was the launch plan's open
question.

What Linux does instead is trust-on-first-use: it captures the SPKI of
whatever certificate served registration
(`core/net/NativeRegistrationClient.h:62-70`) and pins later calls to it. The
gap is that the pairing request carries the pairing token, the push endpoint
and the push credentials together, and TOFU trusts the certificate only
*after* those are disclosed. On a network with a locally trusted CA an
interceptor reads the token and registers its own device first.

Per `PLATFORM_BASELINE.md` §1 a client that reads `pin` must fail closed on
mismatch. Linux cannot fail closed on something it never reads.

**Not fixed for 0.2.0.** It is the status quo, TOFU is the documented
fallback, and honouring the pin means adding a fail-closed path to
registration — the one flow whose failure mode is "nobody can pair at all."
That is not a change to make days before launch. Track it for 0.4.0.

## `PLATFORM_BASELINE.md` corrections (server repo, tracked)

The matrix marks every Linux row `unverified` because its author had no
checkout. These are now settled:

| Contract | Linux | Evidence |
| --- | --- | --- |
| Pairing URI, unknown-param tolerance | ✅ | `app/pairing/PairingController.cpp:130-148` |
| `pin=` honoured | ❌ TOFU instead | `NativeRegistrationClient.h:62-70`, `CertificatePinSink.cpp` |
| Enrollment code: 14 chars / Crockford / 65-byte key | ✅ | `DeviceEnrollmentCrypto.cpp:17,127,144-158` |
| Enrollment display grouping | ✅ **(handoff says ❌ — it is wrong)** | `DeviceEnrollmentCrypto.cpp:156-162`, `PgpEnrollmentController.cpp:107` |
| Enrollment bucket size 120 s | ✅ | `PgpEnrollmentController.cpp:108` |
| `transport` sent explicitly | ✅ `"unifiedpush"` | `core/net/NativeRegistrationClient.cpp:43` |
| Device credential headers | ✅ | `core/net/RelayAuth.h:22-23` |
| Contact sync | ✅ | `core/net/ContactSyncClient.cpp:215` |
| Delivery modes | ✅ `push`/`pull` | `app/pairing/PairingController.h:129-130` |

Still unverified: the contact-sync 500-change batching limit. Only the pull
path was read.

## Order of work

1. **`PLATFORM_BASELINE.md` corrections.** Minutes, and it stops someone
   "fixing" correct code.
2. **Server endpoint + tests.** Must ship before step 3.
3. **Client: `VersionCompare`, `ClientVersionClient`, `UpdateCheckController`,
   Settings About section, startup toast.**
4. **File Bug 2** as a tracked issue against 0.4.0.

## Out of scope

Downloading or applying an update; any change to the pairing or registration
path; Flathub; anything that makes the client talk to a host it is not paired
to.
