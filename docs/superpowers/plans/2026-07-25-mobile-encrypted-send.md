# Mobile Encrypted Send — kypost-Linux / kypost-for-Mac Implementation Plan

> **SUPERSEDED (2026-07-26) — DO NOT EXECUTE.**
> Replaced by `docs/superpowers/plans/2026-07-26-client-encrypted-send.md`, written
> against the `Client_Encrypted_Send.md` contract. This plan disagrees with the
> contract and with the shipped code in four ways, each verified:
>
> 1. Its `sendGateFor()` opens the plaintext-link confirmation from the *preflight*
>    result. The preflight reads contacts only while the send path also runs WKD
>    discovery, so that shows the dialog for recipients who do have a discoverable
>    key. The server's 409 must be the gate.
> 2. Its test code targets a `FakeRelayServer` API that does not exist
>    (`enqueue`/`baseUrl`/`httpClient`/`lastRequestBody`). The real harness takes a
>    canned response in its constructor and serves exactly one connection.
> 3. It requires matching `kypost-android`'s `pgpComposeStateOf`, which does not
>    exist anywhere in that repo.
> 4. Its `openWebmailDrafts` builds a `QUrl` with no scheme validation, bypassing
>    the https-only check already shipped in `app/mail/PgpMessagePresentation.cpp`.
>
> Kept for its reasoning, which the replacement reuses. Nothing here was implemented.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a server-custody account send signed and encrypted mail from the desktop app, and hand a client-custody account off to webmail instead of failing.

**Architecture:** The app does no OpenPGP. It sets `sign`/`encrypt`/`allowPickupFallback` on `/api/mail/send` and lets the server do the crypto. Custody mode comes from `GET /api/pgp/bootstrap`; keyless recipients come from `POST /api/pgp/recipients/check`. Client-custody compose saves a draft and opens webmail via `QDesktopServices`.

**Tech Stack:** Qt 6 only (Qt 5 is not a build path — `AGENTS.md` §3), C++, QtTest with a stubbed `HttpClient`, ctest.

**Depends on:** the server plan in `kypost-server/docs/superpowers/plans/2026-07-25-mobile-encrypted-send-server.md`. This mirrors `kypost-android/docs/superpowers/plans/2026-07-25-mobile-encrypted-send.md` — read that one's `PgpComposeState` task before writing this one's, because the two must agree.

**Spec:** `kypost-server/docs/superpowers/specs/2026-07-25-mobile-encrypted-send-design.md`

## Global Constraints

- Qt 6 only. Single out-of-tree build dir: `cmake -B build -S .`, `cmake --build build`, `ctest --test-dir build`. A change is not verified until that is green (`AGENTS.md` §3).
- The app holds **no private key** and does **no OpenPGP**. If a task seems to need GPGME or Sequoia, it is the wrong task.
- `allowPickupFallback` is true only after the user confirms a dialog naming the keyless recipients.
- "Couldn't reach bootstrap" is never "no PGP" — unknown hides the controls rather than guessing a custody mode.
- Encrypt does **not** require the account to have a PGP identity; Sign does (server: `server.go:1199-1208`).
- The handoff opens the system browser via `QDesktopServices::openUrl` — never an embedded web view.
- `PgpComposeState` must behave identically to Android's `pgpComposeStateOf`. Two named pure functions with matching tests are what hold the clients together; prose does not.

## File Structure

| File | Responsibility |
|---|---|
| `core/net/RelayMailSource.{h,cpp}` | Send flags; the keyless-409 fields on `SendMailResult` |
| `core/net/PgpBootstrapClient.{h,cpp}` (new) | `GET /api/pgp/bootstrap` |
| `core/net/PgpRecipientChecker.{h,cpp}` (new) | `POST /api/pgp/recipients/check` |
| `core/domain/PgpComposeState.{h,cpp}` (new) | Pure function: bootstrap → which controls exist; plus the send decision |
| `app/mail/MailController.{h,cpp}` | Orchestrates preflight → gate → send or handoff |
| `app/qml/pages/Compose.qml` | Toggles, confirm dialog, handoff button |
| `tests/CMakeLists.txt` | Registers the new tests |

