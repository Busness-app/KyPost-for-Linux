# Inbox `bodies=0` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop downloading five hundred message bodies to render one. Ask the relay for the inbox window without bodies, fetch the opened message's body on demand, and cache only what the user actually opened.

**Architecture:** `GET /api/inbox?bodies=0` omits `body`/`bodyMode` from every row on every path (warm cache, live fetch, both halves of the `since=` delta). `GET /api/mail/body?mailbox=&messageId=<uid>` returns `{body, bodyMode}` for one message, taking the same auth and the same `messageId`+`mailbox` pair the attachment endpoints already take. The fetch hangs off *selection*, not off the reader's render pass, and its result is written into the existing `emails` row so an opened message stays readable offline.

**Tech Stack:** Qt 6 only, C++20, QtTest with the real-socket `FakeRelayServer` harness, ctest.

**Server dependency:** both halves are already merged and deployed on `kypost-server`
(`backend/internal/api/server_inbox.go:306` for `bodies`,
`backend/internal/api/mail_body.go` + the route at `server.go:512` for the body
endpoint). The porting guide is `kypost-server/docs/INBOX_PAYLOAD_HANDOFF.md`;
read the two corrections below before trusting it.

**Prerequisite:** the `bodyMode` work on branch `fix/body-mode-from-relay` — `Email::bodyMode`, migration `007_emails_body_mode.sql`, and `Format.emailBodyIsHtml()`. This plan assumes a message's form travels with its body. Do not start here without it.

## What this is worth, measured

From `kypost-server`'s own `TestInboxPayloadSize` / `TestInboxDeltaPayloadSize`, re-run 2026-08-25 (500 messages, 20 KiB average body):

| this client's actual request | today | with `bodies=0` |
|---|---|---|
| cold folder sync (`since=0`) | 13.3 MiB raw → ~1.5 MiB gzipped | 121 KiB raw → ~3 KiB gzipped |
| warm delta, nothing changed | 95 B | 95 B — unchanged |
| warm delta, per new message | ~20 KiB of body, opened or not | metadata only |

Plus the two that do not show up on the wire: the `emails` table stops storing
roughly 10 MB of never-opened mail per folder, and peak JSON decode per refresh
drops from ~13 MiB to ~120 KiB.

**Do not size this work off the handoff's headline.** The browser's 53 MiB/min
idle cost does not exist here: there is no inbox poll timer in this client.
`MailApp.refresh()` fires on user action, folder switch, and push delivery.
`TransportStateMachine`'s 90-second timer polls `PushRepository::pullOnce()`
for notifications, not `/api/inbox` (`app/main.cpp:1543`).

## Two corrections to the handoff

Both were verified in this checkout on 2026-08-25. The handoff is wrong about
this client, because it was written from the security-audit copies rather than
from source.

1. **The `since=` delta path is not dead code.** `MailRepository::planRefresh`
   always sends `since` (0 when there is no cursor) and `applyRefresh`
   persists the cursor on both branches — fixed in `5ca4ad9`, asserted at
   `tests/core/domain/MailRepositoryTest.cpp:123`. This client is normally on
   the delta path, which already omits bodies for `changeType: "updated"`
   rows. The remaining win is cold sync and newly-arrived messages.
2. **There is exactly one consumer of the row's `body`:**
   `EmailDetail.qml`'s `applyContent()`. No draft-open-into-composer, no
   search results, no print. Reply/forward quote `email.preview`, which
   `RelayMailSource` never populates — a separate, pre-existing bug, out of
   scope here.

## Global Constraints

