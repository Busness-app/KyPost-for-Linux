# Getting Relay HTTP off the GUI thread

Status: **Tier A complete; Tier B started.** All three SQL-free controllers
are asynchronous, and the mail refresh — the one request the user waits on
most — is too. This file is the plan for the rest, and — more importantly —
the record of the constraints that decide its shape, because they are
non-obvious and any one of them silently breaks a naive attempt.

## The problem

`HttpClient::waitForReply` drives a nested `QEventLoop`. On the GUI thread a
nested event loop is *worse* than blocking: input keeps being delivered,
timers keep firing, bindings keep re-evaluating. So any controller method
that makes a network call can be re-entered from QML while it is suspended
half-way through its own state changes.

Four things in this repo exist only to survive that:

| Mitigation | What it defends against |
|---|---|
| `core/util/ReentrancyGuard.h` | A controller method being entered twice |
| `PairingStore`'s lock epoch | `AppLock.lockNow()` landing inside `save()` |
| `DeviceRegistrationService`'s sealing-key snapshot | The key changing across the call (TOCTOU) |
| `main.cpp`'s `QTimer::singleShot(0)` on unlock | Escaping a half-finished `tryUnlock()` |

Each is correct. None of them is the fix, and the list grows every time
someone adds a network call. The fix is to run the loop on a thread with no
QML on it.

## Constraint 1 — thread-confined stores, of which SQL is only the loudest

The rule is: **any store in the call chain that is confined to a thread
pins that part of the chain to that thread.** Only the HTTP request may
move.

`QSqlDatabase` is the loudest case — a connection may only be used from the
thread that created it, and `main()` opens ours on the GUI thread. But it is
not the only one, and framing this as "the SQL tier vs the rest" was wrong:
`PairingStore` caches and is mutated by the credential gate, `SettingsStore`
is a `QSettings` file, and the certificate-pin fan-out reaches an
`HttpClient` that now asserts its own thread affinity. All three appear in
the *Tier A* chains.

That is why converting `PairingController` was not "run `pair()` over there".
`DeviceRegistrationService::pair()` had to be split into three phases —
`beginPair()` (guards, sealing-key snapshot, pin suspension) on the calling
thread, `sendRegistration()` on the executor, `finishPair()` (persist, pin,
settings) back on the calling thread — with the synchronous `pair()` kept as
the composition of the three so its 13 existing tests still pin the policy.

The tiers below still hold, but they are about *how much* has to be split,
not whether.

Measured, not assumed (`grep` for DAO/`QSqlQuery` use in each call chain):

**Tier A — no SQL anywhere in the chain. Can move as-is.**
`DeviceRegistrationService`, `PgpQrRepository`, `MfaResponseClient`,
`PgpQrClient`, `NativeRegistrationClient`, `DeregisterClient`.
→ `MfaController`, `PairingController`, `PgpQrController`.

**Tier B — the chain reads and writes the database.**
`MailRepository` (9 DAO uses), `ContactSyncRepository` (25),
`FolderRepository` (5), plus `PushRepository`.
→ `MailController` (14 network methods), `ContactsController` (5).

Tier B cannot simply run its repository call on the executor thread. Either
the repository is split into *plan → [worker: HTTP] → apply+persist*, or the
`Database` and every DAO move to the worker thread as well and the
controllers marshal plain values back.

**This file used to recommend the second. That was wrong, and the first
conversion is the evidence.** Splitting `MailRepository::refreshFolder` into
`planRefresh` / `fetchWith` / `applyRefresh` took one file, changed no QML,
and needed nothing from the composition root — because SQLite latency was
never what froze the UI. Moving the database would instead have made every
*purely local* read asynchronous as well: `findByMessageId()`,
`mailFolders()`, `allKeywordSettings()` and `cachedEmails()` all return
values straight to QML with no signal to hang them on, so it would have added
async conversions rather than removing them, and it would have made the
composition-root restructure a prerequisite for all of Tier B.