---

### Task 1: Send flags and parse the keyless 409

**Files:**
- Modify: `core/net/RelayMailSource.h:88-112` (`SendMailResult`), `:195-198` (`sendMail`), and the matching `RelayMailSource.cpp` request builder and response parser
- Test: `tests/core/net/RelayMailSourceTest.cpp`

**Interfaces:**
- Consumes: nothing
- Produces: `sendMail(..., bool sign, bool encrypt, bool allowPickupFallback)`; `SendMailResult::pickupFallbackNeeded` and `SendMailResult::keylessRecipients` (`QStringList`)

`SendMailResult::clientSideNeeded` already exists and is documented as unreachable from today's compose UI (`RelayMailSource.h:100-110`). That comment stops being true in this plan — update it.

`saveDraft` keeps its current signature: the flags have no meaning for a draft, and the server's draft handler ignores them.

- [ ] **Step 1: Write the failing test**

Append to `tests/core/net/RelayMailSourceTest.cpp`, following the `FakeRelayServer` pattern already used there:

```cpp
void RelayMailSourceTest::sendMailPutsPgpFlagsInTheBody()
{
    FakeRelayServer server;
    server.enqueue(200, QByteArrayLiteral(R"({"ok":true,"sentSaved":true,"warning":""})"));
    RelayMailSource source(server.httpClient());

    source.sendMail(server.baseUrl(), testAuth(), QStringLiteral("bob@example.com"), QString(), QString(),
                    QStringLiteral("hi"), QStringLiteral("hello"), QStringLiteral("plain"), {},
                    /*sign=*/true, /*encrypt=*/true, /*allowPickupFallback=*/true);

    const QByteArray body = server.lastRequestBody();
    QVERIFY(body.contains("\"sign\":true"));
    QVERIFY(body.contains("\"encrypt\":true"));
    QVERIFY(body.contains("\"allowPickupFallback\":true"));
}

void RelayMailSourceTest::sendMailParsesKeylessRecipient409()
{
    FakeRelayServer server;
    server.enqueue(409, QByteArrayLiteral(
        R"({"error":"no key","keylessRecipients":["carol@example.com"],"pickupFallbackAvailable":true})"));
    RelayMailSource source(server.httpClient());

    const SendMailResult result = source.sendMail(
        server.baseUrl(), testAuth(), QStringLiteral("carol@example.com"), QString(), QString(),
        QStringLiteral("hi"), QStringLiteral("hello"), QStringLiteral("plain"), {},
        /*sign=*/false, /*encrypt=*/true, /*allowPickupFallback=*/false);

    QVERIFY(result.pickupFallbackNeeded);
    QCOMPARE(result.keylessRecipients, QStringList{QStringLiteral("carol@example.com")});
    // The two PGP refusals share a status and are told apart by field.
    QVERIFY(!result.clientSideNeeded);
}

void RelayMailSourceTest::clientSideNeeded409TakesPrecedence()
{
    FakeRelayServer server;
    server.enqueue(409, QByteArrayLiteral(R"({"error":"e2e","clientSideNeeded":true})"));
    RelayMailSource source(server.httpClient());

    const SendMailResult result = source.sendMail(
        server.baseUrl(), testAuth(), QStringLiteral("bob@example.com"), QString(), QString(),
        QStringLiteral("hi"), QStringLiteral("hello"), QStringLiteral("plain"), {},
        /*sign=*/false, /*encrypt=*/true, /*allowPickupFallback=*/false);

    QVERIFY(result.clientSideNeeded);
    QVERIFY(!result.pickupFallbackNeeded);
}
```