- Qt 6 only. One out-of-tree build dir: `cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKYPOST_SQLCIPHER_ROOT=/tmp/sqlcipher`, `cmake --build build`, `ctest --test-dir build`. Not verified until that is green (`AGENTS.md` §3). Without the SQLCipher root the encryption tests `QSKIP` and the suite goes green having tested nothing.
- **Flip `bodies=0` last.** Every task before it is additive and ships safely on its own; the flip is what makes messages blank if the fetch path is not already working. Landing step 1 without step 2 ships an app where every message is empty.
- **The default response is wire contract.** Never ask the server to change its default, and never assume another client has adopted this. `bodies=0` is opt-in per request.
- **Do not batch-prefetch visible rows.** A screen is 10–20 rows and the user opens one. Prefetching turns a 3 KiB list back into a 300 KiB one to save a round trip usually not taken.
- **PGP state must keep classifying from the stored row only.** `pgpMessageStateOf()` reads `body`, and `pgpEncrypted && no body && no error` is the wire signature of a client-protected message (`core/domain/PgpMessageState.h`). If a fetched body reaches the classifier before it is stored, a row's badge changes meaning after the user opens it. The server decrypts for nobody (`pgp_receive.go`), so an encrypted message has no body to fetch anyway — skip the fetch when the cached row classifies as `ClientProtected`, and let `PgpPayloadClient` keep owning that path.
- **Fetch the body for signed-but-not-encrypted mail.** A signature that fails to verify must fall back to the server's copy rather than costing the reader the message. Never skip the fetch on a signature flag.
- **In-flight is not empty.** "No message body available." is true only when the server answered and had nothing. A pending fetch is a spinner; a failed one is an error the user can retry. Three distinct states, plus the body itself.
- **`413` is its own answer.** The relay returns it for a message too large to hold in memory. Say that; do not report it as an empty message.
- The messageId sent to `/api/mail/body` is the same value `listAttachments` already sends (the relay parses it as an IMAP UID via `attachmentRequestParams`). Do not invent a second identifier.
- `HttpClient::kMaxInboxResponseBytes` (64 MiB) stays as it is — a ceiling, not an allocation, and a relay that ignores `bodies` still has to fit. The body endpoint uses the default ceiling: one message, not five hundred.

## File Structure

| File | Responsibility |
|---|---|
| `core/net/RelayMailSource.{h,cpp}` | `bodies=0` on the inbox query; `MailBodyResult` + `fetchBody()` |
| `core/domain/MailRepository.{h,cpp}` | `storeBody()`; body preservation across a full snapshot |
| `app/mail/MailController.{h,cpp}` | `loadBody()` invokable, `bodyLoaded()` signal, pairing re-check |
| `app/qml/pages/EmailDetail.qml` | Fetch on selection; the four render states |
| `tests/core/net/RelayMailSourceTest.cpp` | Query param; body decode; error mapping |
| `tests/core/domain/MailRepositoryTest.cpp` | Snapshot preservation; `storeBody` round trip |
| `tests/app/mail/MailControllerTest.cpp` | Re-pair mid-flight writes nothing |
| `tests/qml/tst_EmailBodyRendering.qml` | Loading/error/empty are three different things |

**Harness facts:** `FakeRelayServer` takes its canned response in the constructor and **accepts exactly one connection** — a test covering an inbox fetch *and* a body fetch needs two instances on two ports. Copy the shape from `tests/core/net/RelayMailSourceTest.cpp`.

---

### Task 1: `fetchBody()` on RelayMailSource

- [ ] Add `MailBodyResult { std::optional<NetworkError> error; QString detail; QString body; std::optional<QString> bodyMode; bool tooLarge = false; }` to `RelayMailSource.h`, documented against `mail_body.go`'s response literal.
- [ ] Add `MailBodyResult fetchBody(const QUrl&, const RelayAuth&, const QString& mailbox, const QString& messageId) const`, mirroring `listAttachments()`: same query pair, same decode-or-`NetworkError::Decoding` shape, default response ceiling.
- [ ] Map `413` to `tooLarge` rather than folding it into a generic failure. Everything else follows the existing `HttpClient` error mapping.
- [ ] Absent `bodyMode` stays `std::nullopt` — "the server did not say", never `"plain"`.
- [ ] Test: `"html"` and `"plain"` both decode; an absent mode stays absent; a `413` sets `tooLarge`; a `404` reports an error without `tooLarge`.

**Verify:** `ctest --test-dir build -R RelayMailSourceTest --output-on-failure`

### Task 2: Store a fetched body, and stop the snapshot from wiping cached ones

