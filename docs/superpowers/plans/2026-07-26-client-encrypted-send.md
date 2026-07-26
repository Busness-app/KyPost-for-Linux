# Client Encrypted Send Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a server-custody account send signed and encrypted mail from this client, confirm the plaintext-link fallback per message, and hand a client-custody account off to webmail instead of failing.

**Architecture:** This client does no OpenPGP. It sets `sign`/`encrypt`/`allowPickupFallback` on `POST /api/mail/send` and the server does the crypto. Custody mode comes from `GET /api/pgp/bootstrap`; a non-blocking preflight warning comes from `POST /api/pgp/recipients/check`. The server's `409 + keylessRecipients` — not the preflight — is what triggers the confirmation dialog, and confirming re-sends a **cached, byte-identical** request with the fallback flag flipped. Client-custody compose saves a draft and opens webmail via `QDesktopServices`.

**Tech Stack:** Qt 6 only, C++20, QtTest with the real-socket `FakeRelayServer` harness, ctest.

**Spec:** `Client_Encrypted_Send.md` (repo root).

**Supersedes:** `docs/superpowers/plans/2026-07-19-llama-naming-cleanup.md` is unrelated; this plan supersedes `docs/superpowers/plans/2026-07-25-mobile-encrypted-send.md`, which was written before the spec and disagrees with it in three places (see "Deviations" at the bottom). Do not execute both.

**Server dependency:** every endpoint here lives on `kypost-server` branch `feat/mobile-encrypted-send`, which is **not merged and not deployed**. Unit tests in this plan use a fake socket and pass regardless. Manual verification cannot happen until that branch lands. If a real call 404s, check that before assuming a client bug.

## Global Constraints

- Qt 6 only. One out-of-tree build dir: `cmake -B build -S .`, `cmake --build build`, `ctest --test-dir build`. A change is not verified until that is green (`AGENTS.md` §3).
- This client **never holds the account's private key** and does **no OpenPGP**. If a task seems to need GPGME, Sequoia, or a PBKDF2 key unwrap, it is the wrong task — an earlier superseded design called for exactly that.
- **Do not build:** any call to `POST /api/pgp/pickup` (browser-sealed path, 409s for server custody), any call to `POST /api/mail/send-pgp` (takes a pre-built ciphertext this client never has), any call to `POST /api/pgp/recipients/resolve` (409s for every account that is not client-protected — use `check`), or a remembered "always allow pickup fallback" preference (the opt-in must reset on every compose; that was a specific review finding).
- `allowPickupFallback` is true **only** on a re-send after the user confirmed a dialog naming the exact keyless addresses.
- **Discriminate the two 409s by field, never by status code or error prose.** The prose is user-facing copy and may be reworded; `clientSideNeeded` and `keylessRecipients` are the contract. Check `clientSideNeeded` first, matching the server's own ordering (`server.go:1207` precedes `:1272`).
- **Sign requires a PGP identity; Encrypt does not.** Verified at `kypost-server/backend/internal/api/server.go:1214-1223`: with no server-readable key, `signer` stays nil and only `req.Sign` produces a 400. Encryption uses the *recipients'* public keys. Gating Encrypt on `hasIdentity` would deny encryption to an account that never made a key.
- **The preflight is a lower bound, not a prediction.** `check` reads only the user's contacts, while the send path additionally runs the WKD/keyserver discovery ladder, so an address reported keyless may still be encrypted to successfully. Warn ("we don't have a key on file for X"), never promise ("this will be sent as a plaintext link").
- **A failed or unreachable bootstrap is never "no PGP".** Unknown custody hides the controls rather than guessing.
- The pickup link stores this message's plaintext on the server for **7 days**, server-readable. Every pickup link this client can cause is that kind — the sealed variant is gated to client-custody accounts, which cannot send from here at all. The confirmation copy must say so; softening it defeats the entire opt-in.
- The confirmation dialog copy in Task 7 is **contract, not a suggestion**. It carries the security property.
- The webmail handoff opens the system browser via `QDesktopServices::openUrl` — never an embedded web view, which shares no session and would put an account-password field inside this app.
- Every endpoint is `withMailAuth` (verified: `server.go:452`, `:468`), so the existing `X-Kypost-Device-Id`/`X-Kypost-Device-Secret` pairing headers are first-class. There is no desktop login and no session cookie.
- Body format: the two 409s and the 200 are JSON; **every other error status returns a plain-text body**. Keep the 409 decode best-effort — a non-JSON body must fall through to status-code wording, not crash.

## File Structure

| File | Responsibility |
|---|---|
| `core/net/RelayMailSource.{h,cpp}` | The three send flags; both 409 shapes on `SendMailResult` |
| `core/net/PgpBootstrapClient.{h,cpp}` (new) | `GET /api/pgp/bootstrap` — two fields, everything else ignored |
| `core/net/PgpRecipientChecker.{h,cpp}` (new) | `POST /api/pgp/recipients/check` |
| `core/domain/PgpComposeState.{h,cpp}` (new) | Pure function: bootstrap response → which compose controls exist |
| `app/mail/PgpMessagePresentation.{h,cpp}` | Adds `webmailMailboxUrl()`, sharing the existing https validation |
| `app/mail/MailController.{h,cpp}` | Preflight, send, the pending-send cache, the re-send, the handoff |
| `app/qml/pages/Compose.qml` | Toggles, inline warning, confirm dialog, handoff button |
| `tests/CMakeLists.txt` | Registers three new tests |

**Harness facts, verified — the old plan got these wrong.** `tests/core/net/FakeRelayServer.h` has no `enqueue`/`baseUrl`/`httpClient`/`lastRequestBody`. Its real API is:

```cpp
FakeRelayServer fake(httpResponse(409, "Conflict", body));  // canned response in the ctor
fake.port();                 // quint16
fake.receivedRequest();      // const QByteArray& — raw request
fake.receivedJsonBody();     // QJsonObject — parsed body
```

It **accepts exactly one connection**, so a test covering a send *and* a re-send needs two `FakeRelayServer` instances on two ports. Callers build their own `QNetworkAccessManager`, `HttpClient`, base URL and `RelayAuth`; copy the shape from `tests/core/net/RelayMailSourceTest.cpp:327-348`.

---

### Task 1: Send flags and both 409 shapes

**Files:**
- Modify: `core/net/RelayMailSource.h` (`SendMailResult` at `:93-112`, `sendMail` at `:195-197`), `core/net/RelayMailSource.cpp` (`sendMail` at `:229-264`)
- Test: `tests/core/net/RelayMailSourceTest.cpp`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `SendMailResult sendMail(const QUrl&, const RelayAuth&, const QString& to, const QString& cc, const QString& bcc, const QString& subject, const QString& body, const QString& mode, const QVector<MailAttachmentUpload>& attachments, bool sign = false, bool encrypt = false, bool allowPickupFallback = false) const;`
  - `SendMailResult::pickupFallbackNeeded` (`bool`) and `SendMailResult::keylessRecipients` (`QStringList`)

The three flags get **default arguments** so the existing `MailController` call site and the three existing `sendMail` tests keep compiling; only Task 6 passes them explicitly.

`saveDraft` keeps its signature — the flags are meaningless for a draft and the server's draft handler ignores them. The shared `mailRequestBody()` helper (used by both) must therefore stay untouched: insert the three flags into the object *after* `mailRequestBody()` returns, inside `sendMail` only.

- [ ] **Step 1: Write the failing tests**

Add these four declarations to the `private slots:` block in `tests/core/net/RelayMailSourceTest.cpp`, after `sendMailParsesAlwaysPresentWarningField();`:

```cpp
    void sendMailPutsPgpFlagsInTheRequestBody();
    void sendMailParsesKeylessRecipient409();
    void sendMailClientSideNeeded409IsNotAKeylessRefusal();
    void sendMail409WithNeitherPgpFieldStaysAGenericError();
```

Then append the bodies:

```cpp
void RelayMailSourceTest::sendMailPutsPgpFlagsInTheRequestBody()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true,"warning":""})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    source.sendMail(serverBaseUrl, auth, QStringLiteral("a@example.com"), QString(), QString(),
                    QStringLiteral("Hi"), QStringLiteral("Body"), QStringLiteral("plain"), {},
                    /*sign=*/true, /*encrypt=*/true, /*allowPickupFallback=*/true);

    const QJsonObject body = fake.receivedJsonBody();
    QCOMPARE(body.value(QStringLiteral("sign")).toBool(), true);
    QCOMPARE(body.value(QStringLiteral("encrypt")).toBool(), true);
    QCOMPARE(body.value(QStringLiteral("allowPickupFallback")).toBool(), true);
}

void RelayMailSourceTest::sendMailParsesKeylessRecipient409()
{
    // Nothing was delivered: the server refuses before any SMTP, which is why
    // re-sending with the opt-in cannot duplicate the message.
    FakeRelayServer fake(httpResponse(
        409, "Conflict",
        R"({"error":"some recipients have no usable PGP key","keylessRecipients":["bob@example.com"],)"
        R"("pickupFallbackAvailable":true})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const SendMailResult result =
        source.sendMail(serverBaseUrl, auth, QStringLiteral("bob@example.com"), QString(), QString(),
                        QStringLiteral("Hi"), QStringLiteral("Body"), QStringLiteral("plain"), {},
                        /*sign=*/false, /*encrypt=*/true, /*allowPickupFallback=*/false);

    QCOMPARE(result.ok, false);
    QCOMPARE(result.pickupFallbackNeeded, true);
    QCOMPARE(result.keylessRecipients, QStringList{ QStringLiteral("bob@example.com") });
    // The two PGP refusals share a status code and are told apart by field.
    QCOMPARE(result.clientSideNeeded, false);
}

void RelayMailSourceTest::sendMailClientSideNeeded409IsNotAKeylessRefusal()
{
    FakeRelayServer fake(httpResponse(
        409, "Conflict",
        R"({"error":"this account's PGP key is end-to-end protected","clientSideNeeded":true})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const SendMailResult result =
        source.sendMail(serverBaseUrl, auth, QStringLiteral("bob@example.com"), QString(), QString(),
                        QStringLiteral("Hi"), QStringLiteral("Body"), QStringLiteral("plain"), {},
                        /*sign=*/false, /*encrypt=*/true, /*allowPickupFallback=*/false);

    QCOMPARE(result.clientSideNeeded, true);
    QCOMPARE(result.pickupFallbackNeeded, false);
    QVERIFY(result.keylessRecipients.isEmpty());
}

void RelayMailSourceTest::sendMail409WithNeitherPgpFieldStaysAGenericError()
{
    // A 409 that is neither PGP refusal must not inherit PGP wording or set
    // either flag -- otherwise an unrelated future conflict would open the
    // plaintext-link dialog.
    FakeRelayServer fake(httpResponse(409, "Conflict", R"({"error":"something else entirely"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const SendMailResult result =
        source.sendMail(serverBaseUrl, auth, QStringLiteral("bob@example.com"), QString(), QString(),
                        QStringLiteral("Hi"), QStringLiteral("Body"), QStringLiteral("plain"), {},
                        /*sign=*/false, /*encrypt=*/true, /*allowPickupFallback=*/false);

    QCOMPARE(result.pickupFallbackNeeded, false);
    QCOMPARE(result.clientSideNeeded, false);
    QCOMPARE(result.detail, QStringLiteral("something else entirely"));
}
```

The existing decode is already best-effort, but nothing pins that down — add a fifth test so a future refactor cannot regress it into a crash. Declare it alongside the other four:

```cpp
    void sendMailMalformed409BodyDoesNotCrashTheDecode();
```

```cpp
void RelayMailSourceTest::sendMailMalformed409BodyDoesNotCrashTheDecode()
{
    FakeRelayServer fake(httpResponse(409, "Conflict", "not json at all", "text/plain"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    RelayMailSource source(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const SendMailResult result =
        source.sendMail(serverBaseUrl, auth, QStringLiteral("bob@example.com"), QString(), QString(),
                        QStringLiteral("Hi"), QStringLiteral("Body"), QStringLiteral("plain"), {},
                        /*sign=*/false, /*encrypt=*/true, /*allowPickupFallback=*/false);

    QCOMPARE(result.pickupFallbackNeeded, false);
    QCOMPARE(result.clientSideNeeded, false);
    QVERIFY(!result.detail.isEmpty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R RelayMailSourceTest --output-on-failure`
Expected: FAIL to compile — `sendMail` takes no such arguments and `SendMailResult` has no `pickupFallbackNeeded`.

- [ ] **Step 3: Implement**

In `core/net/RelayMailSource.h`, inside `SendMailResult` after `clientSideNeeded`:

```cpp
    // Set when the backend refused with 409 + `keylessRecipients`: at least
    // one recipient has no usable PGP key, and the server refused rather than
    // quietly falling back to a one-time link that stores this message's
    // plaintext server-side for seven days.
    //
    // Nothing was delivered -- the refusal happens before any SMTP
    // (server.go:1272), so re-sending the identical request with
    // allowPickupFallback set is safe and cannot duplicate. Distinct from
    // clientSideNeeded above, which is the same status for a different and
    // unrecoverable reason.
    bool pickupFallbackNeeded = false;
    QStringList keylessRecipients;
```

Fix the now-false comment at `:107-110` ("Not reachable from today's Compose UI, which sends no sign/encrypt flags") — as of this task it is reachable.

Add the three defaulted parameters to the `sendMail` declaration and definition. In the `.cpp`, after `mailRequestBody(...)` returns:

```cpp
    QJsonObject requestBody = mailRequestBody(to, cc, bcc, subject, body, mode, attachments);
    // Inserted here rather than inside mailRequestBody() because saveDraft()
    // shares that helper and the draft handler ignores these fields.
    requestBody.insert(QStringLiteral("sign"), sign);
    requestBody.insert(QStringLiteral("encrypt"), encrypt);
    requestBody.insert(QStringLiteral("allowPickupFallback"), allowPickupFallback);
```

In the error branch, extend the existing best-effort decode (currently `:252-255`):

```cpp
        if (errorJson.has_value()) {
            bodyError = errorJson->value(QStringLiteral("error")).toString();
            // Order matches the server's own: a client-custody account is
            // refused at server.go:1207, before the keyless gate at :1272, so
            // the two never arrive together. Checking clientSideNeeded first
            // means a future server that did send both still reports the
            // unrecoverable one.
            out.clientSideNeeded = errorJson->value(QStringLiteral("clientSideNeeded")).toBool();
            if (!out.clientSideNeeded) {
                const QJsonArray keyless = errorJson->value(QStringLiteral("keylessRecipients")).toArray();
                for (const QJsonValue& value : keyless) {
                    const QString address = value.toString();
                    if (!address.isEmpty())
                        out.keylessRecipients.append(address);
                }
                // Driven by the field's presence, not by pickupFallbackAvailable:
                // the server sets that to a constant true, so treating it as the
                // trigger would add a dependency on a value that carries no
                // information.
                out.pickupFallbackNeeded = !out.keylessRecipients.isEmpty();
            }
        }
```

Add `#include <QJsonArray>` if it is not already there.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R RelayMailSourceTest --output-on-failure`
Expected: PASS, all tests including the three pre-existing `sendMail*` ones.

- [ ] **Step 5: Commit**

```bash
git add core/net/RelayMailSource.h core/net/RelayMailSource.cpp tests/core/net/RelayMailSourceTest.cpp
git commit -m "feat(pgp): send sign/encrypt/allowPickupFallback and parse both 409 refusals"
```

---

### Task 2: `PgpComposeState` — which controls exist

**Files:**
- Create: `core/domain/PgpComposeState.h`, `core/domain/PgpComposeState.cpp`
- Create: `tests/core/domain/PgpComposeStateTest.cpp`
- Modify: `core/CMakeLists.txt` (beside `domain/PgpMessageState.cpp`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `struct PgpComposeState { bool canEncrypt = false; bool canSign = false; bool handoffToWebmail = false; };`
  - `PgpComposeState pgpComposeStateOf(std::optional<bool> hasIdentity, std::optional<QString> protection);`

Pure, no Qt beyond `QString`/`std::optional`, so it belongs in `core/domain` next to `PgpMessageState`. **There is no `SendGate`** — see Deviations: the confirmation is driven by the server's 409, not by the preflight, so no client-side gate function is needed.

The full truth table this must implement:

| `hasIdentity` | `protection` | canEncrypt | canSign | handoff | Why |
|---|---|---|---|---|---|
| any | `nullopt` (bootstrap failed) | false | false | false | Couldn't check is not "no" |
| true | `"server"` | true | true | false | The server can sign and encrypt |
| false | `"server"` | true | false | false | Contradictory response; encrypt still works, signing cannot |
| true | `"client"` | false | false | true | Key is in the browser only |
| false | `""` | true | false | false | No identity: encrypt to recipients' keys, cannot sign |
| any | `"anything-else"` | false | false | false | Unknown custody: degrade rather than guess |

- [ ] **Step 1: Write the failing test**

`tests/core/domain/PgpComposeStateTest.cpp`. Written as direct assertions rather than `QTest::addColumn`, because `std::optional<QString>` columns would each need a `Q_DECLARE_METATYPE` and the table above is more readable inline:

```cpp
#include "domain/PgpComposeState.h"