Declare all three in the class's `private slots:` block. Match `FakeRelayServer`'s real API — read `tests/core/net/FakeRelayServer.h` first; the enqueue/lastRequestBody names above are illustrative and must be replaced with whatever it actually exposes.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R RelayMailSourceTest --output-on-failure`
Expected: FAIL — `sendMail` takes no such arguments.

- [ ] **Step 3: Implement**

`RelayMailSource.h`, in `SendMailResult`:

```cpp
    // Set when the backend refused with 409 + `keylessRecipients`: one or more
    // recipients have no usable PGP key, and the server refused rather than
    // quietly falling back to a one-time link that stores this message's
    // plaintext for seven days. Recoverable -- re-send with
    // allowPickupFallback once the user has confirmed. Distinct from
    // clientSideNeeded above, which is the same status for a different reason.
    bool pickupFallbackNeeded = false;
    QStringList keylessRecipients;
```

Add the three `bool` parameters to `sendMail`, emit them in the JSON body, and parse the new 409 shape in the response handler. Check `clientSideNeeded` **first** so precedence matches the server's. Update the now-stale comment at `RelayMailSource.h:106-109` that says these flags are unreachable from compose.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R RelayMailSourceTest --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add core/net/RelayMailSource.h core/net/RelayMailSource.cpp tests/core/net/RelayMailSourceTest.cpp
git commit -m "feat(pgp): send sign/encrypt/allowPickupFallback and parse the keyless 409"
```

---

### Task 2: `PgpComposeState` and the send decision

**Files:**
- Create: `core/domain/PgpComposeState.h`, `core/domain/PgpComposeState.cpp`
- Create: `tests/core/domain/PgpComposeStateTest.cpp`
- Modify: `tests/CMakeLists.txt`, and `core/CMakeLists.txt` (or wherever `core/domain` sources are listed)

**Interfaces:**
- Consumes: nothing
- Produces:
  - `struct PgpComposeState { bool canEncrypt; bool canSign; bool handoffToWebmail; }`
  - `PgpComposeState pgpComposeStateOf(std::optional<bool> hasIdentity, std::optional<QString> protection);`
  - `enum class SendGate { Send, SendWithFallback, Confirm };`
  - `SendGate sendGateFor(const QStringList& keylessRecipients, bool confirmed);`

This must match Android's `pgpComposeStateOf` case for case. Read `kypost-android/app/src/main/java/com/urlxl/mail/pgp/PgpComposeState.kt` before writing it.

- [ ] **Step 1: Write the failing test**

`tests/core/domain/PgpComposeStateTest.cpp`, data-driven in the style of `PgpMessageStateTest.cpp`:

