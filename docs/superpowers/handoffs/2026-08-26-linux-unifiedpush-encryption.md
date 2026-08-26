# Handoff: UnifiedPush payload encryption (RFC 8291) — Linux client

Written 2026-08-26 from the **Android** repo, having read the Linux checkout at
`/home/yoshi/busness.app/kypost-Linux` (at `fab1ace`), the server checkout, and
KUnifiedPush's own source at the tag this project pins. Every claim carries a
citation and was verified against the file it names.

> **Reviewed 2026-08-25 from the Linux repo.** Every citation below was
> re-checked against the real file and none were wrong. The review closed two of
> the open questions at the bottom (both in this plan's favour), corrected the
> plumbing route in §2, and found one hazard this plan had missed — the
> private-key file mode, now the third entry under *Hazards*. Changes from that
> review are marked **[review]**.

**Path convention:** `app/` and `core/` are in `kypost-Linux`. `backend/` is in
`kypost-server`. The contract is `docs/PLATFORM_BASELINE.md` in the server repo,
a tracked file, so also readable at
<https://github.com/Busness-app/KyPost-Server/blob/main/docs/PLATFORM_BASELINE.md>
— §4 gained a "UnifiedPush: the WebPush key exchange is REQUIRED" subsection in
the server PR below.

---

## Headline: the crypto is already written. This is plumbing.

KUnifiedPush generates the keypair, persists it, and decrypts incoming messages
for you. **You are not writing RFC 8291.** The whole job is:

1. Read two byte arrays out of the connector.
2. Base64url them into two new JSON fields on the registration request.
3. Raise the KUnifiedPush version floor so the API is guaranteed present.

Estimate: one small PR, most of it tests.

---

## What changed on the server

**`Busness-app/KyPost-Server` PR #149**, branch `feat/unifiedpush-rfc8291`.
**[review] Merged `2026-08-26T02:45:01Z`** — *"Encrypt UnifiedPush payloads
(RFC 8291), and let push-MFA follow"*, confirmed with `gh pr view 149`. What the
*deployed* server is running is still unchecked.

Until that PR, `UnifiedPushSender.Send` POSTed `{"title","body","data"}` as
plaintext JSON straight to the distributor's broker. Every mail subject and
sender line reaching a Linux user crossed a third party in the clear. The Android
client had been sending `p256dh`/`auth` since July and the server's decoder was
silently discarding them; #149 is the missing half.

After it:

| Condition | Server behaviour |
| --- | --- |
| Device registered **with** `p256dh` + `auth` | `aes128gcm` ciphertext, RFC 8291 |
| Device registered **without** them | today's plaintext POST, unchanged |

Two extra consequences, both relevant to Linux:

- **Push-MFA eligibility now follows the keys — and for Linux that is a trap.**
  `api.MFATransportEligible` used to exclude every `unifiedpush` device outright,
  because an MFA challenge carries the sign-in IP, the user agent and the match
  digits, and those must not cross a public broker in the clear. It now allows a
  `unifiedpush` device **that supplied keys**; a keyless one still receives
  cleartext and stays excluded.

  ⚠️ **CORRECTION, 2026-08-26 (Android repo).** An earlier draft of this
  paragraph called this "the single biggest user-visible win here" and implied
  that sending the keys was all it took. That was wrong. Sending the keys makes
  this client eligible to *be sent* challenges, and it has no code to receive or
  answer one. See the new first entry under *Hazards* before implementing.
- **`Auth` is redacted from API responses.** With the endpoint URL it is a
  notification-forgery capability, not inert metadata.

---

## The three changes

### 1. Get the keys out of the connector — `app/push/UnifiedPushConnector.{h,cpp}`

`KUnifiedPush::Connector` exposes exactly what the server wants
(`/usr/include/KUnifiedPush/kunifiedpush/connector.h:116-149`, both `@since
25.08`):

```cpp
[[nodiscard]] QByteArray contentEncryptionPublicKey() const;  // raw 65 bytes
[[nodiscard]] QByteArray contentEncryptionAuthSecret() const; // raw 16 bytes
```

Both are documented as: *"When no key pair and authentication secret exist yet, a
new one is generated and persisted. When a key pair and authentication secret
exists incoming messages are automatically decrypted."*

That is not merely the doc comment — verified in the source at the pinned tag,
`src/client/connector.cpp:364-373` (tag `v26.04.3`):

```cpp
QByteArray Connector::contentEncryptionPublicKey() const
{
    d->ensureKeys();
    ...
```

and `ensureKeys()` at `connector.cpp:275-282` generates and stores on first call.
**Nothing else in the library calls `ensureKeys()`** — grep confirms the only two
call sites are those two accessors. This is why the client has no keys today: it
has never called either one.

Add two pass-through accessors on `UnifiedPushConnector`, alongside the existing
`endpoint()` / `state()` (`UnifiedPushConnector.h:38-39`), which exist for exactly
this reason — `main.cpp` needs them to build the registration request.

The server wants base64. `QByteArray::toBase64(QByteArray::Base64UrlEncoding |
QByteArray::OmitTrailingEquals)` is the natural choice, but see *"Any base64
works"* below — you have latitude here.

### 2. Send them — `core/net/NativeRegistrationClient.cpp:34-43`

The request body is built as a plain `QJsonObject`:

```cpp
body[QStringLiteral("platform")] = QStringLiteral("linux");
body[QStringLiteral("transport")] = QStringLiteral("unifiedpush");
```

Add `p256dh` and `auth` next to those. Server-side shape:

| Field | Value | Server check |
| --- | --- | --- |
| `p256dh` | uncompressed P-256 point, 65 bytes, base64 | must be a point **on the curve** |
| `auth` | 16-byte secret, base64 | must decode to exactly 16 bytes |

**Both or neither.** A partial pair is a `400`, as is a malformed `p256dh`. Both
absent is fine and means "no encryption", which is today's behaviour.

**Any base64 works.** The server tries the standard alphabet first, then URL,
padding either way — it mirrors `webpush-go`'s own `decodeSubscriptionKey`
exactly, deliberately, so validation can never refuse material the sender would
have accepted. Do not agonise over the encoding.

**Mind the `core/` boundary.** `NativeRegistrationClient` lives in `core/` and
must not learn about KUnifiedPush — that library is `app/`-only by the same rule
that keeps `SecureStoreKeychain` there (`UnifiedPushConnector.h:8-11`). The keys
therefore have to be threaded in as parameters, the same route `deviceToken`
already takes. Both call sites need it:

- `core/domain/DeviceRegistrationService.cpp:123` (`sendRegistration`, the
  re-registration path)
- `core/domain/DeviceRegistrationService.cpp:225` (`pair`)

**[review]** `registerDevice` already takes a `QUrl` and five positional
`QString`s (`NativeRegistrationClient.h:86-88`). Adding two more puts seven
strings in a row, which is past the point where a caller can get them right by
eye. A small
`struct WebPushKeys { QString p256dh; QString auth; };` in `core/net/` is
probably worth it, but that is your call.

**[review] There are two routes above that, not one.** An earlier draft of this
handoff said the keys "can follow the identical path" as `deviceToken`. That is
true for `pair()` only. The re-registration path never touches
`PairingController`:

- **`pair()` — via `PairingController`.** `deviceToken` is late-bound:
  `main.cpp` pushes it in via `PairingController::setDeviceToken()` whenever the
  distributor reports an endpoint (`PairingController.h:27-37,250,354`, wiring at
  `app/main.cpp:1509-1519`). Keys can and should follow this path — they are
  available at the same moment and change on the same events.
- **`sendRegistration()` — direct from `main.cpp`.** `reregisterAndReport`
  (`app/main.cpp:1451`) takes `endpoint` as a lambda **parameter** and hands it
  straight to `DeviceRegistrationService::sendRegistration`. Its capture list is
  `deviceRegistrationService`, `networkExecutor`, `pairingController`,
  `reportReregistration` — **no `pushConnector`**. This path needs its own
  threading: capture `pushConnector` (legal, `main.cpp` is `app/`) or take the
  keys as a second parameter alongside `endpoint`.

Do not forget the deferred-replay half. `deferredReregistrationEndpoint`
(`app/main.cpp:~1497`) stores only the endpoint across a lock, so the keys need
matching treatment or must be re-read from the connector at replay time.

**[review] This is also what makes the upgrade path work for existing users, so
get it right.** A device that is already paired and already holds a persisted
endpoint will never see `endpointChanged` fire again — that signal reports a
*change*. The re-registration that delivers its keys comes from the other
trigger: `TransportStateMachine` starts at `TransportTier::Polling`
(`TransportStateMachine.h:70`), so the distributor coming up on each launch
fires `tierChanged` → `reregisterAndReport(pushConnector.endpoint())`
(`app/main.cpp:1585-1590`). Plumb only the `pair()` route and every existing
user stays keyless — plaintext push forever, push-MFA still excluded — while a
fresh-pair test passes.

### 3. Raise the version floor — `app/CMakeLists.txt:161`

```cmake
find_package(KUnifiedPush REQUIRED)   # no version constraint
```

The content-encryption API is `@since 25.08`. Without a floor, a distro shipping
something older fails at compile time with a missing method and a confusing
error. Add the version.

✅ **[review] The comparison works — checked, not assumed.** KDE
release-service versions are `YY.MM`, so `find_package(KUnifiedPush 25.08 ...)`
leans on CMake comparing `26.08.0 >= 25.08`; whether the exported
`/usr/lib/cmake/KUnifiedPush/KUnifiedPushConfigVersion.cmake` normalises `25.08`
to `25.8` was the open question. Configured both directions against the
installed 26.08.0:

| Requested | Result |
| --- | --- |
| `25.08` | `-- FOUND 26.08.0`, configure succeeds |
| `26.12` | *"The version found is not compatible with the version requested"* |

No normalisation problem, and the floor really does reject. Just add it.

---

## Hazards

### ⚠️ Do NOT send the keys until this client can answer an MFA challenge

Verified 2026-08-26: the Linux client has **no MFA implementation at all**. A
grep for `MfaChallenge`, `showMfaChallenge`, `mfa/approve` and `mfa/respond`
across `core/` and `app/` returns nothing. The only matches anywhere are the
settings string "Require unlock to receive push and MFA"
(`core/domain/PairingStore.h:103,110,150`).

If the keys ship on their own:

1. The client registers with `p256dh` + `auth`.
2. `MFATransportEligible` returns true for it, so `mfaApproverDevices` counts it
   as an approver (`backend/internal/api/push_mfa_handlers.go:64-78`).
3. The user is permitted to enable push approval on the strength of that device.
4. A challenge is dispatched to it, arrives as a JSON payload that
   `PushPayloadParser::parse()` does not recognise, and is dropped.
5. The security prompt silently never appears.

**It is not a lockout.** Push approval cannot be enabled unless TOTP is already
on, so a fallback exists by construction (`push_mfa_handlers.go:86,108`). But the
user is told a device can approve sign-ins when it cannot, which is worse than
that device being excluded.

**The Android client had exactly this bug, from exactly this cause, and it is
fixed** — Android commit `c7c0170`. Its Firebase service checked for a challenge
before treating a push as mail and its UnifiedPush service did not, so the two
drifted. The fix routes both transports through one `IncomingPushRouter` and adds
a source rule that fails the build if a receiving service parses a payload
directly. The Linux equivalent is a branch in the `messageReceived` handler at
`app/main.cpp:1535+`, ahead of `PushPayloadParser::parse()`, plus a UI for the
challenge and an authenticated call to the respond endpoint.

Android also had a second half worth copying: its UnifiedPush service never
created the MFA notification channel, because the mail path self-creates its
channel and the MFA path does not. Check the Linux notification setup for the
same asymmetry rather than assuming it is absent.

**Sequence this deliberately:**

| | Keys first, MFA later | Keys and MFA together |
| --- | --- | --- |
| Confidentiality | fixed immediately | fixed at the same time |
| Push-MFA | advertised, silently broken | works |
| Extra work | none | approval UI + respond call |

If you take the first column, say so where a user can see it, and treat the MFA
path as the next item rather than someday.

---

## Three further hazards, one of which turns out not to exist

### ✅ There is NO rollout-ordering hazard. Linux degrades safely.

The obvious fear: a Linux client that has generated keys, talking to a
self-hosted server that predates PR #149. That server's decoder discards
`p256dh`/`auth`, so it keeps sending plaintext — to a client now holding keys.
Does the client drop it?

**No.** Verified in `src/client/connector.cpp:32-49` — **[review] byte-identical
in both the Flatpak-pinned `26.04.3` and this machine's `26.08.0`**, so the
version divergence below cannot change this answer:

```cpp
if (m_contentEnc.hasKeys()) {
    const auto decrypted = m_contentEnc.decrypt(message);
    if (!decrypted.isEmpty()) {
        Q_EMIT q->messageReceived(decrypted);
        return;
    }
}

Q_EMIT q->messageReceived(message);   // falls through to the RAW bytes
```

Decryption failure falls through and emits the message unchanged.
`PushPayloadParser::parse()` then sees exactly the bytes it sees today. A Linux
client can send the keys to any server, old or new, and lose nothing.

**[review] One upstream wart while you are in this function.** Lines 33 and 40
are `qCDebug(Log) << token << message` and `qCDebug(Log) << token << decrypted` —
the raw payload and the decrypted plaintext. A `QT_LOGGING_RULES` catch-all
(`*.debug=true`) therefore writes mail subjects, sender lines and, after #149,
MFA sign-in IPs and match digits into the journal in the clear. Upstream's
choice, not something this PR introduces, but it is worth knowing that the
confidentiality this work buys ends at that log category.

**Do not carry the Android reasoning across.** The server-side design doc says
"the client drops any message it cannot decrypt (`message.decrypted == false`)".
That is true of the *Android* UnifiedPush connector and false of KUnifiedPush.
The asymmetry is real and it is the reason Linux needs no feature negotiation,
no capability flag, and no staged rollout.

### ⚠️ [review] The private key lands in a world-readable file on first run

This one was missed by the original handoff, and it matters precisely *because*
this PR is a confidentiality fix.

`ensureKeys()` persists through `QSettings` (`connector.cpp:161-172` at
`26.08.0`), and it writes **`ContentEncryptionPrivateKey` in plaintext** into
`$XDG_CONFIG_HOME/kunifiedpush-com.kysecurity.mail` — the same file `main.cpp`
already guards. That guard has a first-run hole:

```cpp
// app/main.cpp:349-350
const bool unifiedPushStatePrivate =
    !QFile::exists(unifiedPushStatePath) || PrivatePath::ensureFile(unifiedPushStatePath);
```

`PrivatePath::ensureFile` returns `false` when the file does not exist
(`core/security/PrivatePath.cpp:42-43`), so on a clean profile the check takes
the `!QFile::exists` short-circuit and hardens **nothing**. `registerClient()`
then runs and QSettings creates the file itself. Measured, not assumed — a
throwaway Qt program writing an Ini-format `QSettings` under `umask 022`:

```
-rw-r--r-- 1 yoshi yoshi 56 /tmp/qsperm/state.ini
```

`0644`. Not hypothetical either: `~/.config/kunifiedpush-com.urlxl.LlamaMail` on
this dev machine is `-rw-r--r--` right now, while the KyPost one is `0600`
because a *later* launch fixed it.

Today that window leaks an endpoint — a push-forgery capability. After this
change it leaks the RFC 8291 private key, so any local user can decrypt every
payload, including the MFA sign-in IP, user agent and match digits that #149
moved onto this channel *on the grounds that it is now encrypted*. The window
runs from first registration until the next app start, and reading the key once
is permanent.

**Fix, and do it in this PR.** Create the file `0600` *before* the connector can
write it, rather than repairing it a launch late. `QSettings` preserves an
existing owner-only mode across a rewrite — verified by chmod-ing the test file
to `0600` and re-running the writer, which left it `-rw-------` — so a
pre-create holds for the life of the file.

### ⚠️ The Flatpak pin and the dev machine have silently diverged

`packaging/flatpak/com.kysecurity.mail.yaml:363-369` builds
`kunifiedpush-26.04.3` from source, and its comment says it is *"version-matched
to this dev machine's installed kunifiedpush-26.04.3"*.

That is no longer true. This machine now has **26.08.0** (`pacman -Q
kunifiedpush` → `kunifiedpush 26.08.0-1.1`, and
`/usr/include/KUnifiedPush/kunifiedpush_version.h:6` → `"26.08.0"`).

So a native build and a Flatpak build now compile against different KUnifiedPush
versions, and the manifest comment asserts otherwise. Both are past 25.08, so
**this does not block the work** — but the comment is stale and the divergence
should be either closed or documented. It is the kind of drift that is harmless
until the day it is not.

---

## What does NOT need to change

- **`app/push/PushPayloadParser.h`.** The library hands over decrypted plaintext,
  which is the same JSON envelope as today. The parser never sees ciphertext.
- **`app/main.cpp:1535+`, the arrival path.** Same signal, same bytes, same
  `PushRepository::recordPushArrival()` and `NotificationDispatcher` handoff.
- **`deviceToken`.** Still the endpoint URL. Unchanged.
- **Any crypto code.** OpenSSL is already doing it inside the library —
  `nm -D /usr/lib/libKUnifiedPush.so.26.08.0` shows
  `KUnifiedPush::ContentEncryption::decrypt(QByteArray const&) const` and the
  `EVP_Decrypt*` symbols it calls.

---

## One decision for a human

**Do you also want VAPID subscription binding?**

`Connector` has `setVapidPublicKey()` and `setVapidPublicKeyRequired()`
(`connector.h:95,115`, both `@since 25.08`). Handing the distributor the server's
VAPID public key binds the subscription to that key, so only the holder of the
matching private key can push to the endpoint. That is anti-spoofing on top of
the confidentiality this handoff delivers, and it is genuinely valuable: the
endpoint URL is a bearer capability today.

**It is blocked on a small server change.** The server does publish its VAPID
public key — `GET /api/notifications/vapid-public-key`
(`backend/internal/api/server.go:652`,
`server_notifications.go:75-84`) — but that route is wrapped in `s.withAuth`,
which is a **web session**. A paired device holding only device credentials
cannot fetch it.

Cheapest fix: echo the VAPID public key in the native registration response,
which the device is already authenticated for and already parses
(`NativeRegistrationResponse`, `NativeRegistrationClient.h:18-34`). One field,
no new route, no new auth surface.

**Recommendation: ship encryption first, without VAPID.** It is independently
correct, it unblocks push-MFA, and it needs nothing from the server beyond
PR #149. Treat VAPID binding as a separate item once someone has decided whether
the endpoint-as-bearer-token property is worth a server round.

---

## Acceptance criteria

- [ ] `registerDevice` sends `p256dh` and `auth` when the connector has keys.
- [ ] A registration against a **current** server produces a device whose stored
      `p256dh`/`auth` are non-empty. Check the server's device list, not the
      request — `Auth` is redacted from responses, so assert on `p256dh` there
      and on `auth` only server-side.
- [ ] A real notification arrives, is decrypted, and reaches
      `NotificationDispatcher` with the same content as before. **Prove it end to
      end against a live distributor** — a unit test over `QJsonObject` proves
      the field is present, not that the round trip works.
- [ ] Against a server **without** PR #149, notifications still arrive. This is
      the safe-degradation path above; it should pass without any code guarding
      it, and if it does not, the fall-through analysis is wrong and the whole
      no-negotiation conclusion needs revisiting.
- [ ] Push-MFA approval works from Linux. It has never worked; if it still does
      not, the keys are not reaching the server.
- [ ] Malformed keys are refused with `400` and the pairing fails cleanly rather
      than half-registering.
- [ ] `find_package` version floor configures against the installed version and
      rejects something newer-than-installed. **[review] Already verified — see
      the table in §3; keep it as a regression check, not an open question.**
- [ ] **[review] Existing paired devices get keys, not just freshly-paired ones.**
      Pair on the old build, upgrade, relaunch, and assert the server-side
      `p256dh` went from empty to non-empty **without re-pairing**. This is the
      `tierChanged` route in §2 and it is the one a fresh-pair test cannot cover.
- [ ] **[review] `~/.config/kunifiedpush-com.kysecurity.mail` is `0600` after a
      FIRST registration on a clean profile** — not merely after the second
      launch. See the private-key hazard above. Delete the file and the pairing
      to test it; a reused profile passes this vacuously.

## What was not checked

- What the deployed server is running. **[review]** PR #149 itself is merged —
  checked; the deployment is not.
- The Flatpak build itself. The pin divergence was found by reading the manifest
  against `pacman -Q`, not by building.
- Whether `kunifiedpush-distributor` in the Flatpak sandbox behaves the same as
  the native one for content encryption. The manifest's own comments
  (`com.kysecurity.mail.yaml:350-353`) note that every live UnifiedPush proof so
  far has been on native, non-Flatpak builds — so this is a pre-existing gap in
  test coverage, and encryption rides on top of it.
