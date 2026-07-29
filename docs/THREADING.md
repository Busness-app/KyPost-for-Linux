# Getting Relay HTTP off the GUI thread

Status: **in progress.** The infrastructure exists and one controller is
converted. This file is the plan for the rest, and — more importantly — the
record of the two constraints that decide its shape, because both are
non-obvious and either one silently breaks a naive attempt.

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

## Constraint 1 — `QSqlDatabase` is bound to the thread that opened it

This is what splits the work into two tiers, and it is not negotiable: a
`QSqlDatabase` connection may only be used from the thread that created it,
and `main()` opens ours on the GUI thread.

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
the repository is split into *prepare → [worker: HTTP] → apply+persist*, or
the `Database` and every DAO move to the worker thread as well and the
controllers marshal plain values back. The second is cleaner and is the
recommendation, but it means constructing `Database` on that thread — which
is the composition-root restructure that was deliberately deferred.

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
   value that no longer exists.

## Remaining order

1. **`PairingController`** (2 methods, Tier A). Needs care: `pairFromDeepLink`
   and `removePairing` touch `PairingStore` *and* the pin. Move pin
   installation to `NetworkExecutor::configure()` in the same change.
2. **`PgpQrController`** (2 methods, Tier A).
3. **Retire the GUI-thread `HttpClient`** once Tier A is done and no Tier B
   caller remains on it. Concretely: drop `guiThreadPinSink` from the
   fan-out in `main()`, leaving the executor as the only target.
4. **Tier B**: move `Database` + DAOs to the executor thread, which requires
   extracting the composition root first. `MailController` and
   `ContactsController` follow, along with ~12 QML sites that currently
   consume a return value (`if (MailApp.archiveEmails(...))` becomes a
   signal handler).
5. **Delete `ReentrancyGuard`** and revisit the three other mitigations
   above. They become unnecessary, not merely redundant.

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