```cpp
#include "domain/PgpComposeState.h"

#include <QTest>

class PgpComposeStateTest : public QObject
{
    Q_OBJECT

private slots:
    void controls_data();
    void controls();
    void sendGate_data();
    void sendGate();
};

void PgpComposeStateTest::controls_data()
{
    QTest::addColumn<std::optional<bool>>("hasIdentity");
    QTest::addColumn<std::optional<QString>>("protection");
    QTest::addColumn<bool>("canEncrypt");
    QTest::addColumn<bool>("canSign");
    QTest::addColumn<bool>("handoff");

    const std::optional<QString> unknown = std::nullopt;
    const std::optional<QString> server = QStringLiteral("server");
    const std::optional<QString> client = QStringLiteral("client");
    const std::optional<QString> none = QStringLiteral("");

    QTest::newRow("server custody with identity") << std::optional<bool>(true) << server << true << true << false;

    // Encrypting uses the recipients' public keys, so it works with no sender
    // identity at all. Only signing needs the account's own key. Gating both on
    // hasIdentity would deny encryption to an account that never made one.
    QTest::newRow("server custody, no identity") << std::optional<bool>(false) << server << true << false << false;

    QTest::newRow("client custody hands off") << std::optional<bool>(true) << client << false << false << true;

    // Couldn't check is not "no": guessing "server" offers a toggle that 409s,
    // guessing "client" sends people to webmail for nothing.
    QTest::newRow("unknown hides everything") << std::optional<bool>() << unknown << false << false << false;

    QTest::newRow("no pgp configured") << std::optional<bool>(false) << none << true << false << false;
}

void PgpComposeStateTest::controls()
{
    QFETCH(std::optional<bool>, hasIdentity);
    QFETCH(std::optional<QString>, protection);
    QFETCH(bool, canEncrypt);
    QFETCH(bool, canSign);
    QFETCH(bool, handoff);

    const PgpComposeState state = pgpComposeStateOf(hasIdentity, protection);

    QCOMPARE(state.canEncrypt, canEncrypt);
    QCOMPARE(state.canSign, canSign);
    QCOMPARE(state.handoffToWebmail, handoff);
}

void PgpComposeStateTest::sendGate_data()
{
    QTest::addColumn<QStringList>("keyless");
    QTest::addColumn<bool>("confirmed");
    QTest::addColumn<SendGate>("expected");

    QTest::newRow("nothing keyless") << QStringList{} << false << SendGate::Send;
    QTest::newRow("keyless, unconfirmed") << QStringList{QStringLiteral("c@e.com")} << false << SendGate::Confirm;
    QTest::newRow("keyless, confirmed") << QStringList{QStringLiteral("c@e.com")} << true << SendGate::SendWithFallback;

    // A failed preflight yields an empty list and therefore a plain Send.
    // Deliberate: a flaky lookup must not make mail unsendable, and because the
    // flag stays false the server's own 409 is still the gate -- so a failed
    // preflight can never be the reason the fallback gets used.
    QTest::newRow("failed preflight sends without the opt-in") << QStringList{} << true << SendGate::Send;
}

void PgpComposeStateTest::sendGate()
{
    QFETCH(QStringList, keyless);
    QFETCH(bool, confirmed);
    QFETCH(SendGate, expected);

    QCOMPARE(sendGateFor(keyless, confirmed), expected);
}

QTEST_MAIN(PgpComposeStateTest)
#include "PgpComposeStateTest.moc"
```

Check how `PgpMessageStateTest.cpp` ends (`QTEST_MAIN` vs `QTEST_APPLESS_MAIN`, moc include) and match it exactly.

- [ ] **Step 2: Register the test and run it**

In `tests/CMakeLists.txt`, beside line 60:

```cmake
kypost_add_test(PgpComposeStateTest core/domain/PgpComposeStateTest.cpp)
```

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build -R PgpComposeStateTest --output-on-failure`
Expected: FAIL — no such header.

- [ ] **Step 3: Implement**

`core/domain/PgpComposeState.h` with the struct, enum and two free functions, documented as above; `.cpp` with the bodies mirroring Android's `when` exactly. Add the new sources to whichever CMake target lists `core/domain/PgpMessageState.cpp`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R PgpComposeStateTest --output-on-failure`
Expected: PASS (all rows).

- [ ] **Step 5: Commit**

```bash
git add core/domain/PgpComposeState.h core/domain/PgpComposeState.cpp tests/core/domain/PgpComposeStateTest.cpp tests/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat(pgp): derive compose controls and the send gate from custody mode"
```

---

### Task 3: `PgpBootstrapClient`

**Files:**
- Create: `core/net/PgpBootstrapClient.{h,cpp}`
- Create: `tests/core/net/PgpBootstrapClientTest.cpp`
- Modify: `tests/CMakeLists.txt`, `core/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces: `struct PgpBootstrapResult { std::optional<NetworkError> error; QString detail; bool ok = false; bool hasIdentity = false; QString protection; }` and `PgpBootstrapResult fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const;`

Model on `core/net/PgpQrClient.{h,cpp}` — same `HttpClient&` constructor injection, same `NetworkError` handling. Read it first; matching it matters more than the shape sketched here.

- [ ] **Step 1: Write the failing test**

```cpp
void PgpBootstrapClientTest::parsesProtectionAndIdentity()
{
    FakeRelayServer server;
    server.enqueue(200, QByteArrayLiteral(R"({"hasIdentity":true,"protection":"client","unlockRequired":true})"));
    PgpBootstrapClient client(server.httpClient());

    const PgpBootstrapResult result = client.fetch(server.baseUrl(), testAuth());

    QVERIFY(result.ok);
    QVERIFY(result.hasIdentity);
    QCOMPARE(result.protection, QStringLiteral("client"));
}