- [ ] `MailRepository::storeBody(folder, messageId, body, bodyMode)` → `EmailDao::insertOrReplace` on the cached row. A missing row is a no-op success, not a failure: the message can be deleted while the fetch is in flight.
- [ ] In `applyRefresh`'s **non-delta** branch, carry cached `body`/`bodyMode` forward onto incoming rows that have neither, exactly as the delta branch already does at `MailRepository.cpp:209`. `replaceFolderSnapshot` wipes the folder first, so without this one forced resync empties every body this plan just decided to keep.
- [ ] Keep the preservation gated on "the incoming row has no body" so a relay that *did* send one still wins.
- [ ] Test: a `bodies=0` full snapshot leaves cached bodies and modes intact while metadata still updates; `storeBody` round-trips through migration 007; `storeBody` on an unknown id reports success and writes nothing.

**Verify:** `ctest --test-dir build -R MailRepositoryTest --output-on-failure`

### Task 3: `loadBody()` on MailController

- [ ] `Q_INVOKABLE void loadBody(const QString& folder, const QString& messageId)`, running the fetch on the executor thread with `pushBusy()`/`popBusy()`, shaped like `listAttachments()` (`MailController.cpp:1016`).
- [ ] Re-check `m_pairingStore.stillCurrent()` in the completion handler **before** `storeBody`. A reply landing after a re-pair would otherwise write the previous account's plaintext into the new account's cache, where the new user reads it — the same rule `applyRefresh` already enforces, for the same reason.
- [ ] Emit `bodyLoaded(folder, messageId, body, bodyMode, status)` where `status` distinguishes ok / not-found / too-large / failed. Wording stays in `app/` (`AGENTS.md` §6c); `core/` owns the value.
- [ ] Add `bodyMode` to `findByMessageId`'s map if the prerequisite branch has not already.
- [ ] Test: a `bodyLoaded` arriving after the pairing changed writes nothing and reports failure.

**Verify:** `ctest --test-dir build -R MailControllerTest --output-on-failure`

### Task 4: Fetch on selection in EmailDetail

- [ ] In `reload()`, when the cached row has no body and its `pgpState` is not `ClientProtected`, set `bodyState = "loading"` and call `MailApp.loadBody(folder, messageId)`.
- [ ] Handle `bodyLoaded` via a `Connections` block that **ignores replies whose `messageId` is not the one on screen** — the same id-comparison discipline `Format.decryptedBodyFor` applies to decrypted plaintext, for the same reason.
- [ ] `applyContent()` renders four states: loading (spinner), error (message + retry), answered-and-empty ("No message body available."), and the body itself via `Format.emailBodyIsHtml(bodyMode, body)`.
- [ ] `hasRenderableBody` must account for the loading state so the image-blocking notices do not flicker in under an empty view.
- [ ] Test in `tst_EmailBodyRendering.qml`: the three non-body states are distinguishable, and a too-large message says so.

**Verify:** `ctest --test-dir build -R QmlTests --output-on-failure`

### Task 5: Flip the inbox request

- [ ] Append `bodies=0` to `fetchInbox`'s query in `RelayMailSource::fetchInbox`.
- [ ] Update the method's doc comment: the response no longer carries bodies, and `InboxEmailItem::email.body` is expected to be absent.
- [ ] Test: the request contains `bodies=0`; a body-less response still parses, still buckets by tab, still carries keywords and PGP fields.

**Verify:** `cmake --build build && ctest --test-dir build --output-on-failure` — the whole suite, not just the new tests.

### Task 6: Verify against the real relay

- [ ] Pair against `mail.urlxl.com`, open a folder, and confirm in the journal that the inbox response is kilobytes rather than megabytes.
- [ ] Open an HTML message, a plain-text message, a message with attachments, and a signed message. All four render.
- [ ] Go offline and re-open a message read while online: it still renders. Open one never opened: it says so, and does not claim the message is empty.
- [ ] Force a full resync and confirm previously-opened bodies survive it.
- [ ] Run `./scripts/verify-guards.sh` on a clean tree.

---

## Out of scope

- The empty-`preview` reply/forward quote bug.
- Any change to `EmailListModel`'s roles — the list renders no body today and must not start.
- Bounded eviction of cached bodies. Cache-on-open is already bounded by what the user opened; add a cap only if a real profile measures large.