So: **Tier B is not blocked on anything.** It is the same three-phase split
already applied three times, repeated per method.

## Constraint 2 — one `HttpClient`, therefore one thread for pin state

The TOFU certificate pin lives in `HttpClient` and is written by
`DeviceRegistrationService::pair()` and `main()`, and read mid-handshake by
whatever request is in flight. Writing it from the GUI thread while a request
reads it on the worker thread is a plain data race.

`NetworkExecutor::configure()` exists for this: it applies a change **on** the
executor thread and blocks until done. Blocking is fine there — these are
in-memory field writes, microseconds, not round trips.

During the migration two `HttpClient`s coexist (the GUI-thread one for
unconverted callers, the executor's for converted ones), so pin state has to
reach both until the last caller moves. That is handled structurally rather
than by remembering — see "Certificate pin state during the migration" below
for the fan-out, and for the unpinned path that shipped before it existed.

## What exists now

- **`core/net/NetworkExecutor`** — owns a `QThread`, and a
  `QNetworkAccessManager` + `HttpClient` constructed *on* it.
  `run(receiver, work, onDone)` returns immediately, runs `work` on the
  executor thread, delivers the result to `onDone` on the receiver's thread.
  12 tests, including one that asserts the calling thread is not re-entered
  while a request is in flight.
- **`MfaController`** — the reference conversion. Chosen as the pilot
  because it is SQL-free, has tests, and is not registered with QML, so
  getting it wrong could not break a screen.
- **`DeviceRegistrationService`** — split into `beginPair()` /
  `sendRegistration()` / `finishPair()`, with `pair()` kept as their
  composition. `PairAttempt` is move-only and restores the certificate pin
  on destruction, so an attempt abandoned mid-flight (executor shut down,
  controller torn down) cannot leave pinning disabled — the async form of
  the guarantee `ScopedPinSuspension` gave within one stack frame.
- **`PairingController`** — `confirmPendingPair()` and `removePairing()` are
  asynchronous. No QML changed: every call site was already fire-and-forget
  driven by `pairingState` (checked before starting). `removePairing()`
  clears local state synchronously and dispatches the best-effort deregister
  with `runDetached` — its result was already ignored, and unpairing must
  not depend on the relay being reachable.
- **`PgpQrController`** — `refreshMyQrCode()` and `scanQrPayload()` are
  asynchronous. `PgpQrRepository::fetchMyToken()` was split the same way
  (`resolvePairing()` here, `fetchTokenWith()` there), with the synchronous
  form kept as the composition. `scanQrPayload()` needed no split at all: it
  uses no pairing, so the whole request moves.

  No QML changed here either. The earlier note in this file claiming
  otherwise was wrong: `myQrImageDataUrl()` and `scannedContactCardFields()`
  do return values to QML, but they are pure local encode/reshape of state
  already fetched, and both call sites already read them from a signal
  handler (`onMyQrDataChanged`, `onKeyScanned`) rather than straight after
  the network call.
- **`MailController::refresh()` / `selectFolder()`** — the first Tier B
  conversion. `MailRepository::refreshFolder()` split into `planRefresh()`
  (pairing + cursor, here) / `fetchWith()` (there) / `applyRefresh()` (delta
  merge and every DAO write, back here), with the synchronous form kept as
  the composition. No QML changed: both were already fire-and-forget.

  Two things this one added that the Tier A conversions did not need:

  * **`isBusy` is a depth, not a flag.** While the rest of this class is
    still synchronous, the two overlap — a click on Archive during an
    in-flight refresh ran `setBusy(true)`/`setBusy(false)` around itself and
    cleared the indicator out from under the refresh, reporting idle with a
    request outstanding. Every network-reaching method now brackets itself
    with `pushBusy()`/`popBusy()`.
  * **Coalescing keeps a *pending* flag, not just an in-flight one**, and the
    follow-up re-reads `m_currentFolder`. Dropping the overlapping call
    outright (what `PgpQrController` does, correctly, for camera frames)
    would mean a folder selected during a refresh was never fetched at all.
    `forceFullResync` is sticky across the fold, so a user-initiated full
    resync cannot be silently downgraded into the background delta it landed
    on top of.
- **The four mail actions** (archive/delete/spam/move) — report via
  `actionCompleted(action, messageIds, ok)`. Six QML sites read the `bool`
  before; they read the signal now, and `messageIds` is how each decides
  whether the answer was theirs. That value has to be *carried*, because the
  test it replaces was "is a call of mine on the stack", and with the request
  off-thread nobody has one. MobileRoot's swipe rows also had to invert their
  double-tap latch: it was set on success, which only worked while the call
  was synchronous — the window it guards is now the whole round trip, so it
  is set on dispatch and released by the failure case.
- **The four folder methods** — `FolderRepository` split the same way, with
  one wrinkle: each mutating verb is two round trips (mutate, then re-list,
  because the backend decides the resulting path and a deletion is only
  visible as an absence). Both go in phase 2 together; splitting them would
  put a thread hop between a mutation and the re-list that makes it visible,
  for nothing.

### The conversion pattern

```cpp
void SomeController::doThing(QString arg)
{
    if (m_inFlight) return;                    // coalescing, not re-entrancy
    const auto pairing = m_pairingStore.load(); // GUI thread: stores stay here
    if (!pairing) { setState("failed"); return; }

    setInFlight(true);
    setState("sending");                        // UI updates before we return

    m_executor.run(this,
        [/* captured BY VALUE */](HttpClient& http) {
            SomeClient client(http);            // net clients are stateless
            return client.call(...);            // wrappers -- build them here
        },
        [this](SomeResult r) {                  // back on this thread
            setInFlight(false);
            applyResult(r);
        });
}
```

Three rules that make it work:

1. **Read stores on the calling thread, capture only plain values.**
   `PairingStore` caches and is mutated by the credential gate; it stays GUI-confined.
2. **Construct the `net/` client inside the work.** They are all stateless
   wrappers over an `HttpClient&`, so this is free — and it avoids holding a
   reference to another thread's object.
3. **Tests become `QTRY_*` on published state**, not assertions on a return
   value that no longer exists. A `busy`/`inFlight` property makes a uniform
   barrier: set synchronously before the call returns, cleared by the
   completion handler.
4. **Assert coalescing as a comparison, not an absolute count.** Against a
   server that accepts and never answers, one request is not one TCP
   connection — Qt retries an idempotent GET after the transfer timeout
   (measured: 3 connections for 1 request). "Ten calls cost what one call
   costs" is the property; a hardcoded count is a flake.

## Remaining order

1. ~~`PairingController`~~ — done.
2. ~~`PgpQrController`~~ — done. **Tier A is complete.**
3. **Retire the GUI-thread `HttpClient`** — still BLOCKED, and not by Tier A.
   Eight clients are still constructed on it in `main()`
   (`RelayMailSource`, `ContactSyncClient`, `GroupsClient`, `FolderClient`,
   `ContactPhotoClient`, `PgpBootstrapClient`, `PgpRecipientChecker`,
   `PushNotificationClient`), all of them Tier B. So this cannot happen
   before step 4, and the certificate-pin fan-out keeps both targets until
   it does. When it can: drop `guiThreadPinSink` from the fan-out in
   `main()`, leaving the executor as the only target.
4. **Tier B**, in progress. Not blocked on the composition root — see
   Constraint 1. Done so far: the mail refresh, the four actions, and the four
   folder methods. Remaining, in the order they are worth doing:
   - `sendMail` / `saveDraft` / `confirmPickupFallbackSend`. The awkward one:
     `pickupFallbackRequired` is documented as being emitted *synchronously*
     so a Compose instance can tell its own send from another window's. That
     ownership test has to be replaced (carry the token out of `sendMail()`
     instead) before the emit can become asynchronous.
   - `FolderRepository` create/rename/delete/refresh, same three-phase split.
   - `refreshPgpComposeState` / `preflightRecipients`. Both already have their
     own in-flight flags, so they convert cleanly; `m_preflightInFlight`
     becomes ordinary coalescing rather than a re-entrancy defence.
   - `ContactsController` (5 methods) over `ContactSyncRepository`'s 25 DAO
     uses — the largest single split left.
5. **Delete `ReentrancyGuard`** and revisit the three other mitigations
   above. They become unnecessary, not merely redundant. `MailController`
   still holds `m_inNetworkCall` for its unconverted methods and is the last
   real user.

## ThreadSanitizer does not work here — use the affinity guard instead

Do not add a TSan job to CI, and do not trust a TSan run against Qt. This was
measured, not assumed.

Qt is not built with `-fsanitize=thread`, so the happens-before edges its
event queue establishes are invisible and **everything passed through a
queued connection is reported as a race**. Numbers, from a probe carrying
both a known-correct Qt handoff and a deliberately injected real race:

| Configuration | Qt false positives | Real race (plain threads) | Real race (inside Qt slots) |
|---|---|---|---|
| No suppressions | 11 | 12 | 10 |
| `race:qobjectdefs_impl.h` | 5 | 6 | 5 |
| Narrow (`std::function`, `QCallableObject`, …) | 4 | 4 | 4 |

Both suppression sets cut the **real** reports as well, so neither is safe to
ship: a suppression file that hides genuine races is worse than no tool.
TSan happens-before annotations at our own seam were tried too and moved 11 →
10, because the residual reports are Qt's internal `QMetaCallEvent` storage,
which cannot be annotated from outside.

### What replaces it

`HttpClient` records its constructing thread and checks it on every public
entry point (`assertOwningThread`). That covers the defect that actually
matters — a client touched from the wrong thread, which for the certificate
pin is a data race on the value deciding where the device secret is sent —
and it is deterministic, has zero false positives, and fires in every test
run.

Deliberately not a bare `Q_ASSERT`: the default build type is Release, which
defines `NDEBUG`, so an assert-only check would be absent from exactly the
build users run. The condition is evaluated and reported (`qCritical`)
unconditionally; the abort is the debug-build extra.
`CertificatePinSinkTest::touchingAnHttpClientFromTheWrongThreadIsReported`
pins this.

TSan was still worth running once by hand — it found two genuine defects
that review had not:

- `NetworkExecutor::run` originally held a `QPointer` to the receiver and
  checked it on the executor thread. `QPointer` is documented as not
  thread-safe; the check races with `~QObject` and can report "alive" for an
  object already being torn down. Replaced with a stated precondition
  enforced by `shutdown()` ordering.
- Several tests observed cross-thread state through plain `bool`s. Now
  `std::atomic`.

If you do run it again, treat only reports whose *both* sides are in this
repo's own code as real, and expect to filter by hand.

## Certificate pin state during the migration

Pin state lives inside `HttpClient`, and there is more than one of those
while this is in progress. The first conversion shipped an **unpinned path**
because of it: `MfaController` moved to the executor's client, which nobody
had given a pin or a mismatch handler, so its requests went out under
whatever certificate the CA chain accepted and the impersonation banner had
nothing to compare against.

`core/net/CertificatePinSink` is the structural fix. Everything that mutates
pin state takes a `CertificatePinSink&` rather than an `HttpClient&`, and
`main()` builds a `FanOutCertificatePinSink` over the GUI-thread client and
the executor. Adding a client means adding one entry; retiring the
GUI-thread one means removing one. Nobody has to remember to do it twice.

The executor's entry goes through `NetworkExecutor::configure()`, which
applies the change on the executor thread and blocks — the pin is read
mid-handshake there, so writing it from the GUI thread would be the race the
affinity guard now catches.