// A failure must be distinguishable from a successful "no identity", or the
// compose screen cannot honor the couldn't-check-is-not-no rule.
void PgpBootstrapClientTest::failureIsNotAnEmptySuccess()
{
    FakeRelayServer server;
    server.enqueue(503, QByteArrayLiteral("unavailable"));
    PgpBootstrapClient client(server.httpClient());

    const PgpBootstrapResult result = client.fetch(server.baseUrl(), testAuth());

    QVERIFY(!result.ok);
}

// Bootstrap carries signerPublicKeys, payloadEndpoint and more this client has
// no use for; unknown fields must not break parsing.
void PgpBootstrapClientTest::ignoresUnknownFields()
{
    FakeRelayServer server;
    server.enqueue(200, QByteArrayLiteral(
        R"({"hasIdentity":false,"protection":"server","signerPublicKeys":[],"payloadEndpoint":"/x"})"));
    PgpBootstrapClient client(server.httpClient());

    const PgpBootstrapResult result = client.fetch(server.baseUrl(), testAuth());

    QVERIFY(result.ok);
    QCOMPARE(result.protection, QStringLiteral("server"));
}
```

- [ ] **Step 2: Register and run**

`kypost_add_test(PgpBootstrapClientTest core/net/PgpBootstrapClientTest.cpp)` in `tests/CMakeLists.txt`.

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build -R PgpBootstrapClientTest --output-on-failure`
Expected: FAIL — no such header.

- [ ] **Step 3: Implement**, following `PgpQrClient`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R PgpBootstrapClientTest --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add core/net/PgpBootstrapClient.h core/net/PgpBootstrapClient.cpp tests/core/net/PgpBootstrapClientTest.cpp tests/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat(pgp): fetch custody mode from /api/pgp/bootstrap"
```

---

### Task 4: `PgpRecipientChecker`

**Files:**
- Create: `core/net/PgpRecipientChecker.{h,cpp}`
- Create: `tests/core/net/PgpRecipientCheckerTest.cpp`
- Modify: `tests/CMakeLists.txt`, `core/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces: `struct RecipientKeyResult { std::optional<NetworkError> error; QString detail; bool ok = false; QStringList keylessRecipients; }` and `RecipientKeyResult check(const QUrl&, const RelayAuth&, const QStringList& addresses) const;`

**Endpoint: `POST /api/pgp/recipients/check`, not `resolve`.** `/api/pgp/recipients/resolve` is device-reachable but refuses with 409 for any account that is not client-protected (`pgp_resolve_handler.go:44-48`) — it hands real public keys to a browser doing its own encryption, which is precisely the account type this app does *not* build encrypted send for. `check` answers the yes/no question and needs the server plan's Task 7 to reach device auth. The class is named for the endpoint it calls so nobody has to rediscover this.

Request: `{"addresses": [...]}`. Response: a `results` array of `{address, hasKey, revoked, expired, tier}` (`pgp_keyserver.go:129-135`). Keyless means `hasKey == false`; the handler already folds revoked and expired into it (`pgp_keyserver.go:143`), so do not re-derive from those flags. Tiers are for the web UI's badges — do not model them.

**Expect false positives.** `check` reads only pinned contact keys, while the send path also runs WKD discovery, so it can name a recipient as keyless who turns out to have a key. Over-warning is the safe direction, and the 409 from Task 1 is the real gate.

- [ ] **Step 1: Write the failing test**