#include <QTest>

class PgpComposeStateTest : public QObject
{
    Q_OBJECT

private slots:
    void serverCustodyWithIdentityOffersBoth();
    void encryptDoesNotRequireAnIdentityButSignDoes();
    void clientCustodyHandsOffInsteadOfOfferingToggles();
    void unreachableBootstrapHidesEverything();
    void unrecognizedProtectionDegradesRatherThanGuessing();
};

void PgpComposeStateTest::serverCustodyWithIdentityOffersBoth()
{
    const PgpComposeState state = pgpComposeStateOf(true, QStringLiteral("server"));

    QCOMPARE(state.canEncrypt, true);
    QCOMPARE(state.canSign, true);
    QCOMPARE(state.handoffToWebmail, false);
}

// Encrypting uses the RECIPIENTS' public keys, so it works with no sender
// identity at all; only signing needs the account's own key. Verified against
// kypost-server server.go:1214-1223, where a missing server-readable key
// 400s only when req.Sign is set. Gating both on hasIdentity would deny
// encryption to an account that never made a key.
void PgpComposeStateTest::encryptDoesNotRequireAnIdentityButSignDoes()
{
    const PgpComposeState noIdentity = pgpComposeStateOf(false, QStringLiteral(""));

    QCOMPARE(noIdentity.canEncrypt, true);
    QCOMPARE(noIdentity.canSign, false);
    QCOMPARE(noIdentity.handoffToWebmail, false);

    // Contradictory but defensive: protection "server" implies a key exists,
    // so hasIdentity false should not happen. Signing still must not be
    // offered on the strength of the protection value alone.
    const PgpComposeState serverButNoKey = pgpComposeStateOf(false, QStringLiteral("server"));

    QCOMPARE(serverButNoKey.canEncrypt, true);
    QCOMPARE(serverButNoKey.canSign, false);
}

void PgpComposeStateTest::clientCustodyHandsOffInsteadOfOfferingToggles()
{
    const PgpComposeState state = pgpComposeStateOf(true, QStringLiteral("client"));

    QCOMPARE(state.canEncrypt, false);
    QCOMPARE(state.canSign, false);
    QCOMPARE(state.handoffToWebmail, true);
}

// Guessing "server" offers a toggle that 409s; guessing "client" sends people
// to webmail for nothing. Neither is acceptable, so an unreachable bootstrap
// shows no PGP controls and plain send keeps working.
void PgpComposeStateTest::unreachableBootstrapHidesEverything()
{
    const PgpComposeState state = pgpComposeStateOf(std::nullopt, std::nullopt);

    QCOMPARE(state.canEncrypt, false);
    QCOMPARE(state.canSign, false);
    QCOMPARE(state.handoffToWebmail, false);
}

void PgpComposeStateTest::unrecognizedProtectionDegradesRatherThanGuessing()
{
    const PgpComposeState state = pgpComposeStateOf(true, QStringLiteral("some-future-mode"));

    QCOMPARE(state.canEncrypt, false);
    QCOMPARE(state.canSign, false);
    QCOMPARE(state.handoffToWebmail, false);
}

QTEST_APPLESS_MAIN(PgpComposeStateTest)
#include "PgpComposeStateTest.moc"
```

Before running, open `tests/core/domain/PgpMessageStateTest.cpp` and confirm its final two lines use `QTEST_APPLESS_MAIN` and a `.moc` include; match whichever it uses exactly.

- [ ] **Step 2: Register the test and run it to verify it fails**

In `tests/CMakeLists.txt`, beside the existing `kypost_add_test(PgpMessageStateTest core/domain/PgpMessageStateTest.cpp)`:

```cmake
kypost_add_test(PgpComposeStateTest core/domain/PgpComposeStateTest.cpp)
```

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build -R PgpComposeStateTest --output-on-failure`
Expected: FAIL — `domain/PgpComposeState.h` does not exist.

- [ ] **Step 3: Implement**

`core/domain/PgpComposeState.h`:

```cpp
#pragma once

#include <QString>
#include <optional>

// Which PGP controls a compose screen may offer, derived from
// GET /api/pgp/bootstrap. Pure: no network, no Qt beyond QString, so it
// lives in core/domain beside PgpMessageState.
struct PgpComposeState
{
    bool canEncrypt = false;
    bool canSign = false;
    // Replace the toggles with a "continue in webmail" affordance: the
    // account's private key exists only in the user's browser, so no request
    // from this client can sign or encrypt.
    bool handoffToWebmail = false;
};

// Both arguments are optional because a failed bootstrap must be
// distinguishable from a successful "no identity" -- std::nullopt means
// "could not check", which hides every control rather than guessing a
// custody mode.
//
// `protection` values are the server's: "server", "client", "" (no
// identity). Anything else is treated as not-server and degrades to no
// controls.
PgpComposeState pgpComposeStateOf(std::optional<bool> hasIdentity, std::optional<QString> protection);
```

`core/domain/PgpComposeState.cpp`:

```cpp
#include "domain/PgpComposeState.h"

PgpComposeState pgpComposeStateOf(std::optional<bool> hasIdentity, std::optional<QString> protection)
{
    PgpComposeState state;
    if (!protection.has_value())
        return state;

    const QString mode = *protection;
    if (mode == QStringLiteral("client")) {
        state.handoffToWebmail = true;
        return state;
    }
    if (mode != QStringLiteral("server") && !mode.isEmpty())
        return state;

    // "server" or "" (no identity): encryption targets the recipients' keys
    // either way, so it is always available. Signing needs this account's own
    // key, which only "server" plus a confirmed identity provides.
    state.canEncrypt = true;
    state.canSign = mode == QStringLiteral("server") && hasIdentity.value_or(false);
    return state;
}
```

Add both sources to `core/CMakeLists.txt` beside `domain/PgpMessageState.cpp`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R PgpComposeStateTest --output-on-failure`
Expected: PASS, all five.

- [ ] **Step 5: Commit**

```bash
git add core/domain/PgpComposeState.h core/domain/PgpComposeState.cpp \
        tests/core/domain/PgpComposeStateTest.cpp tests/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat(pgp): derive compose controls from custody mode"
```

---

### Task 3: `PgpBootstrapClient`

**Files:**
- Create: `core/net/PgpBootstrapClient.h`, `core/net/PgpBootstrapClient.cpp`
- Create: `tests/core/net/PgpBootstrapClientTest.cpp`
- Modify: `core/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `struct PgpBootstrapResult { std::optional<NetworkError> error; QString detail; bool ok = false; bool hasIdentity = false; QString protection; };`
  - `class PgpBootstrapClient { explicit PgpBootstrapClient(HttpClient&); PgpBootstrapResult fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const; };`

Follow `core/net/PgpQrClient.{h,cpp}`: constructor takes `HttpClient&`, one method per endpoint, one small result struct, URL built with `joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/bootstrap"))` from `HttpClient.h`. Read `PgpQrClient.cpp` first — matching it matters more than the sketch here.

The real response carries `wrappedPrivateKey`, `unlockRequired`, `signerPublicKeys`, `payloadEndpoint`, `fingerprint`, `keyId`, `publicKey`, `keySource`, `createdAt`. **Parse only `hasIdentity` and `protection`.** The rest exist for the browser; reading them would create a dependency on fields this client must never use.

- [ ] **Step 1: Write the failing test**

`tests/core/net/PgpBootstrapClientTest.cpp`:

```cpp
#include "net/PgpBootstrapClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTest>

class PgpBootstrapClientTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesIdentityAndProtection();
    void ignoresTheBrowserOnlyFields();
    void failureIsNotAnEmptySuccess();
};

void PgpBootstrapClientTest::parsesIdentityAndProtection()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"hasIdentity":true,"protection":"client"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpBootstrapClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const PgpBootstrapResult result = client.fetch(serverBaseUrl, auth);

    QCOMPARE(result.ok, true);
    QCOMPARE(result.hasIdentity, true);
    QCOMPARE(result.protection, QStringLiteral("client"));
}

// bootstrap carries wrappedPrivateKey, unlockRequired, signerPublicKeys and
// more that exist for the browser. Unknown/unused fields must not break
// parsing, and nothing here may start depending on them.
void PgpBootstrapClientTest::ignoresTheBrowserOnlyFields()
{
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"hasIdentity":false,"protection":"server","wrappedPrivateKey":"xxx","unlockRequired":true,)"
        R"("signerPublicKeys":[],"payloadEndpoint":"/x","somethingAddedLater":42})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpBootstrapClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const PgpBootstrapResult result = client.fetch(serverBaseUrl, auth);

    QCOMPARE(result.ok, true);
    QCOMPARE(result.hasIdentity, false);
    QCOMPARE(result.protection, QStringLiteral("server"));
}

// A failure must be distinguishable from a successful "no identity", or the
// compose screen cannot honor "couldn't check is not no".
void PgpBootstrapClientTest::failureIsNotAnEmptySuccess()
{
    FakeRelayServer fake(httpResponse(503, "Service Unavailable", "unavailable", "text/plain"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpBootstrapClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const PgpBootstrapResult result = client.fetch(serverBaseUrl, auth);

    QCOMPARE(result.ok, false);
    QVERIFY(result.error.has_value());
}

QTEST_MAIN(PgpBootstrapClientTest)
#include "PgpBootstrapClientTest.moc"
```

Match `tests/core/net/PgpQrClientTest.cpp`'s final two lines for `QTEST_MAIN` vs `QTEST_APPLESS_MAIN`.

- [ ] **Step 2: Register and run to verify it fails**

