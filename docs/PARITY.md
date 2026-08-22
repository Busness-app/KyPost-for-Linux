# Android ↔ Linux parity matrix

**Status: authoritative.** This file, not `Linux_QT_Client_Plan.md`, is the
acceptance checklist for parity work. The plan document predates several
locked decisions and still describes a dual-Qt/Ubuntu-Touch target; where the
two disagree, this file and `AGENTS.md` win.

The Android behaviour contract is `~/busness.app/kypost-android/app/AGENTS.md`.
The wire contract is the backend Go source
(`~/busness.app/kypost-server/backend/internal/api/`), never a guess.

## How to read a row

| Status | Meaning |
| --- | --- |
| **Matched** | Linux does the same thing, and a test pins it. |
| **Equivalent** | Different mechanism, same user-visible outcome. The mechanism is named. |
| **Missing** | Android has it, Linux does not, and it is agreed that Linux should. |
| **Intentional difference** | Android has it, Linux deliberately does not. A reason is required, not a shrug. |
| **Blocked** | Waiting on the backend or on a platform capability. |

A row may not sit at **Missing** without an owner phase. A row may not sit at
**Intentional difference** without a reason someone could argue with.

## 1. Mail

| Capability | Status | Notes |
| --- | --- | --- |
| Inbox, keyword tabs, folder list | Matched | |
| Delta/cursor sync | Matched | Fixed 2026-08-22. `since` is always sent; `since=0` is the full window. See `AGENTS.md` §4 — the previous two versions of that entry were both wrong. |
| Cursor scoped per (subscriber, folder) | Matched | `CursorStore::mailCursor(subscriberId, folder)`, mirroring Android's `MailCheckpoint`. |
| Full-window prune of vanished rows | Matched | `replaceFolderSnapshot` on the `delta:false` branch. |
| Cursor withheld when the cache write fails | **Better than Android** | `MailRepositoryOutcome::CacheWriteFailed`. Advancing past unwritten rows makes them unrequestable forever; Android does not check this. |
| Same message ID in two folders | Matched | PK is `(folder, message_id)` since migration 006. |
| Archive / spam / delete / move / read | Matched | |
| Reply, Reply All, Forward | Matched | Verified line-for-line against `EmailDetailActivity.kt:210`: sender + To + Cc, deduped, blanks dropped. **Neither client removes the local identity** — a 2026-08-22 review claimed Android did; it does not. |
| Drafts, attachments, HTML/plain/markup compose | Matched | |
| Open mail from a notification | **Equivalent, partial** | The UnifiedPush envelope carries no mailbox (`PushPayloadParser.h`), so an ID cached in two folders is genuinely ambiguous. `findCachedEmail` refuses rather than opening the wrong copy. The UI still renders that as a blank page — marked `ponytail:` in `MailController::findByMessageId`. |
| Force a full refresh before opening from a notification | **Missing** | Phase 4. |
| Per-route response size limits | Matched | Added 2026-08-22. `HttpClient` enforces a ceiling on both the declared `Content-Length` (at the headers, before any body arrives) and the running byte count (the only guard a chunked response leaves), reporting `NetworkError::ResponseTooLarge` and discarding the partial body. Shape follows Android's `MemoryBudget.kt`; **values deliberately differ** — Android asks the relay for `limit=50`, this client sends no limit and gets the server default of 500 with untruncated bodies, so Android's 8 MB would have broken first sync here. Default 8 MB, inbox 64 MB, attachment 25 MB. |
| Inbox window size | **Open question** | Linux sends no `limit`, so the relay's default 500 applies and a full window carries 500 untruncated HTML bodies — the largest allocation in the app, and why the response ceiling for that one route is 64 MB. Android asks for 50. Lowering it would shrink the response ~10x and allow a much tighter cap, but `replaceFolderSnapshot` prunes to the window, so it would also cut the local archive 10x. That is a product call, not a CI or security one. |
| Pull-to-refresh on mobile | **Equivalent** | Explicit refresh control. Revisit if that is judged insufficient. |

## 2. Contacts

| Capability | Status | Notes |
| --- | --- | --- |
| CRUD, groups, photos, dedupe, offline changes, autocomplete | Matched | |
| System address-book sync | **Intentional difference** | Reaffirmed 2026-08-22. Akonadi/KDE PIM integration is not a product requirement, and the dependency is large relative to the benefit. `core/db/migrations/002` already carries a `native_contact_links` table with an `akonadi` backend column, so the groundwork survives if this is ever reversed. |

## 3. Pairing, credentials, recovery