```cpp
void PgpRecipientCheckerTest::reportsRecipientsWithNoUsableKey()
{
    FakeRelayServer server;
    server.enqueue(200, QByteArrayLiteral(R"({"results":[
        {"address":"bob@example.com","hasKey":true,"revoked":false,"expired":false,"tier":"contact-verified"},
        {"address":"carol@example.com","hasKey":false,"revoked":false,"expired":false,"tier":"none"}
    ]})"));
    PgpRecipientChecker checker(server.httpClient());

    const RecipientKeyResult result = checker.check(
        server.baseUrl(), testAuth(),
        QStringList{QStringLiteral("bob@example.com"), QStringLiteral("carol@example.com")});

    QVERIFY(result.ok);
    QCOMPARE(result.keylessRecipients, QStringList{QStringLiteral("carol@example.com")});
}

// hasKey is already false for a revoked or expired key (the handler sets it
// from ks.Usable()), so a revoked contact is keyless without this client
// re-deriving anything from the revoked/expired flags.
void PgpRecipientCheckerTest::revokedKeyCountsAsKeyless()
{
    FakeRelayServer server;
    server.enqueue(200, QByteArrayLiteral(R"({"results":[
        {"address":"dave@example.com","hasKey":false,"revoked":true,"expired":false,"tier":"none"}
    ]})"));
    PgpRecipientChecker checker(server.httpClient());

    const RecipientKeyResult result = checker.check(
        server.baseUrl(), testAuth(), QStringList{QStringLiteral("dave@example.com")});

    QCOMPARE(result.keylessRecipients, QStringList{QStringLiteral("dave@example.com")});
}

// A failed preflight must not read as "everyone has a key" -- that would let
// the send proceed believing no fallback is involved.
void PgpRecipientCheckerTest::failureIsNotAnEmptyKeylessList()
{
    FakeRelayServer server;
    server.enqueue(500, QByteArrayLiteral("boom"));
    PgpRecipientChecker checker(server.httpClient());

    const RecipientKeyResult result = checker.check(
        server.baseUrl(), testAuth(), QStringList{QStringLiteral("bob@example.com")});

    QVERIFY(!result.ok);
}
```

Confirm the `results` wrapper key against the handler's final `writeJSON` call in `pgp_keyserver.go` before writing the parser — the field list above is from the struct, the wrapper name is set at the response site.

- [ ] **Step 2: Register and run to verify it fails**

`kypost_add_test(PgpRecipientCheckerTest core/net/PgpRecipientCheckerTest.cpp)`

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build -R PgpRecipientCheckerTest --output-on-failure`
Expected: FAIL — no such header.

- [ ] **Step 3: Implement, then run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R PgpRecipientCheckerTest --output-on-failure`
Expected: PASS (all three).

- [ ] **Step 4: Commit**

```bash
git add core/net/PgpRecipientChecker.h core/net/PgpRecipientChecker.cpp tests/core/net/PgpRecipientCheckerTest.cpp tests/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat(pgp): check recipient key status before an encrypted send"
```

---

### Task 5: `MailController` orchestration

**Files:**
- Modify: `app/mail/MailController.h:136-148`, `app/mail/MailController.cpp`

**Interfaces:**
- Consumes: Tasks 1-4
- Produces, all `Q_INVOKABLE` for QML:
  - `bool sendMail(to, cc, bcc, subject, body, attachmentFilePaths, bool sign, bool encrypt, bool allowPickupFallback)`
  - `void refreshPgpComposeState()` plus `Q_PROPERTY`s `pgpCanEncrypt`, `pgpCanSign`, `pgpHandoffToWebmail`
  - `void preflightRecipients(const QString& to, const QString& cc, const QString& bcc)` plus `Q_PROPERTY QStringList pgpKeylessRecipients`
  - `bool openWebmailDrafts()` — saves the draft, then opens the browser
  - signal `void pickupFallbackRequired(const QStringList& recipients)`