```cmake
kypost_add_test(PgpBootstrapClientTest core/net/PgpBootstrapClientTest.cpp)
```

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build -R PgpBootstrapClientTest --output-on-failure`
Expected: FAIL — no such header.

- [ ] **Step 3: Implement**

`fetch` does `m_httpClient.get(joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/bootstrap")), {}, auth.headerItems())`, returns early with `error`/`detail` set when `result.error` has a value, otherwise decodes the body and sets `ok = true`, `hasIdentity`, `protection`. A body that fails to parse maps to `NetworkError::Decoding` with `ok` left false — copy how `PgpQrClient` does that rather than inventing a second convention.

Add both sources to `core/CMakeLists.txt` beside `net/PgpQrClient.cpp`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R PgpBootstrapClientTest --output-on-failure`
Expected: PASS, all three.

- [ ] **Step 5: Commit**

```bash
git add core/net/PgpBootstrapClient.h core/net/PgpBootstrapClient.cpp \
        tests/core/net/PgpBootstrapClientTest.cpp tests/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat(pgp): read custody mode from /api/pgp/bootstrap"
```

---

### Task 4: `PgpRecipientChecker`

**Files:**
- Create: `core/net/PgpRecipientChecker.h`, `core/net/PgpRecipientChecker.cpp`
- Create: `tests/core/net/PgpRecipientCheckerTest.cpp`
- Modify: `core/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `struct RecipientKeyCheckResult { std::optional<NetworkError> error; QString detail; bool ok = false; QStringList keylessRecipients; };`
  - `class PgpRecipientChecker { explicit PgpRecipientChecker(HttpClient&); RecipientKeyCheckResult check(const QUrl& serverBaseUrl, const RelayAuth& auth, const QStringList& addresses) const; };`

**Endpoint is `POST /api/pgp/recipients/check`, never `resolve`.** `resolve` is device-reachable but 409s for every account that is not client-protected (`pgp_resolve_handler.go`) — it hands real public keys to a browser doing its own encryption, the one account type this client cannot send encrypted mail for at all. The class is named for the endpoint so nobody rediscovers this.

Request body: `{"addresses": [...]}`. Response (verified `pgp_keyserver.go:129-151`): `{"results": [{"address", "hasKey", "revoked", "expired", "tier"}]}`. **Keyless means `hasKey == false` and nothing else** — the handler already sets `HasKey = ks.Usable()`, folding revoked and expired in (`:143`), so do not re-derive from those flags. Read them only to explain *why* if the UI ever wants to. `tier` is for the web UI's badges; do not model it.

- [ ] **Step 1: Write the failing test**

`tests/core/net/PgpRecipientCheckerTest.cpp`:

```cpp
#include "net/PgpRecipientChecker.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QTest>

class PgpRecipientCheckerTest : public QObject
{
    Q_OBJECT

private slots:
    void reportsOnlyTheAddressesWithNoUsableKey();
    void sendsTheAddressesInTheRequestBody();
    void revokedKeyIsAlreadyKeylessWithoutReDerivation();
    void failureIsNotAnEmptyKeylessList();
};

void PgpRecipientCheckerTest::reportsOnlyTheAddressesWithNoUsableKey()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"results":[
        {"address":"alice@example.com","hasKey":true,"revoked":false,"expired":false,"tier":"contact-verified"},
        {"address":"bob@example.com","hasKey":false,"revoked":false,"expired":false,"tier":"none"}
    ]})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const RecipientKeyCheckResult result = checker.check(
        serverBaseUrl, auth,
        QStringList{ QStringLiteral("alice@example.com"), QStringLiteral("bob@example.com") });

    QCOMPARE(result.ok, true);
    QCOMPARE(result.keylessRecipients, QStringList{ QStringLiteral("bob@example.com") });
}

void PgpRecipientCheckerTest::sendsTheAddressesInTheRequestBody()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"results":[]})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    checker.check(serverBaseUrl, auth,
                  QStringList{ QStringLiteral("alice@example.com"), QStringLiteral("bob@example.com") });

    const QJsonArray sent = fake.receivedJsonBody().value(QStringLiteral("addresses")).toArray();
    QCOMPARE(sent.size(), 2);
    QCOMPARE(sent.at(0).toString(), QStringLiteral("alice@example.com"));
    QCOMPARE(sent.at(1).toString(), QStringLiteral("bob@example.com"));
}

// hasKey is already false for a revoked or expired key -- the handler sets it
// from ks.Usable() (pgp_keyserver.go:143). A revoked contact is therefore
// keyless without this client deriving anything from revoked/expired.
void PgpRecipientCheckerTest::revokedKeyIsAlreadyKeylessWithoutReDerivation()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"results":[
        {"address":"dave@example.com","hasKey":false,"revoked":true,"expired":false,"tier":"none"}
    ]})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const RecipientKeyCheckResult result =
        checker.check(serverBaseUrl, auth, QStringList{ QStringLiteral("dave@example.com") });

    QCOMPARE(result.keylessRecipients, QStringList{ QStringLiteral("dave@example.com") });
}

// A failed preflight must not read as "everyone has a key". It is only an
// inline warning, so ok=false means "show nothing", never "all clear".
void PgpRecipientCheckerTest::failureIsNotAnEmptyKeylessList()
{
    FakeRelayServer fake(httpResponse(500, "Internal Server Error", "boom", "text/plain"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    PgpRecipientChecker checker(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const RecipientKeyCheckResult result =
        checker.check(serverBaseUrl, auth, QStringList{ QStringLiteral("alice@example.com") });

    QCOMPARE(result.ok, false);
    QVERIFY(result.keylessRecipients.isEmpty());
}

QTEST_MAIN(PgpRecipientCheckerTest)
#include "PgpRecipientCheckerTest.moc"
```

- [ ] **Step 2: Register and run to verify it fails**

```cmake
kypost_add_test(PgpRecipientCheckerTest core/net/PgpRecipientCheckerTest.cpp)
```

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build -R PgpRecipientCheckerTest --output-on-failure`
Expected: FAIL — no such header.

- [ ] **Step 3: Implement**

`check` builds `QJsonObject{{"addresses", QJsonArray::fromStringList(addresses)}}`, posts to `joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/recipients/check"))` with `auth.headerItems()`, and on success walks `results`, appending `address` wherever `hasKey` is false. Skip entries with an empty `address`.

Add a header comment recording the two traps (use `check` not `resolve`; the result is a lower bound because `check` is contacts-only while send also runs WKD discovery), so they survive without this plan.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R PgpRecipientCheckerTest --output-on-failure`
Expected: PASS, all four.

- [ ] **Step 5: Commit**

```bash
git add core/net/PgpRecipientChecker.h core/net/PgpRecipientChecker.cpp \
        tests/core/net/PgpRecipientCheckerTest.cpp tests/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat(pgp): warn early about recipients with no key on file"
```

---

### Task 5: `webmailMailboxUrl` — the handoff URL

**Files:**
- Modify: `app/mail/PgpMessagePresentation.h`, `app/mail/PgpMessagePresentation.cpp:73-103`
- Test: `tests/app/mail/PgpMessagePresentationTest.cpp`

**Interfaces:**
- Consumes: nothing
- Produces: `QUrl webmailMailboxUrl(const QUrl& serverBaseUrl, const QString& mailbox);`

The existing `webmailReadUrl()` cannot be reused directly: it returns an empty `QUrl` when `messageId` is empty, and the handoff has no message id — `POST /api/mail/draft` answers with a bare `{ok:true}` and no UID, so there is nothing to link to. Rather than writing a second scheme check, **extract the existing validation** into a file-local helper both functions call.

**Note the real rule is https-only**, stricter than "non-http(s) rejected": `:82` requires the scheme to be `https`. A pairing on plain `http` therefore yields an empty URL and the handoff button must report an error rather than silently doing nothing (Task 6 handles that). This is deliberate and matches the read-side affordance already shipped — do not loosen it here.

- [ ] **Step 1: Write the failing test**

Append to `tests/app/mail/PgpMessagePresentationTest.cpp`, declaring each in its `private slots:` block:

```cpp
void PgpMessagePresentationTest::webmailMailboxUrlTargetsTheMailboxWithNoMessageId()
{
    const QUrl url = webmailMailboxUrl(QUrl(QStringLiteral("https://mail.example.com")),
                                        QStringLiteral("Drafts"));

    QCOMPARE(url.toString(), QStringLiteral("https://mail.example.com/read?mailbox=Drafts"));
}

// Same containment rule as webmailReadUrl: this URL goes to an external
// browser, so a pairing holding a file://, javascript: or downgraded http
// base must never produce one.
void PgpMessagePresentationTest::webmailMailboxUrlRejectsAnythingButHttps()
{
    QVERIFY(webmailMailboxUrl(QUrl(QStringLiteral("http://mail.example.com")),
                              QStringLiteral("Drafts")).isEmpty());
    QVERIFY(webmailMailboxUrl(QUrl(QStringLiteral("file:///etc/passwd")),
                              QStringLiteral("Drafts")).isEmpty());
    QVERIFY(webmailMailboxUrl(QUrl(QStringLiteral("javascript:alert(1)")),
                              QStringLiteral("Drafts")).isEmpty());
    QVERIFY(webmailMailboxUrl(QUrl(), QStringLiteral("Drafts")).isEmpty());
    QVERIFY(webmailMailboxUrl(QUrl(QStringLiteral("https://")), QStringLiteral("Drafts")).isEmpty());
}

// A base URL carrying its own query or fragment must not leak into the link.
void PgpMessagePresentationTest::webmailMailboxUrlDropsAnyBaseQueryOrFragment()
{
    const QUrl url = webmailMailboxUrl(
        QUrl(QStringLiteral("https://mail.example.com/?tracking=1#section")), QStringLiteral("Drafts"));

    QCOMPARE(url.toString(), QStringLiteral("https://mail.example.com/read?mailbox=Drafts"));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R PgpMessagePresentationTest --output-on-failure`
Expected: FAIL to compile — `webmailMailboxUrl` is not declared.

- [ ] **Step 3: Implement**

In `PgpMessagePresentation.cpp`, above `webmailReadUrl`, add the shared helper and rewrite `webmailReadUrl` to use it:

```cpp
namespace {

// The one place the webmail base URL is vetted, shared by webmailReadUrl()
// and webmailMailboxUrl(). https-only and host-bearing: these URLs are handed
// to an external browser, so a pairing holding a file://, javascript: or
// otherwise degraded base must never reach one, and requiring https (not
// merely "not file") stops a downgraded pairing from putting a mailbox name
// or message id on the wire in the clear.
//
// Returns a URL with /read as the path and any inherited query/fragment
// cleared, ready for the caller to set its own query.
std::optional<QUrl> webmailReadBase(const QUrl& serverBaseUrl)
{
    if (!serverBaseUrl.isValid() || serverBaseUrl.isRelative())
        return std::nullopt;
    if (serverBaseUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return std::nullopt;
    if (serverBaseUrl.host().isEmpty())
        return std::nullopt;

    QUrl url = serverBaseUrl;
    url.setPath(QStringLiteral("/read"));
    // Cleared explicitly: a base URL carrying its own query or fragment would
    // otherwise leak into the link, and setQuery() below only replaces the query.
    url.setFragment(QString());
    url.setQuery(QUrlQuery());
    return url;
}

} // namespace

QUrl webmailReadUrl(const QUrl& serverBaseUrl, const QString& mailbox, const QString& messageId)
{
    const std::optional<QUrl> base = webmailReadBase(serverBaseUrl);
    if (!base.has_value())
        return {};
    if (messageId.isEmpty())
        return {};

    QUrl url = *base;
    QUrlQuery query;
    if (!mailbox.isEmpty())
        query.addQueryItem(QStringLiteral("mailbox"), mailbox);
    query.addQueryItem(QStringLiteral("message"), messageId);
    url.setQuery(query);
    return url;
}

// Targets a mailbox rather than one message: POST /api/mail/draft answers with
// a bare {ok:true} and no UID, so a saved draft has nothing to link to.
QUrl webmailMailboxUrl(const QUrl& serverBaseUrl, const QString& mailbox)
{
    const std::optional<QUrl> base = webmailReadBase(serverBaseUrl);
    if (!base.has_value())
        return {};
    if (mailbox.isEmpty())
        return {};

    QUrl url = *base;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("mailbox"), mailbox);
    url.setQuery(query);
    return url;
}
```

Add `#include <optional>` to the `.cpp` if absent, and declare `webmailMailboxUrl` in the header with the doc comment above it.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R PgpMessagePresentationTest --output-on-failure`
Expected: PASS, including the pre-existing `webmailReadUrl` tests — the extraction must not change its behavior.

- [ ] **Step 5: Commit**

```bash
git add app/mail/PgpMessagePresentation.h app/mail/PgpMessagePresentation.cpp \
        tests/app/mail/PgpMessagePresentationTest.cpp
git commit -m "feat(pgp): build the webmail Drafts handoff URL through the shared https check"
```

---

### Task 6: `MailController` orchestration and the pending-send cache

**Files:**
- Modify: `app/mail/MailController.h` (constructor at `:33-36` of the `.cpp`, `sendMail` at `:81-82`, private section at `:180-205`), `app/mail/MailController.cpp`
- Modify: `app/main.cpp` (construct the two new clients and pass them in)
- Test: `tests/app/mail/MailControllerTest.cpp`

**Interfaces:**
- Consumes: Tasks 1-5
- Produces:
  - `Q_PROPERTY(bool pgpCanEncrypt READ pgpCanEncrypt NOTIFY pgpComposeStateChanged)`, same for `pgpCanSign` and `pgpHandoffToWebmail`
  - `Q_PROPERTY(QStringList pgpKeylessRecipients READ pgpKeylessRecipients NOTIFY pgpKeylessRecipientsChanged)`
  - `Q_INVOKABLE void refreshPgpComposeState();`
  - `Q_INVOKABLE void preflightRecipients(const QString& to, const QString& cc, const QString& bcc);`
  - `bool sendMail(to, cc, bcc, subject, body, attachmentFilePaths, bool sign, bool encrypt);` — two new trailing parameters
  - `Q_INVOKABLE bool confirmPickupFallbackSend();`
  - `Q_INVOKABLE void discardPendingSend();`
  - `Q_INVOKABLE bool openWebmailDrafts(to, cc, bcc, subject, body, attachmentFilePaths);`
  - `signals: void pickupFallbackRequired(const QStringList& recipients); void sendWarning(const QString& warning); void pgpComposeStateChanged(); void pgpKeylessRecipientsChanged();`

**The pending-send cache is the point of this task.** The spec requires the confirmed re-send to be the *byte-identical* request — do not re-run the preflight, do not rebuild the message, do not re-encode attachments, because a rebuild risks a subtly different message. `MailController::sendMail` takes local file *paths* and reads them through `readAttachments()` on every call, so re-invoking it from QML would re-read and re-encode every file. Instead cache the already-built payload:

```cpp
    // The exact payload of a send the server refused with 409 +
    // keylessRecipients, held so confirmPickupFallbackSend() can re-send it
    // byte-identically with allowPickupFallback flipped. Rebuilding from the
    // QML fields would re-read and re-base64 every attachment off disk, and
    // any drift between the refused request and the confirmed one is a
    // message the user did not review.
    //
    // Holds the composed plaintext in memory until cleared, so it is cleared
    // on success, on cancel, and whenever a fresh send starts. Under Hostile
    // Location Protection this never reaches disk, matching the rest of that
    // mode.
    struct PendingSend
    {
        bool valid = false;
        QString to;
        QString cc;
        QString bcc;
        QString subject;
        QString body;
        QString mode;
        QVector<MailAttachmentUpload> attachments;
        bool sign = false;
        bool encrypt = false;
    };
    PendingSend m_pendingSend;
```

- [ ] **Step 1: Write the failing tests**

Add to `tests/app/mail/MailControllerTest.cpp`'s `private slots:` and append the bodies. Build the controller exactly as the existing tests do (`Database db; db.open(":memory:")`, the DAO/repository chain, `savePairing(pairingStore, fake.port())`), adding the two new clients. The re-send test needs **two** `FakeRelayServer`s because each accepts one connection — and since the pairing base URL fixes the port, re-point the pairing between the two calls:

```cpp
    void sendMailEmitsPickupFallbackRequiredWithTheServersAddressList();
    void confirmPickupFallbackSendResendsTheIdenticalBodyWithTheOptIn();
    void confirmPickupFallbackSendWithoutAPendingSendDoesNothing();
    void sendMailSurfacesAWarningOnAnOtherwiseSuccessfulSend();
```

```cpp
void MailControllerTest::sendMailEmitsPickupFallbackRequiredWithTheServersAddressList()
{
    // The dialog names the SERVER's list, not the preflight's: the server ran
    // the discovery ladder as well as contacts, and may name an address typed
    // after the last preflight.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    // ... same DAO/repository/controller construction as the tests above ...

    FakeRelayServer fake(httpResponse(
        409, "Conflict",
        R"({"error":"some recipients have no usable PGP key",)"
        R"("keylessRecipients":["bob@example.com"],"pickupFallbackAvailable":true})"));
    savePairing(pairingStore, fake.port());

    QSignalSpy spy(&controller, &MailController::pickupFallbackRequired);
    const bool sent = controller.sendMail(QStringLiteral("bob@example.com"), QString(), QString(),
                                           QStringLiteral("Hi"), QStringLiteral("Body"), {},
                                           /*sign=*/false, /*encrypt=*/true);

    QCOMPARE(sent, false);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{ QStringLiteral("bob@example.com") });
}

void MailControllerTest::confirmPickupFallbackSendResendsTheIdenticalBodyWithTheOptIn()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    // ... same construction ...

    FakeRelayServer refusal(httpResponse(
        409, "Conflict",
        R"({"error":"no usable key","keylessRecipients":["bob@example.com"],"pickupFallbackAvailable":true})"));
    savePairing(pairingStore, refusal.port());
    QVERIFY(!controller.sendMail(QStringLiteral("bob@example.com"), QString(), QString(),
                                  QStringLiteral("Hi"), QStringLiteral("Body"), {},
                                  /*sign=*/false, /*encrypt=*/true));

    // Second server: FakeRelayServer serves exactly one connection.
    FakeRelayServer accepted(httpResponse(200, "OK", R"({"ok":true,"sentSaved":true,"warning":""})"));
    savePairing(pairingStore, accepted.port());
    QVERIFY(controller.confirmPickupFallbackSend());

    const QJsonObject first = refusal.receivedJsonBody();
    const QJsonObject second = accepted.receivedJsonBody();
    QCOMPARE(second.value(QStringLiteral("allowPickupFallback")).toBool(), true);
    QCOMPARE(first.value(QStringLiteral("allowPickupFallback")).toBool(), false);
    // Everything else is byte-identical: same subject, body, recipients, flags.
    for (const QString& key : { QStringLiteral("to"), QStringLiteral("cc"), QStringLiteral("bcc"),
                                 QStringLiteral("subject"), QStringLiteral("body"),
                                 QStringLiteral("mode"), QStringLiteral("sign"),
                                 QStringLiteral("encrypt") }) {
        QCOMPARE(second.value(key), first.value(key));
    }
    QCOMPARE(second.value(QStringLiteral("attachments")), first.value(QStringLiteral("attachments")));

    // The opt-in is per-message: a second confirm must not re-send anything.
    QCOMPARE(controller.confirmPickupFallbackSend(), false);
}

void MailControllerTest::confirmPickupFallbackSendWithoutAPendingSendDoesNothing()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    // ... same construction, pairing saved, but no send attempted ...

    QCOMPARE(controller.confirmPickupFallbackSend(), false);
}

void MailControllerTest::sendMailSurfacesAWarningOnAnOtherwiseSuccessfulSend()
{
    // A non-empty warning means partial trouble (the Sent copy failed, or some
    // pickup links did not deliver) -- the message WAS sent, so this must not
    // look like a failure and must not offer a retry that would duplicate it.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    // ... same construction ...

    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"ok":true,"sentSaved":false,"warning":"failed to deliver a pickup link to 1 of 3 recipient(s)"})"));
    savePairing(pairingStore, fake.port());

    QSignalSpy spy(&controller, &MailController::sendWarning);
    const bool sent = controller.sendMail(QStringLiteral("bob@example.com"), QString(), QString(),
                                           QStringLiteral("Hi"), QStringLiteral("Body"), {},
                                           /*sign=*/false, /*encrypt=*/true);

    QCOMPARE(sent, true);
    QCOMPARE(spy.count(), 1);
    QVERIFY(!spy.at(0).at(0).toString().isEmpty());
}
```

Every `// ... same construction ...` above means: copy `tests/app/mail/MailControllerTest.cpp:59-128` verbatim (the `Database`/DAO/repository/`MailController` chain from `Database db;` through the `MailController controller(...)` line). Do not invent a shorter version — the DAOs hold `QSqlDatabase&` references and must outlive the controller.

`MailController`'s constructor gains two parameters in Step 3, so **all existing `MailController controller(...)` call sites in this test file must be updated** (currently `:127`, `:186`, `:229`, plus any added since). Construct `PgpBootstrapClient bootstrapClient(http);` and `PgpRecipientChecker recipientChecker(http);` next to the existing `RelayMailSource source(http);` and pass both.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R MailControllerTest --output-on-failure`
Expected: FAIL to compile — `sendMail` takes 6 arguments, and the new members do not exist.

- [ ] **Step 3: Implement the send path**

`sendMail` gains `bool sign, bool encrypt`. After `requirePairing` and `readAttachments` succeed, it caches the payload, sends with `allowPickupFallback = false`, then dispatches on the result:

```cpp
    // Field order matters -- PendingSend is aggregate-initialized. Ten fields:
    // valid, to, cc, bcc, subject, body, mode, attachments, sign, encrypt.
    m_pendingSend = PendingSend{ true, to, cc, bcc, subject, body, sendMode, attachments, sign, encrypt };

    const SendMailResult result = m_relayMailSource.sendMail(
        serverBaseUrl, auth, to, cc, bcc, subject, body, sendMode, attachments,
        sign, encrypt, /*allowPickupFallback=*/false);

    if (result.pickupFallbackNeeded) {
        // Nothing was delivered; the pending payload stays cached for the
        // confirmed re-send. The server's list is authoritative -- it ran WKD
        // discovery too, and may name an address typed since the last preflight.
        emit pickupFallbackRequired(result.keylessRecipients);
        return false;
    }
    if (result.clientSideNeeded) {
        // Categorical: no re-send from this client can fix it.
        m_pendingSend = {};
        m_pgpHandoffToWebmail = true;
        emit pgpComposeStateChanged();
        setLastError(i18n("This account's PGP key is held only in your browser, so this app cannot "
                           "encrypt on its behalf. Continue in webmail to send it."));
        return false;
    }
    m_pendingSend = {};
    if (!result.ok) {
        // Same shape as every other failure in this class: prefer the server's
        // own detail, fall back to localized wording.
        setLastError(result.detail.isEmpty() ? i18n("Could not send message") : result.detail);
        return false;
    }
    if (!result.warning.isEmpty())
        emit sendWarning(result.warning);
    return true;
```

`sendMode` above stands for whatever expression the current `sendMail` already passes as `mode` — it is hardcoded `"html"` today (see the header comment at `MailController.h:74-80`). Do not introduce a new constant; reuse the existing expression and store that same value in `PendingSend::mode` so the re-send cannot differ. Keep the existing `setLastError(QString())`-on-success call and the `setBusy(true/false)` bracketing exactly where they already are.

`confirmPickupFallbackSend()`:

```cpp
// Re-sends the exact payload the server refused, with allowPickupFallback set.
// Byte-identical by construction: nothing is rebuilt, no file is re-read, and
// the preflight is not re-run. Returns false without sending when there is no
// pending send, so a stray confirm cannot mail anything.
bool MailController::confirmPickupFallbackSend()
{
    if (!m_pendingSend.valid)
        return false;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return false;

    const PendingSend pending = m_pendingSend;
    // Cleared before the call, not after: the opt-in is per-message, and a
    // failure here must not leave a payload a second confirm could re-send.
    m_pendingSend = {};

    const SendMailResult result = m_relayMailSource.sendMail(
        serverBaseUrl, auth, pending.to, pending.cc, pending.bcc, pending.subject, pending.body,
        pending.mode, pending.attachments, pending.sign, pending.encrypt,
        /*allowPickupFallback=*/true);

    if (!result.ok) {
        setLastError(result.detail);
        return false;
    }
    if (!result.warning.isEmpty())
        emit sendWarning(result.warning);
    return true;
}

// Cancelling the dialog drops the cached plaintext rather than holding it for
// a confirm that may never come.
void MailController::discardPendingSend()
{
    m_pendingSend = {};
}
```

- [ ] **Step 4: Implement bootstrap, preflight and the handoff**

```cpp
// Called once when Compose opens. A failure leaves every control hidden --
// "couldn't check" is never "no PGP".
void MailController::refreshPgpComposeState()
{
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return;

    const PgpBootstrapResult bootstrap = m_pgpBootstrapClient.fetch(serverBaseUrl, auth);
    const PgpComposeState state = bootstrap.ok
        ? pgpComposeStateOf(bootstrap.hasIdentity, bootstrap.protection)
        : pgpComposeStateOf(std::nullopt, std::nullopt);

    m_pgpCanEncrypt = state.canEncrypt;
    m_pgpCanSign = state.canSign;
    m_pgpHandoffToWebmail = state.handoffToWebmail;
    emit pgpComposeStateChanged();
}

// Inline, non-blocking warning only. This is a LOWER BOUND: check reads the
// user's contacts, while the send path also runs WKD/keyserver discovery, so
// an address named here may still be encrypted to successfully. Never phrase
// the result as a prediction, and never let it gate the send -- the server's
// 409 is the gate.
void MailController::preflightRecipients(const QString& to, const QString& cc, const QString& bcc)
{
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return;

    QStringList addresses;
    for (const QString& field : { to, cc, bcc })
        addresses += field.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString& address : addresses)
        address = address.trimmed();
    addresses.removeAll(QString());

    QStringList keyless;
    if (!addresses.isEmpty()) {
        const RecipientKeyCheckResult result = m_pgpRecipientChecker.check(serverBaseUrl, auth, addresses);
        // A failed preflight shows nothing rather than a false all-clear.
        if (result.ok)
            keyless = result.keylessRecipients;
    }
    if (keyless == m_pgpKeylessRecipients)
        return;
    m_pgpKeylessRecipients = keyless;
    emit pgpKeylessRecipientsChanged();
}

// Saves the composition to Drafts, then opens webmail there in the user's
// browser -- never an embedded view, which shares no session and would put an
// account-password field inside this app.
//
// Returns false without opening anything if the draft did not save: opening a
// browser onto a draft that is not there loses the user's message. Also
// returns false when the pairing's base URL is not https, since
// webmailMailboxUrl() refuses to build a link from a downgraded base.
bool MailController::openWebmailDrafts(const QString& to, const QString& cc, const QString& bcc,
                                        const QString& subject, const QString& body,
                                        const QStringList& attachmentFilePaths)
{
    const QUrl url = webmailMailboxUrl(webmailBaseUrl(), QStringLiteral("Drafts"));
    // Checked BEFORE saving: a draft saved for a handoff that cannot open
    // leaves the user with a silently duplicated draft and no browser.
    if (url.isEmpty()) {
        setLastError(i18n("This device is paired over an insecure connection, so KyPost cannot open "
                           "webmail for you. Open your mail in a browser to send this message."));
        return false;
    }
    if (!saveDraft(to, cc, bcc, subject, body, attachmentFilePaths))
        return false;
    if (!QDesktopServices::openUrl(url)) {
        setLastError(i18n("Saved to Drafts, but KyPost could not open your browser."));
        return false;
    }
    return true;
}
```

Add the `PgpBootstrapClient&` and `PgpRecipientChecker&` constructor parameters and members, then construct both in `app/main.cpp` beside the existing `PgpQrClient`/`FolderClient` construction and pass them to `MailController`. Include `<QDesktopServices>` in the `.cpp`.

- [ ] **Step 5: Run the full suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS — every test, not just the new ones. The `sendMail` signature change touches `Compose.qml`'s call site, which Task 7 fixes; until then the QML call passes too few arguments. Give `sign`/`encrypt` no defaults and update the single call site in this task's commit so the tree is never broken:

In `app/qml/pages/Compose.qml:137`, pass `false, false` for now — Task 7 replaces them with the toggle states.

- [ ] **Step 6: Commit**

```bash
git add app/mail/MailController.h app/mail/MailController.cpp app/main.cpp \
        app/qml/pages/Compose.qml tests/app/mail/MailControllerTest.cpp
git commit -m "feat(pgp): orchestrate encrypted send, the confirmed re-send, and the webmail handoff"
```

---

### Task 7: Compose UI — toggles, warning, dialog, handoff

**Files:**
- Modify: `app/qml/pages/Compose.qml` (send handler at `:130-160`, toolbar at `:355-370`)

**Interfaces:**
- Consumes: Task 6's properties, invokables and signals

**The dialog copy is contract.** Reproduce it as written — it is the entire reason the opt-in exists, and softening it defeats the purpose. Name the addresses explicitly; never summarize as "some recipients".

- [ ] **Step 1: Add the toggles and the handoff affordance**

Two `CheckBox`es above the send row, `visible: MailApp.pgpCanEncrypt` and `visible: MailApp.pgpCanSign`, labelled `i18n("Encrypt")` and `i18n("Sign")`, both unchecked on open. When `MailApp.pgpHandoffToWebmail` is true, hide both and show a `Button` reading `i18n("Continue in webmail")` with an explanatory `Label`: `i18n("This account's PGP key is stored only in your browser, so this app can't encrypt on its behalf.")`

Call `MailApp.refreshPgpComposeState()` in the page's `Component.onCompleted`.

- [ ] **Step 2: Debounced preflight with the lower-bound wording**

A `Timer { id: preflightTimer; interval: 500; onTriggered: MailApp.preflightRecipients(toField.joinedText, ccField.joinedText, bccField.joinedText) }`, restarted on edits to any of the three recipient fields, and only while `encryptToggle.checked`. Render the result as an inline, non-blocking warning:

```qml
Label {
    visible: encryptToggle.checked && MailApp.pgpKeylessRecipients.length > 0
    wrapMode: Text.WordWrap
    // Deliberately "no key on file", not "will be sent as a plaintext link":
    // the preflight reads contacts only, while the send path also runs WKD
    // discovery, so this is a lower bound and must not be phrased as a
    // prediction.
    text: i18np("We don't have a PGP key on file for %2.",
                "We don't have PGP keys on file for %2.",
                MailApp.pgpKeylessRecipients.length,
                MailApp.pgpKeylessRecipients.join(", "))
}
```

- [ ] **Step 3: Add the confirmation dialog**

```qml
Dialog {
    id: pickupFallbackDialog
    property var recipients: []
    title: i18n("Send an unencrypted link?")
    modal: true
    // Default to cancel: the affirmative action must never be the one a
    // stray Return key picks.
    standardButtons: Dialog.Cancel | Dialog.Ok
    onOpened: standardButton(Dialog.Cancel).forceActiveFocus()

    ColumnLayout {
        Label {
            wrapMode: Text.WordWrap
            text: i18np("We don't have a PGP key for %2. They'll get an email with a one-time link instead.",
                        "We don't have PGP keys for %2. They'll get an email with a one-time link instead.",
                        pickupFallbackDialog.recipients.length,
                        pickupFallbackDialog.recipients.join(", "))
        }
        Label {
            wrapMode: Text.WordWrap
            // The seven days and the word "unencrypted" are the security
            // property this dialog exists to disclose. Do not soften.
            text: i18n("To make that work, this message's contents are stored on your KyPost server — unencrypted — for up to 7 days or until the link is opened. Everyone else on this message still gets it encrypted.")
        }
    }

    onAccepted: {
        if (!MailApp.confirmPickupFallbackSend()) {
            return;
        }
        root.sendCompleted();
    }
    onRejected: MailApp.discardPendingSend()
}

Connections {
    target: MailApp
    function onPickupFallbackRequired(recipients) {
        pickupFallbackDialog.recipients = recipients;
        pickupFallbackDialog.open();
    }
    function onSendWarning(warning) {
        // Sent, with a caveat. Never a failure, and never offer a retry --
        // that would duplicate the message.
        root.showNotice(warning);
    }
}
```

Relabel OK to `i18n("Send link anyway")` via `Dialog.standardButton(Dialog.Ok).text`. Match `root.sendCompleted()`/`root.showNotice()` to whatever signals and notice mechanism `Compose.qml` already uses for a successful send (see `:34-40`) — do not add a second notification path.

- [ ] **Step 4: Pass the toggle states and wire the handoff button**

Replace the `false, false` from Task 6 at the `MailApp.sendMail(...)` call with `signToggle.checked, encryptToggle.checked` in the declared parameter order (`..., sign, encrypt`). The handoff button calls `MailApp.openWebmailDrafts(toField.joinedText, ccField.joinedText, bccField.joinedText, subjectField.text, bodyEditor.html, root.attachmentPaths)`; on false, show `MailApp.lastError` and keep the page open — do not navigate away.

- [ ] **Step 5: Build and verify**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS. Then confirm the QML has no runtime errors: `cmake --build build && ./build/app/kypost --version` at minimum, and ideally open Compose in a running app.

- [ ] **Step 6: Update the pot file and commit**

```bash
cmake --build build --target update-pot   # or however AGENTS.md says to regenerate po/kypost.pot
git add app/qml/pages/Compose.qml po/kypost.pot
git commit -m "feat(pgp): offer encrypted send, the fallback confirmation, and the webmail handoff"
```

Check `TESTING.md` / `AGENTS.md` for the actual pot-regeneration command before running that target; every new `i18n()` string above must appear in `po/kypost.pot`.

---

## Manual verification

Only possible once `kypost-server`'s `feat/mobile-encrypted-send` is merged and deployed. None of the tests above touch a real SMTP server.

- [ ] Server-custody account, recipient with a key in Contacts → arrives encrypted, no dialog.
- [ ] Server-custody, one keyless recipient → dialog names that exact address; Cancel leaves the composition intact and sends nothing; confirm delivers and the keyless recipient gets a working link.
- [ ] Confirm that a second send in the same session starts with the fallback opt-in **off** — it must not be remembered.
- [ ] Server-custody account with no PGP identity → Encrypt is offered, Sign is not, and an encrypted send to a keyed recipient works.
- [ ] Client-custody account → toggles replaced by "Continue in webmail"; clicking it saves the draft and opens the system browser at Drafts.
- [ ] Relay unreachable when Compose opens → no PGP controls at all, plain send still works.
- [ ] A recipient the preflight calls keyless but WKD finds → no dialog, message arrives encrypted (this is trap 2 in practice).
- [ ] Pairing on plain `http` → the handoff button reports the insecure-connection error rather than doing nothing.

## Deviations from the two source documents

Recorded so a reviewer can see these were decisions, not drift.

**1. Encrypt is offered without a PGP identity.** `Client_Encrypted_Send.md` says to offer the toggles "when `hasIdentity` is true" and lists `protection: ""` as "plaintext send only". The server disagrees: `server.go:1214-1223` only 400s when `req.Sign` is set without a server-readable key, and encryption uses the recipients' keys. Following the doc would deny encryption to an account that never made a key. Implemented per the server; Task 2's test names the file and lines.

**2. The confirmation is driven by the server's 409, not by the preflight.** The superseded plan had a `sendGateFor(keylessRecipients, confirmed)` that opened the dialog from the *preflight* result. That directly violates trap 2 — the preflight is contacts-only, so it would show the plaintext-link dialog for recipients whose keys WKD would have found. There is no `SendGate` here; the client always sends with `allowPickupFallback: false` first and lets the server's refusal be the gate.

**3. No Android parity constraint.** The superseded plan required `PgpComposeState` to match Android's `pgpComposeStateOf` and to read `kypost-android/.../pgp/PgpComposeState.kt` first. **That file does not exist** — nothing in `kypost-android` defines `pgpComposeStateOf`. The constraint was unexecutable, so it is dropped. If Android later grows an equivalent, whichever side lands second should match the other, and this truth table is the reference.

**4. Bootstrap is fetched when Compose opens, not once per launch.** The spec says "call once per launch, cache for the session". Fetching on `Component.onCompleted` of Compose costs one extra request per compose but keeps the state fresh and avoids a launch-order dependency on pairing being loaded. Custody mode cannot change within a session (it is fixed at key creation with no downgrade path), so either timing is correct; this one is simpler to reason about. If compose-open latency ever matters, cache it in `MailController` behind a "already fetched" flag.

**5. The webmail base URL rule is https-only, not "http(s)".** The spec describes the existing check as "rejecting non-http(s) schemes"; the shipped code (`PgpMessagePresentation.cpp:82`) requires https. Kept strict and shared, with the plain-http consequence surfaced as an error rather than a silent no-op.

**6. The confirmation dialog is an overlay `Item`, not a `QtQuick.Controls.Dialog`.** The plan sketched `PickupFallbackDialog.qml` as a `Controls.Dialog`. It is implemented instead in this codebase's existing overlay-`Item` shape (a scrim `Rectangle` with a `TapHandler`, plus a centered panel), matching `HyperlinkDialog.qml` and `AddressBookPickerDialog.qml` — `HyperlinkDialog.qml`'s own header comment already records that there is **no `Controls.Dialog` precedent anywhere in this repo**, and introducing the first one for the single most security-sensitive dialog in the app is the wrong place to take on a new, untravelled Qt Quick Controls behavior (its own popup/overlay/z-ordering/modality rules, and `Controls.Dialog`'s theming, which nothing here styles yet).

The substitution has one real cost, stated rather than hidden: **no dialog button takes focus, so the affirmative action cannot be triggered by a keystroke.** A `Controls.Dialog` with `standardButtons` would have given both keyboard activation *and* Escape-to-reject for free. The Escape half is implemented by hand (the panel claims active focus on `open()` and handles `Keys.onEscapePressed` → `cancel()`); the keyboard-activation half is deliberately **not** restored. For this dialog that asymmetry is a feature, not a regression: the affirmative action consents to a server-side *plaintext* copy of the message for up to 7 days, and requiring a deliberate pointer press on a distinct button — while leaving all four cancel paths (button, scrim, Escape, walking away) cheap — is the safer default. Cancel-by-reflex should be easy; consent-by-reflex should not be. A consequence worth knowing: this dialog is not operable by keyboard alone, which is an accessibility gap. Revisit if `Controls.Dialog` ever gains a precedent here, and if so, prefer explicitly setting the reject button as the default rather than the accept button.