| Capability | Status | Notes |
| --- | --- | --- |
| Pairing, TLS TOFU pinning, credential sealing, deregistration | Matched | |
| Non-destructive "Reconnect to server" | Matched | Added 2026-08-22, as `reconnectToServer()`. Note the review's premise was partly wrong: `removePairing()` never called `LocalDataWipe`, so cached mail and contacts already survived unpairing. What unpair actually costs is a server-side deregistration and a fresh pairing link. |
| Staged account replacement (register, then purge) | Matched | Added 2026-08-22. Pairing a **different** account now erases the previous one's cached data — and only after the replacement registration has succeeded, so a failed pairing cannot delete a working account's mail. If the purge leaves anything behind, the new pairing is refused and the device is wiped: no table carries a subscriber column, so survivors are readable by whoever pairs next. Mirrors Android's `PushSyncCoordinator.attemptPairing`. |
| Serialised registration / re-registration / pull | **Partial** | `reconnectToServer()` and `pairFromParsedParams()` guard on `m_inNetworkCall`, so no two registrations overlap, and the previous account is captured **before** the request rather than re-read after it (the TOCTOU Android's `registrationGate` closes by reading inside the mutex). Registration is still not *serialised* against an in-flight pull — but as of 2026-08-22 it no longer needs to be for the mail and push paths, which now discard a reply the current pairing did not authorise (row below) instead of blocking the request that produced it. |
| Replies discarded when they outlive their pairing | Matched | Added 2026-08-22. `PairingStore::stillCurrent()` compares the (subscriberId, deviceId) identity captured **before** a request against the one held when its reply lands. Enforced at every place a relay reply reaches local storage: `MailRepository::applyRefresh`, `PushRepository::pullOnce`, `FolderRepository::applyList`/`applyMutation`, `GroupsRepository::applyRefresh`, `ContactSyncRepository::applySync`, `ContactPhotoRepository::photoPathFor`, attachment downloads, and the cached PGP compose state and recipient preflight. Deliberately not keyed on `deviceSecret`, which the credential gate rewrites on every lock/unlock. Not enforced on `MailController::finishAction`, which touches only the in-memory list and writes nothing to disk. |
| Secure-store *unreadable* distinguished from *absent* | **Partial** | The credential gate is flag-OR-blob and fails closed (`AGENTS.md` §6d), but `SecureStore::get()` still collapses both into `std::nullopt` everywhere else. Phase 2. |
| Certificate-renewal recovery UI | Matched | Added 2026-08-22. `Pairing.reconnectToServer()` re-anchors the pin via a forced re-registration — **not** a pin clear: the pin is only ever captured during registration, so clearing it alone would leave the device permanently unpinned. `CertificateChangeDialog.qml` shows the pinned and presented SPKI fingerprints side by side before the user decides. Non-destructive: no deregistration, no cache loss; `removePairing()` stays the destructive path. |
| Secret Service calls always terminate | Matched | Fixed 2026-08-22. `SecureStoreKeychain::runBlocking` waited on `QKeychain::Job::finished` alone, and QKeychain never emits it when its D-Bus call gives up — so the app hung **forever** at startup, before any window, with nothing in the journal. Bounded now, and a timeout maps to `Failed`, never `Absent`. Measured floor: the first call costs ~25 s inside a synchronous D-Bus call (Qt's default), which no timer can shorten; `main()`'s canary therefore stops at the first failure instead of paying it three times. |
| Sync cursors erased on wipe | Matched | Fixed 2026-08-22. `cursors.ini` survived every wipe path — `CursorStore::reset()` had no caller anywhere in the app. |

## 4. Local data protection

| Capability | Status | Notes |
| --- | --- | --- |
| PIN lock, lockout, wipe, hidden notifications | Matched | |
| Hostile Location Protection (memory-only database) | **Better than Android** | No Android equivalent. |
| Encrypted database at rest | **Missing** | Phase 3, **its own branch** (decided 2026-08-22). SQLCipher is not in `org.kde.Platform`, so it needs a Flatpak module and a custom Qt SQL driver path. `AppLockStore.h` documents the current honest position; do not weaken that wording before the encryption actually ships. |
| Configurable background-lock grace period | **Missing** | Phase 5. |
| Configurable erase-after threshold, incl. "never" | **Missing** | Phase 5. `LockoutPolicy::kWipeThreshold` is a compile-time constant. |
| Interrupted-wipe tripwire, incomplete-wipe state | **Missing** | Phase 5. |
| Biometric unlock | **Intentional difference** | No portable Linux equivalent. `AGENTS.md` §4. |
| Window-content protection (`FLAG_SECURE`) | **Intentional difference** | No Wayland analogue. `AGENTS.md` §4. |

## 5. Push

| Capability | Status | Notes |
| --- | --- | --- |
| UnifiedPush with 90 s polling fallback | Matched | |
| MFA over push | **Blocked** | The relay derives `transport: "unifiedpush"` from `platform: "linux"` and filters those devices out of every MFA challenge. Confirm the server filter is gone before building any MFA UI. `AGENTS.md` §4. |

## 6. OpenPGP

| Capability | Status | Notes |
| --- | --- | --- |
| Public-key QR exchange, encrypted-message awareness | Matched | |
| Device enrolment, private-key custody, client-side decrypt/sign/encrypt | **Under review — decision re-opened 2026-08-22** | `AGENTS.md` §4 currently reads "No PGP private key on this client, ever", and client-protected mail is routed to webmail. That decision has been re-opened at the user's direction. **It is not yet reversed**: until §4 is edited, the ban stands and no private-key code may land. Reversing it means a separate project — keyring-protected key storage, memory clearing, authenticated enrolment, payload bounds, PGP/MIME parsing, and a security test suite proportional to holding users' private keys. |

## 7. Presentation

| Capability | Status | Notes |
| --- | --- | --- |
| Fifteen themes | Matched | `AppTheme::themeNames()` is the source of truth for the count. |
| Desktop, mobile, wide/master-detail layouts | Matched | |
| Compose/contact-edit drafts survive layout change and app lock | **Unverified** | Phase 4. Must not write plaintext form data to disk. |