- [ ] **Step 1: Extend `sendMail` and emit the gate signal**

`sendMail` consults `sendGateFor(m_keylessRecipients, confirmed)` from Task 2. On `Confirm` it emits `pickupFallbackRequired` and returns false without calling the network. On `Send`/`SendWithFallback` it calls `RelayMailSource::sendMail` with the matching flag.

Handle both 409s from the result: `pickupFallbackNeeded` emits `pickupFallbackRequired` with the server's list (which may name a recipient typed after the last preflight); `clientSideNeeded` sets the handoff state.

Surface `SendMailResult::warning` when non-empty rather than treating `ok` as unqualified success.

- [ ] **Step 2: Implement `openWebmailDrafts`**

```cpp
// Saves the composition to Drafts, then opens webmail there in the user's
// browser. The URL targets the mailbox, not one draft: POST /api/mail/draft
// answers with a bare {ok: true} and no UID, so there is nothing to link to.
//
// Returns false without opening anything if the draft did not save. Opening a
// browser onto a draft that is not there loses the user's message.
bool MailController::openWebmailDrafts(/* same args as saveDraft */)
{
    if (!saveDraft(/* ... */)) {
        return false;
    }
    QUrl url(m_serverBaseUrl);
    url.setPath(QStringLiteral("/read"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("mailbox"), QStringLiteral("Drafts"));
    url.setQuery(query);
    return QDesktopServices::openUrl(url);
}
```

- [ ] **Step 3: Build and run the full suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add app/mail/MailController.h app/mail/MailController.cpp
git commit -m "feat(pgp): orchestrate the encrypted-send gate and webmail handoff"
```

---

### Task 6: Compose UI

**Files:**
- Modify: `app/qml/pages/Compose.qml:130-190`

**Interfaces:**
- Consumes: Task 5's properties, invokables and signal

- [ ] **Step 1: Add the toggles**

Two checkboxes bound to `MailApp.pgpCanEncrypt` / `MailApp.pgpCanSign` for visibility. When `MailApp.pgpHandoffToWebmail` is true, show a "Continue in webmail" button in their place instead.

- [ ] **Step 2: Preflight on recipient change**

Call `MailApp.preflightRecipients(...)` on recipient-field edits, debounced with a `Timer`, and only while the encrypt toggle is on. Render `MailApp.pgpKeylessRecipients` as an inline warning.

- [ ] **Step 3: Add the confirm dialog**

On `pickupFallbackRequired`, open a `Dialog` naming the addresses and stating what the fallback costs: stored on the server in plaintext for 7 days, the link travels as ordinary unencrypted mail, anyone holding the link can read it once. Accepting re-calls `MailApp.sendMail(...)` with `allowPickupFallback = true`. Rejecting closes the dialog and leaves the composition untouched.

- [ ] **Step 4: Wire the handoff button**

Calls `MailApp.openWebmailDrafts(...)`. On false, show the error and keep the compose page open — do not navigate away.

- [ ] **Step 5: Build and verify**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add app/qml/pages/Compose.qml
git commit -m "feat(pgp): offer encrypted send and the webmail handoff in compose"
```

---

## Manual verification

Against a real paired server — none of the tests touch a real IMAP or SMTP server:

- [ ] Server-custody account, recipient with a key in Contacts → arrives encrypted.
- [ ] Server-custody, one keyless recipient → dialog names them; cancel keeps the composition; confirm delivers and the keyless recipient gets a working link.
- [ ] Server-custody account with no PGP identity → Encrypt is offered, Sign is not.
- [ ] Client-custody account → toggles replaced by "Continue in webmail"; clicking it saves the draft and opens the system browser at Drafts.
- [ ] Server unreachable at compose open → no PGP controls appear, plain send still works.
- [ ] Repeat on macOS (kypost-for-Mac): `QDesktopServices::openUrl` and the Drafts mailbox name are the two places the platforms could diverge.
