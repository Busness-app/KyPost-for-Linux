# Linux in-app update check (E1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A KyPost Linux user learns when a newer release exists, by asking their own paired KyPost server rather than GitHub.

**Architecture:** The server already polls GitHub hourly for its own releases (`StartVersionMonitor`). It gains a second check for the Linux client repo, caches the answer, and serves it on a device-authenticated endpoint. The Linux client reads that endpoint over its existing pinned connection, compares the result against its compiled-in `KYPOST_VERSION`, and surfaces a notice. Nothing downloads or installs anything.

**Tech Stack:** Go 1.x + `net/http` + `httptest` (server); C++20 + Qt 6 + QtTest + CMake/CTest (client).

**Spec:** `docs/superpowers/specs/2026-08-25-linux-update-check-design.md` (in the `kypost-Linux` repo)

## Global Constraints

- **Two repos.** Tasks 1-3 and 8 are in `kypost-server` (`/home/yoshi/busness.app/kypost-server`). Tasks 4-7 are in `kypost-Linux` (`/home/yoshi/busness.app/kypost-Linux`). Confirm your working directory before every edit — the two repos have same-named files.
- **Release ordering:** the server endpoint (Tasks 2-3) must be released before the client (Tasks 4-7). Task 5 makes a 404 harmless, which is what allows the client to ship against older servers.
- **The releases URL is permanent for every install that ships with it:** `https://api.github.com/repos/Busness-app/KyPost-for-Linux/releases`. Verified against the Linux repo's `git remote`. Use the **list** endpoint, never `/releases/latest` — the list is what allows a soak window.
- **Soak window is 6 hours** (`linuxClientReleaseMinAge = 6 * time.Hour`).
- **The installed version is the left-hand side of the comparison** and is compiled in. Never send the client's version to the server for comparison.
- **Version strings:** tags are `v0.2.0`, `KYPOST_VERSION` is `0.2.0`. Strip a leading `v` on both sides. Refuse anything that is not `N.N.N` — `v0.1-alpha` exists in the Linux tag list today.
- **Never auto-update.** A Flatpak cannot update itself. Notice and link only.
- **Do not touch `StatusBanner.qml`.** It is danger-styled and non-dismissible by design, reserved for conditions that stop the app working.
- Branch before committing; do not commit directly to `main` in either repo.

---

### Task 1: Correct `PLATFORM_BASELINE.md`

The matrix marks every Linux row `unverified`, and the handoff that fed this plan reported the enrollment-code grouping as broken. It is not — `formatEnrollmentCode` is applied before the code reaches QML. Correcting this first stops someone "fixing" working security-ceremony code.

**Files:**
- Modify: `docs/PLATFORM_BASELINE.md` (**`kypost-server` repo**)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing consumed by later tasks. Independent — can land in parallel with Tasks 2-7.

- [ ] **Step 1: Read the current matrix**

```bash
cd /home/yoshi/busness.app/kypost-server
grep -n "Linux" docs/PLATFORM_BASELINE.md
```

- [ ] **Step 2: Set each Linux row to its verified value**

Replace the Linux column entries with these. Every citation below was verified against the Linux tree at `e1a1a9c`; paths are relative to the `kypost-Linux` repo root.

| Contract | Linux | Evidence |
| --- | --- | --- |
| Pairing URI, unknown-param tolerance | ✅ | `app/pairing/PairingController.cpp:130-148` |
| `pin=` honoured | ❌ TOFU instead | `core/net/NativeRegistrationClient.h:62-70`, `core/net/CertificatePinSink.cpp` |
| Enrollment code: 14 chars / Crockford / 65-byte key | ✅ | `core/pgp/DeviceEnrollmentCrypto.cpp:17,127,144-158` |
| Enrollment display grouping (`4-3-4-3`) | ✅ | `core/pgp/DeviceEnrollmentCrypto.cpp:156-162`, `app/pgp/PgpEnrollmentController.cpp:107` |
| Enrollment bucket size 120 s | ✅ | `app/pgp/PgpEnrollmentController.cpp:108` |
| `transport` sent explicitly | ✅ `"unifiedpush"` | `core/net/NativeRegistrationClient.cpp:43` |
| Device credential headers | ✅ | `core/net/RelayAuth.h:22-23` |
| Contact sync | ✅ | `core/net/ContactSyncClient.cpp:215` |
| Delivery modes | ✅ `push`/`pull` | `app/pairing/PairingController.h:129-130` |

- [ ] **Step 3: Add a note under the grouping row**

Add this sentence so the correction does not get re-reverted by the next reader of the handoff:

```markdown
> Linux was reported ungrouped on 2026-08-25. That report was wrong: it read
> `Settings.qml`'s raw property binding without following
> `PgpEnrollmentController.cpp:107`, which applies `formatEnrollmentCode`
> before the value reaches QML. The helper has been present since `e92b16b`.
```

- [ ] **Step 4: Note the one row still unverified**

The contact-sync 500-change batching limit is still unverified — only the pull path was read. Leave that row `unverified` and do not guess it.

- [ ] **Step 5: Commit**

```bash
cd /home/yoshi/busness.app/kypost-server
git checkout -b docs/platform-baseline-linux-verified
git add docs/PLATFORM_BASELINE.md
git commit -m "docs(baseline): settle the Linux rows, including one that was never broken"
```

---

### Task 2: Server — the Linux client release check and its cache

**Files:**
- Create: `backend/internal/api/client_version.go` (**`kypost-server` repo**)
- Create: `backend/internal/api/client_version_test.go`
- Modify: `backend/internal/api/server.go:44-49` (lock-order comment), `:176-177` (struct fields)
- Modify: `backend/internal/api/ollama_version.go:62-77` (hook into `StartVersionMonitor`)
- Modify: `backend/internal/api/lock_order_test.go` (`lockRank` map)

**Interfaces:**
- Consumes: `ghrelease.Latest(ctx, url, minAge) (string, error)` — unchanged, no edits to that package.
- Produces:
  - `type linuxClientStatus struct { latestVersion string; checkedAt time.Time; checkErr string }`
  - `func (s *Server) checkForLinuxClientUpdate(ctx context.Context)`
  - `func (s *Server) getLinuxClientStatus() linuxClientStatus`
  - `var linuxClientReleasesURL string`
  - Task 3 consumes `getLinuxClientStatus`.

- [ ] **Step 1: Write the failing test**

Create `backend/internal/api/client_version_test.go`. Note it needs its own `serveClientReleases` helper — the existing `serveReleases` in `server_version_test.go:99-110` swaps `serverReleasesURL`, which is a different variable.

```go
package api

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// serveClientReleases points linuxClientReleasesURL at a fake GitHub for the
// duration of one test. Mirrors serveReleases in server_version_test.go,
// which swaps a different variable.
func serveClientReleases(t *testing.T, body string) {
	t.Helper()
	gh := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(body))
	}))
	orig := linuxClientReleasesURL
	linuxClientReleasesURL = gh.URL
	t.Cleanup(func() {
		linuxClientReleasesURL = orig
		gh.Close()
	})
}

func published(ago time.Duration) string {
	return time.Now().UTC().Add(-ago).Format(time.RFC3339)
}

// TestLinuxClientStatusReportsNewestSoakedRelease covers the states the
// endpoint has to distinguish. The soak window is the interesting one: a
// Linux tag is not installable until flatpak.yml has finished BOTH arch
// bundles, and the aarch64 build has already failed twice on Flathub CDN
// faults, so advertising a fresh tag can point users at a release that has
// no bundle for their machine.
func TestLinuxClientStatusReportsNewestSoakedRelease(t *testing.T) {
	for _, tc := range []struct {
		name string
		body string
		want string
	}{
		{
			name: "soaked release is reported",
			body: `[{"tag_name":"v0.3.0","published_at":"` + published(24*time.Hour) + `"}]`,
			want: "0.3.0",
		},
		{
			name: "release still inside the soak window is not",
			body: `[{"tag_name":"v0.3.0","published_at":"` + published(time.Hour) + `"}]`,
			want: "",
		},
		{
			name: "draft is skipped",
			body: `[{"tag_name":"v0.3.0","draft":true,"published_at":"` + published(24*time.Hour) + `"}]`,
			want: "",
		},
		{
			name: "prerelease is skipped",
			body: `[{"tag_name":"v0.3.0","prerelease":true,"published_at":"` + published(24*time.Hour) + `"}]`,
			want: "",
		},
		{
			name: "newest soaked release wins",
			body: `[{"tag_name":"v0.4.0","published_at":"` + published(time.Hour) + `"},` +
				`{"tag_name":"v0.3.0","published_at":"` + published(48*time.Hour) + `"}]`,
			want: "0.3.0",
		},
		{
			name: "no releases at all is an ordinary state",
			body: `[]`,
			want: "",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			srv := newTestServer(t)
			serveClientReleases(t, tc.body)

			srv.checkForLinuxClientUpdate(context.Background())

			got := srv.getLinuxClientStatus()
			if got.latestVersion != tc.want {
				t.Fatalf("latestVersion = %q, want %q", got.latestVersion, tc.want)
			}
			if got.checkErr != "" {
				t.Fatalf("unexpected checkErr %q", got.checkErr)
			}
			if got.checkedAt.IsZero() {
				t.Fatal("checkedAt must be set even when there is nothing to report")
			}
		})
	}
}

// A GitHub outage must not become a permanent error on the user's About
// screen: the status records the failure, the next hourly tick retries.
func TestLinuxClientStatusRecordsCheckFailure(t *testing.T) {
	srv := newTestServer(t)
	gh := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer gh.Close()
	orig := linuxClientReleasesURL
	linuxClientReleasesURL = gh.URL
	defer func() { linuxClientReleasesURL = orig }()

	srv.checkForLinuxClientUpdate(context.Background())

	if srv.getLinuxClientStatus().checkErr == "" {
		t.Fatal("a failed release check must be recorded")
	}
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd /home/yoshi/busness.app/kypost-server
go test ./backend/internal/api/ -run TestLinuxClientStatus -v
```

Expected: FAIL to **compile**, with `undefined: linuxClientReleasesURL`, `undefined: checkForLinuxClientUpdate`, `undefined: getLinuxClientStatus`.

- [ ] **Step 3: Create `backend/internal/api/client_version.go`**

```go
package api

import (
	"context"
	"net/http"
	"time"

	"kypost-server/backend/internal/ghrelease"
)

// linuxClientReleasesURL is the LIST endpoint for the Linux client's own
// releases. A var, not a const, only so tests can point it at an httptest
// server — the same reason serverReleasesURL is one.
//
// This URL is compiled into every client that asks for it and is permanent
// for the life of those installs. It was read from the Linux repository's
// git remote rather than assumed.
var linuxClientReleasesURL = "https://api.github.com/repos/Busness-app/KyPost-for-Linux/releases"

// linuxClientReleaseMinAge is six hours where serverReleaseMinAge is zero.
//
// A KyPost-Server release is installable the moment it is published, so it
// has nothing to wait for. A Linux tag is not: flatpak.yml builds the x86_64
// and aarch64 bundles AFTER the tag exists, and the aarch64 job has already
// died twice on Flathub CDN faults (flatpak.yml:172-180). Without a soak
// window we would tell an aarch64 user about a release that has no bundle
// they can install.
const linuxClientReleaseMinAge = 6 * time.Hour

// linuxClientStatus is the last completed Linux-client release check.
//
// It holds no "upgradeAvailable" field on purpose. This server does not know
// what version any given client is running, and must not: the comparison
// belongs on the client, where the installed version is a compiled-in
// constant and is the LEFT-HAND SIDE. server_version.go:12-28 explains what
// a wrong left-hand side costs.
type linuxClientStatus struct {
	latestVersion string
	checkedAt     time.Time
	checkErr      string
}

func (s *Server) getLinuxClientStatus() linuxClientStatus {
	s.linuxClientMu.Lock()
	defer s.linuxClientMu.Unlock()
	return s.linuxClientStatus
}

func (s *Server) setLinuxClientStatus(status linuxClientStatus) {
	s.linuxClientMu.Lock()
	defer s.linuxClientMu.Unlock()
	s.linuxClientStatus = status
}

// checkForLinuxClientUpdate refreshes the cached newest Linux client release.
// Run from StartVersionMonitor alongside checkForServerUpdate.
//
// Unlike checkForServerUpdate this emails nobody. A server upgrade is the
// admin's to apply, so mailing them is actionable; a Linux client upgrade is
// applied by whoever is sitting at the Linux machine, who is usually not the
// admin and cannot act on the mail.
//
// Failures are recorded and logged rather than retried here: this runs
// unattended on an hourly tick, GitHub being unreachable is routine for a
// self-hosted box, and the next tick retries.
func (s *Server) checkForLinuxClientUpdate(ctx context.Context) {
	checkCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()

	latest, err := ghrelease.Latest(checkCtx, linuxClientReleasesURL, linuxClientReleaseMinAge)
	if err != nil {
		s.logger.Error("linux client release check failed", "error", err.Error())
		s.setLinuxClientStatus(linuxClientStatus{
			checkedAt: time.Now().UTC(),
			checkErr:  "failed to check for updates",
		})
		return
	}
	// An empty latest means no release has soaked yet, or the repository has
	// published none. Both are ordinary states, not errors: the client renders
	// an empty latestVersion as "no information", never as a failure.
	s.setLinuxClientStatus(linuxClientStatus{
		latestVersion: latest,
		checkedAt:     time.Now().UTC(),
	})
}
```

Leave the `net/http` import out for now if your linter objects — Task 3 adds the handler to this file and uses it.

- [ ] **Step 4: Add the mutex and cache to the `Server` struct**

In `backend/internal/api/server.go`, next to the existing pair at `:176-177`:

```go
	serverMu     sync.Mutex
	serverStatus serverVersionStatus

	linuxClientMu     sync.Mutex
	linuxClientStatus linuxClientStatus
```

- [ ] **Step 5: Extend the documented lock order**

`server.go:44` currently reads:

```
// LOCK ORDER: cfgMu before sessMu before pairingMu before userMu before ollamaMu before serverMu. Never the
```

Change it to:

```
// LOCK ORDER: cfgMu before sessMu before pairingMu before userMu before ollamaMu before serverMu before
// linuxClientMu. Never the
```

Then add `linuxClientMu` to the `lockRank` map in `lock_order_test.go`, ranked after `serverMu`. `TestLockOrderIsRespected` does not check a mutex that is missing from that map, so skipping this silently drops the new mutex out of the check rather than failing.

- [ ] **Step 6: Hook it into the hourly monitor**

In `backend/internal/api/ollama_version.go`, both in the startup call and inside the ticker loop (`:63-76`):

```go
func (s *Server) StartVersionMonitor(ctx context.Context) {
	s.refreshOllamaVersionStatus(ctx)
	s.checkForServerUpdate(ctx)
	s.checkForLinuxClientUpdate(ctx)

	ticker := time.NewTicker(versionPollInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			s.refreshOllamaVersionStatus(ctx)
			s.checkForServerUpdate(ctx)
			s.checkForLinuxClientUpdate(ctx)
		}
	}
}
```

Update that function's doc comment, which currently says the monitor checks "the installed Ollama and KyPost-Server versions" — it now checks three things.

- [ ] **Step 7: Run the tests to verify they pass**

```bash
cd /home/yoshi/busness.app/kypost-server
go test ./backend/internal/api/ -run 'TestLinuxClientStatus|TestLockOrderIsRespected' -v
```

Expected: PASS, all subtests.

- [ ] **Step 8: Commit**

```bash
cd /home/yoshi/busness.app/kypost-server
git checkout -b feat/linux-client-version-endpoint
git add backend/internal/api/client_version.go backend/internal/api/client_version_test.go \
        backend/internal/api/server.go backend/internal/api/ollama_version.go \
        backend/internal/api/lock_order_test.go
git commit -m "feat(api): track the newest Linux client release, with a soak window"
```

---

### Task 3: Server — the `/api/client/version` endpoint

**Files:**
- Modify: `backend/internal/api/client_version.go` (append the handler)
- Modify: `backend/internal/api/client_version_test.go` (append the endpoint test)
- Modify: `backend/internal/api/server.go` (route registration, near `:541`)

**Interfaces:**
- Consumes: `getLinuxClientStatus()` from Task 2; `s.deviceAuthFromRequest(r) (userID, device, ok, retryAfter)`; `writeDeviceAuthFailure(w, retryAfter)`; `writeJSON(w, status, any)`; the `withDeviceAuth` marker.
- Produces: `GET /api/client/version` returning `{"latestVersion": string, "checkedAt": string, "error": string}`. Task 5 parses exactly these three keys.

- [ ] **Step 1: Write the failing test**

Append to `backend/internal/api/client_version_test.go`:

```go
// The endpoint serves the cache and never performs network I/O of its own,
// so opening the client's About screen cannot generate a GitHub request.
func TestClientVersionEndpointServesCache(t *testing.T) {
	srv := newTestServer(t)
	srv.setLinuxClientStatus(linuxClientStatus{
		latestVersion: "0.3.0",
		checkedAt:     time.Now().UTC(),
	})

	id, secret := pairNativeDevice(t, srv, testUserID(t, srv), "device-e1")
	req := httptest.NewRequest(http.MethodGet, "/api/client/version", nil)
	req.Header.Set("X-Kypost-Device-Id", id)
	req.Header.Set("X-Kypost-Device-Secret", secret)
	rec := httptest.NewRecorder()
	withDeviceAuth(srv.handleClientVersion)(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body=%s", rec.Code, rec.Body.String())
	}
	var got struct {
		LatestVersion string `json:"latestVersion"`
		CheckedAt     string `json:"checkedAt"`
		Error         string `json:"error"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if got.LatestVersion != "0.3.0" || got.CheckedAt == "" || got.Error != "" {
		t.Fatalf("unexpected body: %+v", got)
	}
}

// No device credential, no answer. The body carries nothing per-user, but an
// unauthenticated route is a new anonymous surface and this does not need to
// be one.
func TestClientVersionEndpointRejectsUnauthenticated(t *testing.T) {
	srv := newTestServer(t)
	req := httptest.NewRequest(http.MethodGet, "/api/client/version", nil)
	rec := httptest.NewRecorder()
	withDeviceAuth(srv.handleClientVersion)(rec, req)

	if rec.Code == http.StatusOK {
		t.Fatalf("unauthenticated request was answered %d", rec.Code)
	}
}
```

Add `"encoding/json"` to that file's imports.

`pairNativeDevice(t, srv, userID, deviceID) (id, secret string)` already exists at `server_native_test.go:124-150`: it registers a device directly in the user's state store and returns the credential pair a simulated device presents. Use it rather than faking `deviceAuthFromRequest` — the point of the second test is that the real check runs.

`testUserID` is shorthand for whichever user the fixture created; `authRequest` (`server_native_test.go:101`) gets it via `s.users.List()[0].ID`. Either add a one-line `testUserID` helper doing the same, or inline that call.

- [ ] **Step 2: Run the test to verify it fails**

```bash
go test ./backend/internal/api/ -run TestClientVersionEndpoint -v
```

Expected: FAIL to compile with `undefined: handleClientVersion`.

- [ ] **Step 3: Append the handler to `client_version.go`**

```go
// handleClientVersion reports the newest published Linux client release to a
// paired device, which compares it against its own compiled-in version. It
// never performs network I/O: the value comes from the hourly monitor's
// cache, so a client opening its About screen cannot make this server call
// GitHub.
//
// Device-authenticated rather than public. The body carries nothing per-user,
// but this client is always paired by the time it asks, so there is no reason
// to add an anonymous route to reach it.
func (s *Server) handleClientVersion(w http.ResponseWriter, r *http.Request) {
	_, _, ok, retryAfter := s.deviceAuthFromRequest(r)
	if !ok {
		writeDeviceAuthFailure(w, retryAfter)
		return
	}
	status := s.getLinuxClientStatus()
	checkedAt := ""
	if !status.checkedAt.IsZero() {
		checkedAt = status.checkedAt.Format(time.RFC3339)
	}
	// latestVersion is empty until the first check completes, when nothing has
	// soaked yet, or when the repository has no releases. The client renders
	// an empty value as "no information", never as an error.
	writeJSON(w, http.StatusOK, map[string]any{
		"latestVersion": status.latestVersion,
		"checkedAt":     checkedAt,
		"error":         status.checkErr,
	})
}
```

- [ ] **Step 4: Register the route**

In `backend/internal/api/server.go`, alongside the other `withDeviceAuth` routes (near `:541`):

```go
	mux.HandleFunc("GET /api/client/version", withDeviceAuth(s.handleClientVersion))
```

The marker is mandatory: `TestEveryRouteDeclaresItsAuthModel` fails an unmarked route (`route_auth_markers.go:1-17`).

- [ ] **Step 5: Run the full API test suite**

```bash
cd /home/yoshi/busness.app/kypost-server
go test ./backend/internal/api/
```

Expected: PASS. `TestEveryRouteDeclaresItsAuthModel` and `TestLockOrderIsRespected` must both be green — they are the two that fail on a mis-wired new route.

- [ ] **Step 6: Commit**

```bash
git add backend/internal/api/client_version.go backend/internal/api/client_version_test.go \
        backend/internal/api/server.go
git commit -m "feat(api): serve the newest Linux client release to paired devices"
```

---

### Task 4: Client — `VersionCompare`

**Files:**
- Create: `core/version/VersionCompare.h`, `core/version/VersionCompare.cpp` (**`kypost-Linux` repo**)
- Create: `tests/core/version/VersionCompareTest.cpp`
- Modify: `core/CMakeLists.txt` (source list, near `:117`)
- Modify: `tests/CMakeLists.txt` (register the test)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `namespace VersionCompare { bool isNewer(const QString& latest, const QString& installed); }`
  - Task 6 calls `VersionCompare::isNewer`.

- [ ] **Step 1: Write the failing test**

Create `tests/core/version/VersionCompareTest.cpp`:

```cpp
#include "version/VersionCompare.h"

#include <QTest>

class VersionCompareTest : public QObject
{
    Q_OBJECT

private slots:
    void comparesDottedNumericVersions_data();
    void comparesDottedNumericVersions();
    void refusesAnythingThatIsNotThreeNumbers_data();
    void refusesAnythingThatIsNotThreeNumbers();
};

void VersionCompareTest::comparesDottedNumericVersions_data()
{
    QTest::addColumn<QString>("latest");
    QTest::addColumn<QString>("installed");
    QTest::addColumn<bool>("expected");

    QTest::newRow("newer patch") << "0.2.1" << "0.2.0" << true;
    QTest::newRow("newer minor") << "0.3.0" << "0.2.9" << true;
    QTest::newRow("newer major") << "1.0.0" << "0.99.99" << true;
    QTest::newRow("equal") << "0.2.0" << "0.2.0" << false;
    QTest::newRow("older") << "0.1.0" << "0.2.0" << false;
    // Numeric, not lexicographic: "10" sorts before "9" as text.
    QTest::newRow("ten beats nine numerically") << "0.10.0" << "0.9.0" << true;
    // The tags carry a leading v and KYPOST_VERSION does not. Both sides have
    // to reach the same form or every comparison is against a malformed
    // string.
    QTest::newRow("leading v on latest") << "v0.3.0" << "0.2.0" << true;
    QTest::newRow("leading v on both") << "v0.3.0" << "v0.2.0" << true;
}

void VersionCompareTest::comparesDottedNumericVersions()
{
    QFETCH(QString, latest);
    QFETCH(QString, installed);
    QFETCH(bool, expected);
    QCOMPARE(VersionCompare::isNewer(latest, installed), expected);
}

void VersionCompareTest::refusesAnythingThatIsNotThreeNumbers_data()
{
    QTest::addColumn<QString>("latest");
    QTest::addColumn<QString>("installed");

    // v0.1-alpha is in this repository's tag list today. Parsing it
    // best-effort as 0.1.0 would be a silent wrong answer; refusing it is a
    // visible "no information".
    QTest::newRow("alpha tag") << "v0.1-alpha" << "0.2.0";
    QTest::newRow("two components") << "0.3" << "0.2.0";
    QTest::newRow("four components") << "0.3.0.1" << "0.2.0";
    QTest::newRow("non-numeric component") << "0.x.0" << "0.2.0";
    QTest::newRow("empty latest") << "" << "0.2.0";
    QTest::newRow("empty installed") << "0.3.0" << "";
    QTest::newRow("malformed installed") << "0.3.0" << "not-a-version";
    QTest::newRow("negative") << "-1.0.0" << "0.2.0";
    QTest::newRow("trailing suffix") << "0.3.0-rc1" << "0.2.0";
}

void VersionCompareTest::refusesAnythingThatIsNotThreeNumbers()
{
    QFETCH(QString, latest);
    QFETCH(QString, installed);
    // Refusing means "no update", never a crash and never a true.
    QCOMPARE(VersionCompare::isNewer(latest, installed), false);
}

QTEST_MAIN(VersionCompareTest)
#include "VersionCompareTest.moc"
```

- [ ] **Step 2: Register the test and run it to verify it fails**

In `tests/CMakeLists.txt`, alongside the other `kypost_add_test` lines:

```cmake
# core/version: the dotted-numeric compare behind the update notice. Pure, no
# network -- the check that decides whether a user is told they are behind.
kypost_add_test(VersionCompareTest core/version/VersionCompareTest.cpp)
```

```bash
cd /home/yoshi/busness.app/kypost-Linux
cmake -S . -B build-e1 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-e1 --target VersionCompareTest
```

Expected: FAIL to compile — `version/VersionCompare.h: No such file or directory`.

- [ ] **Step 3: Write `core/version/VersionCompare.h`**

```cpp
#pragma once

#include <QString>

// Dotted-numeric version comparison for the update notice. A port of the
// server's ghrelease.IsNewer/parseVersion, kept identical in behaviour so the
// two channels cannot disagree about what "newer" means.
namespace VersionCompare {

// True only when `latest` is a strictly newer N.N.N version than `installed`.
//
// A leading "v" is stripped from either side: release tags are written
// "v0.3.0" and KYPOST_VERSION is written "0.2.0".
//
// Anything that is not three non-negative integers is REFUSED rather than
// parsed best-effort, and a refusal returns false. Best-effort parsing of a
// tag like "v0.1-alpha" produces a confident wrong answer, and the failure
// mode of a wrong answer here is either nagging a current user forever or
// silently never telling a stale one.
bool isNewer(const QString& latest, const QString& installed);

} // namespace VersionCompare
```

- [ ] **Step 4: Write `core/version/VersionCompare.cpp`**

```cpp
#include "version/VersionCompare.h"

#include <QStringList>

namespace {

// Fills `out` with exactly three non-negative components, or returns false.
bool parseVersion(const QString& version, int (&out)[3])
{
    QString trimmed = version.trimmed();
    if (trimmed.startsWith(QLatin1Char('v')))
        trimmed = trimmed.mid(1);

    const QStringList parts = trimmed.split(QLatin1Char('.'));
    if (parts.size() != 3)
        return false;

    for (int i = 0; i < 3; ++i) {
        // QString::toInt accepts a leading '+'/'-' and surrounding space, none
        // of which belongs in a version, so check the shape before converting.
        const QString& part = parts.at(i);
        if (part.isEmpty())
            return false;
        for (const QChar c : part) {
            if (!c.isDigit())
                return false;
        }
        bool ok = false;
        out[i] = part.toInt(&ok);
        if (!ok)
            return false;
    }
    return true;
}

} // namespace

namespace VersionCompare {

bool isNewer(const QString& latest, const QString& installed)
{
    int latestParts[3];
    int installedParts[3];
    if (!parseVersion(latest, latestParts) || !parseVersion(installed, installedParts))
        return false;

    for (int i = 0; i < 3; ++i) {
        if (latestParts[i] != installedParts[i])
            return latestParts[i] > installedParts[i];
    }
    return false;
}

} // namespace VersionCompare
```

- [ ] **Step 5: Add the sources to `core/CMakeLists.txt`**

In the `add_library(kypostcore STATIC ...)` list:

```cmake
  version/VersionCompare.cpp
  version/VersionCompare.h
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cd /home/yoshi/busness.app/kypost-Linux
cmake --build build-e1 --target VersionCompareTest
ctest --test-dir build-e1 -R VersionCompareTest --output-on-failure
```

Expected: PASS, 17 data rows across the two test functions.

- [ ] **Step 7: Commit**

```bash
git checkout -b feat/update-check
git add core/version/VersionCompare.h core/version/VersionCompare.cpp \
        tests/core/version/VersionCompareTest.cpp core/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(version): compare dotted-numeric versions, refusing anything else"
```

---

### Task 5: Client — `ClientVersionClient`

**Files:**
- Create: `core/net/ClientVersionClient.h`, `core/net/ClientVersionClient.cpp`
- Create: `tests/core/net/ClientVersionClientTest.cpp`
- Modify: `core/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `HttpClient::get(url, query, headers)` returning `HttpResult{error, statusCode, body, detail}`; `RelayAuth::headerItems()`; `joinUrlPath(serverBaseUrl, path)` (already used by `ContactSyncClient.cpp:215`).
- Produces:

```cpp
struct ClientVersionResult
{
    QString latestVersion;      // empty when the server has nothing to report
    QString checkedAt;          // RFC3339, as sent
    bool supported = true;      // false when the server has no such endpoint
    std::optional<NetworkError> error;
};
class ClientVersionClient { public: explicit ClientVersionClient(HttpClient&);
    ClientVersionResult fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const; };
```
  Task 6 calls `fetch`.

- [ ] **Step 1: Write the failing test**

Create `tests/core/net/ClientVersionClientTest.cpp`. It uses the repo's existing `FakeRelayServer` harness (`tests/core/net/FakeRelayServer.h`) exactly as `ContactSyncClientTest.cpp:266-285` does.

```cpp
#include "net/ClientVersionClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"
#include "FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTest>

class ClientVersionClientTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesLatestVersionAndSendsDeviceHeaders();
    void treatsMissingEndpointAsUnsupportedRatherThanError();
    void treatsEmptyLatestVersionAsNoInformation();
    void reportsTransportFailureAsError();
};

void ClientVersionClientTest::parsesLatestVersionAndSendsDeviceHeaders()
{
    FakeRelayServer fake(httpResponse(200, "OK",
        R"({"latestVersion":"0.3.0","checkedAt":"2026-08-25T12:00:00Z","error":""})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ClientVersionClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-9"), QStringLiteral("secret-9") };
    const ClientVersionResult result = client.fetch(serverBaseUrl, auth);

    QVERIFY(!result.error.has_value());
    QVERIFY(result.supported);
    QCOMPARE(result.latestVersion, QStringLiteral("0.3.0"));
    QCOMPARE(result.checkedAt, QStringLiteral("2026-08-25T12:00:00Z"));

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("GET /api/client/version"));
    QVERIFY(request.contains("X-Kypost-Device-Id: device-9"));
    QVERIFY(request.contains("X-Kypost-Device-Secret: secret-9"));
}

// A server on 0.3.0 has no such route. That is not an error condition: it is
// the entire population of servers in the field before this ships, and every
// one of them would otherwise show a permanent failure on the About screen.
void ClientVersionClientTest::treatsMissingEndpointAsUnsupportedRatherThanError()
{
    FakeRelayServer fake(httpResponse(404, "Not Found", R"({"error":"not found"})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ClientVersionClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const ClientVersionResult result = client.fetch(
        serverBaseUrl, RelayAuth{ QStringLiteral("d"), QStringLiteral("s") });

    QVERIFY(!result.error.has_value());
    QVERIFY(!result.supported);
    QVERIFY(result.latestVersion.isEmpty());
}

// The server sends an empty latestVersion before its first check completes,
// while a release is still soaking, and when the repo has no releases.
void ClientVersionClientTest::treatsEmptyLatestVersionAsNoInformation()
{
    FakeRelayServer fake(httpResponse(200, "OK",
        R"({"latestVersion":"","checkedAt":"","error":""})"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ClientVersionClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const ClientVersionResult result = client.fetch(
        serverBaseUrl, RelayAuth{ QStringLiteral("d"), QStringLiteral("s") });

    QVERIFY(!result.error.has_value());
    QVERIFY(result.supported);
    QVERIFY(result.latestVersion.isEmpty());
}

void ClientVersionClientTest::reportsTransportFailureAsError()
{
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ClientVersionClient client(http);

    // Port 1 with nothing listening: a connection refusal, not an HTTP status.
    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:1"));
    const ClientVersionResult result = client.fetch(
        serverBaseUrl, RelayAuth{ QStringLiteral("d"), QStringLiteral("s") });

    QVERIFY(result.error.has_value());
}

QTEST_MAIN(ClientVersionClientTest)
#include "ClientVersionClientTest.moc"
```

- [ ] **Step 2: Register the test and run it to verify it fails**

In `tests/CMakeLists.txt`, next to `kypost_add_test(ContactSyncClientTest ...)`:

```cmake
kypost_add_test(ClientVersionClientTest core/net/ClientVersionClientTest.cpp)
```

```bash
cd /home/yoshi/busness.app/kypost-Linux
cmake --build build-e1 --target ClientVersionClientTest
```

Expected: FAIL to compile — `net/ClientVersionClient.h: No such file or directory`.

- [ ] **Step 3: Write `core/net/ClientVersionClient.h`**

```cpp
#pragma once

#include "net/NetworkError.h"

#include <QString>
#include <QUrl>
#include <optional>

class HttpClient;
struct RelayAuth;

// What the paired server reports about the newest published Linux client
// release. This client does NOT decide whether an update is available: the
// comparison happens against the compiled-in KYPOST_VERSION, which is the
// left-hand side and stays on this side of the wire.
struct ClientVersionResult
{
    // Empty when the server has nothing to report: before its first check
    // completes, while the newest release is still inside its soak window, or
    // when the repository has published no releases. All ordinary states.
    QString latestVersion;
    // RFC3339, exactly as the server sent it. Empty if never checked.
    QString checkedAt;
    // False when the server has no such endpoint (404). Every server released
    // before this feature is in that state, so it must read as "no
    // information" rather than as a failure.
    bool supported = true;
    std::optional<NetworkError> error;
};

// GET {serverBaseUrl}/api/client/version.
//
// Goes to the paired server, NOT to GitHub. That is what keeps the update
// check inside the certificate pin and adds no third-party egress -- see
// docs/superpowers/specs/2026-08-25-linux-update-check-design.md.
class ClientVersionClient
{
public:
    explicit ClientVersionClient(HttpClient& httpClient);
    ClientVersionResult fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const;

private:
    HttpClient& m_httpClient;
};
```

- [ ] **Step 4: Write `core/net/ClientVersionClient.cpp`**

```cpp
#include "net/ClientVersionClient.h"

// joinUrlPath is declared in HttpClient.h (:380), not in a header of its own.
#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include <QJsonDocument>
#include <QJsonObject>

ClientVersionClient::ClientVersionClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

ClientVersionResult ClientVersionClient::fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const
{
    const HttpClient::HttpResult result = m_httpClient.get(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/client/version")), {}, auth.headerItems());

    ClientVersionResult out;

    // Checked before the error branch: HttpClient reports a 404 as an error,
    // and an older server having no such route is not one.
    if (result.statusCode == 404) {
        out.supported = false;
        return out;
    }
    if (result.error.has_value()) {
        out.error = result.error;
        return out;
    }

    const QJsonObject json = QJsonDocument::fromJson(result.body).object();
    out.latestVersion = json.value(QStringLiteral("latestVersion")).toString();
    out.checkedAt = json.value(QStringLiteral("checkedAt")).toString();
    return out;
}
```

- [ ] **Step 5: Add the sources to `core/CMakeLists.txt`**

```cmake
  net/ClientVersionClient.cpp
  net/ClientVersionClient.h
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build build-e1 --target ClientVersionClientTest
ctest --test-dir build-e1 -R ClientVersionClientTest --output-on-failure
```

Expected: PASS, 4 tests. If `treatsMissingEndpointAsUnsupportedRatherThanError` fails because `statusCode` is 0 on the error path, read `HttpClient::get`'s error handling and key off whatever it actually populates — do not weaken the test to match a guess.

- [ ] **Step 7: Commit**

```bash
git add core/net/ClientVersionClient.h core/net/ClientVersionClient.cpp \
        tests/core/net/ClientVersionClientTest.cpp core/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(net): read the newest client release from the paired server"
```

---

### Task 6: Client — `UpdateCheckController`

**Files:**
- Create: `app/update/UpdateCheckController.h`, `app/update/UpdateCheckController.cpp`
- Create: `tests/app/update/UpdateCheckControllerTest.cpp`
- Modify: `app/CMakeLists.txt`, `tests/CMakeLists.txt`
- Modify: `app/main.cpp` (register the singleton, near `:1071-1073`)

**Interfaces:**
- Consumes: `VersionCompare::isNewer` (Task 4); `ClientVersionClient::fetch` (Task 5); `NetworkExecutor::run(receiver, work, onDone)` (`core/net/NetworkExecutor.h`); `PairingStore`.
- Produces: QML singleton `UpdateCheck` with properties `installedVersion`, `latestVersion`, `updateAvailable`, `checkedAt`, `releaseUrl` and signal `changed()`. Task 7 binds all five.

- [ ] **Step 1: Write the failing test**

Create `tests/app/update/UpdateCheckControllerTest.cpp`. Keep it to the decision logic — the network path is already covered by Task 5.

```cpp
#include "update/UpdateCheckController.h"

#include "version/VersionCompare.h"

#include <QTest>

class UpdateCheckControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void reportsAnUpdateOnlyWhenTheServerVersionIsStrictlyNewer_data();
    void reportsAnUpdateOnlyWhenTheServerVersionIsStrictlyNewer();
    void installedVersionIsTheCompiledInConstant();
};

void UpdateCheckControllerTest::reportsAnUpdateOnlyWhenTheServerVersionIsStrictlyNewer_data()
{
    QTest::addColumn<QString>("latest");
    QTest::addColumn<QString>("installed");
    QTest::addColumn<bool>("expected");

    QTest::newRow("newer") << "0.3.0" << "0.2.0" << true;
    QTest::newRow("equal") << "0.2.0" << "0.2.0" << false;
    QTest::newRow("older") << "0.1.0" << "0.2.0" << false;
    // The server sends an empty string when it has nothing to report. That
    // must never render as an update.
    QTest::newRow("nothing reported") << "" << "0.2.0" << false;
}

void UpdateCheckControllerTest::reportsAnUpdateOnlyWhenTheServerVersionIsStrictlyNewer()
{
    QFETCH(QString, latest);
    QFETCH(QString, installed);
    QFETCH(bool, expected);
    QCOMPARE(VersionCompare::isNewer(latest, installed), expected);
}

// The left-hand side of the comparison must come from the build, not from a
// second hand-maintained copy. A constant that drifts from the tag makes a
// current install nag forever -- see the spec and server_version.go:12-28.
void UpdateCheckControllerTest::installedVersionIsTheCompiledInConstant()
{
    QCOMPARE(UpdateCheckController::compiledInVersion(), QStringLiteral(KYPOST_VERSION));
}

QTEST_MAIN(UpdateCheckControllerTest)
#include "UpdateCheckControllerTest.moc"
```

- [ ] **Step 2: Register the test and run it to verify it fails**

In `tests/CMakeLists.txt`, using the app-test helper (this test needs headers under `app/`):

```cmake
kypost_add_app_test(UpdateCheckControllerTest
  SOURCES app/update/UpdateCheckControllerTest.cpp
          ${CMAKE_SOURCE_DIR}/app/update/UpdateCheckController.cpp)
```

```bash
cmake --build build-e1 --target UpdateCheckControllerTest
```

Expected: FAIL to compile — `update/UpdateCheckController.h: No such file or directory`.

- [ ] **Step 3: Write `app/update/UpdateCheckController.h`**

```cpp
#pragma once

#include "net/ClientVersionClient.h"

#include <QObject>
#include <QString>
#include <QTimer>

class NetworkExecutor;
class PairingStore;

// Asks the paired server what the newest published Linux release is, and
// compares it against this build.
//
// SURFACES, NEVER ACTS. A Flatpak cannot update itself, so this produces a
// notice and a link and nothing else. Nothing here downloads, installs, or
// touches the host.
class UpdateCheckController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString installedVersion READ installedVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY changed)
    Q_PROPERTY(QString checkedAt READ checkedAt NOTIFY changed)
    Q_PROPERTY(QString releaseUrl READ releaseUrl CONSTANT)

public:
    UpdateCheckController(PairingStore& pairingStore, NetworkExecutor& executor,
                          QObject* parent = nullptr);

    // The build's own version, and the LEFT-HAND SIDE of every comparison.
    // Exposed as a static so a test can assert it tracks KYPOST_VERSION
    // rather than a second copy.
    static QString compiledInVersion();

    QString installedVersion() const { return compiledInVersion(); }
    QString latestVersion() const { return m_latestVersion; }
    bool updateAvailable() const { return m_updateAvailable; }
    QString checkedAt() const { return m_checkedAt; }
    QString releaseUrl() const;

    Q_INVOKABLE void checkNow();
    void pairingMayHaveChanged();

signals:
    void changed();
    // Raised on the transition into "an update is available", so a root can
    // show a toast without polling the property.
    void updateBecameAvailable();

private:
    void applyResult(const ClientVersionResult& result);

    PairingStore& m_pairingStore;
    NetworkExecutor& m_executor;
    QTimer m_pollTimer;
    QString m_latestVersion;
    QString m_checkedAt;
    bool m_updateAvailable = false;
};
```

- [ ] **Step 4: Write `app/update/UpdateCheckController.cpp`**

Read `app/pgp/PgpEnrollmentController.cpp:100-120` first and mirror its `m_executor.run(...)` shape — `run()` returns immediately, `work` gets an `HttpClient&` on the executor thread, and `onDone` lands back on this object's thread.

```cpp
#include "update/UpdateCheckController.h"

#include "domain/PairingStore.h"
#include "net/HttpClient.h"
#include "net/NetworkExecutor.h"
#include "version/VersionCompare.h"

namespace {
// Matches the server's own hourly release-check tick. Nothing is gained by
// asking more often: the server serves a cache that only moves hourly.
constexpr int kPollIntervalMs = 60 * 60 * 1000;

// The page a user is sent to. Not an API endpoint -- this is for a human.
constexpr auto kReleasesPage = "https://github.com/Busness-app/KyPost-for-Linux/releases";
} // namespace

QString UpdateCheckController::compiledInVersion()
{
    return QStringLiteral(KYPOST_VERSION);
}

QString UpdateCheckController::releaseUrl() const
{
    return QStringLiteral(kReleasesPage);
}

UpdateCheckController::UpdateCheckController(PairingStore& pairingStore, NetworkExecutor& executor,
                                             QObject* parent)
    : QObject(parent)
    , m_pairingStore(pairingStore)
    , m_executor(executor)
{
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &UpdateCheckController::checkNow);
    m_pollTimer.start();
}

void UpdateCheckController::checkNow()
{
    // load(), not loadChecked(). The three-state form exists for callers
    // making a security decision from "is this device paired", where an
    // unreadable keyring must not read as "not paired" (PairingStore.h:39-41).
    // This is not one of those: an unreadable store and an unpaired device
    // both mean "nobody to ask about updates right now", and the next hourly
    // tick retries either way.
    //
    // If this device is not paired there is nobody to ask, and there is no
    // fallback to GitHub by design.
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return;

    const QUrl serverBaseUrl(pairing->serverBaseUrl);
    const RelayAuth auth{ pairing->deviceId, pairing->deviceSecret };
    m_executor.run(
        this,
        [serverBaseUrl, auth](HttpClient& http) {
            return ClientVersionClient(http).fetch(serverBaseUrl, auth);
        },
        [this](ClientVersionResult result) { applyResult(result); });
}

void UpdateCheckController::applyResult(const ClientVersionResult& result)
{
    // Failures and unsupported servers are dropped, not surfaced. This runs
    // unattended on a timer, an unreachable self-hosted server is routine,
    // and the next tick retries -- the same reasoning the server gives for
    // its own check. A server too old to have the endpoint is not a fault at
    // all. In both cases the last known-good values stay on screen.
    if (result.error.has_value() || !result.supported)
        return;

    const bool wasAvailable = m_updateAvailable;
    m_latestVersion = result.latestVersion;
    m_checkedAt = result.checkedAt;
    m_updateAvailable = VersionCompare::isNewer(m_latestVersion, compiledInVersion());
    emit changed();
    if (m_updateAvailable && !wasAvailable)
        emit updateBecameAvailable();
}

void UpdateCheckController::pairingMayHaveChanged()
{
    checkNow();
}
```

`PairingStore` lives at `core/domain/PairingStore.h` and `DevicePairing` at `core/domain/DevicePairing.h` — note `serverBaseUrl` is a `QString` there, hence the `QUrl` construction.

- [ ] **Step 5: Add sources to `app/CMakeLists.txt`**

```cmake
  update/UpdateCheckController.cpp
  update/UpdateCheckController.h
```

- [ ] **Step 6: Register the QML singleton in `app/main.cpp`**

Next to the `PgpEnrollmentController` registration at `:1071-1073`:

```cpp
    UpdateCheckController updateCheckController(pairingStore, networkExecutor);
    qmlRegisterSingletonInstance<UpdateCheckController>(
        "com.kysecurity.mail", 1, 0, "UpdateCheck", &updateCheckController);
    QObject::connect(&pairingController, &PairingController::pairingChanged,
                     &updateCheckController, &UpdateCheckController::pairingMayHaveChanged);
```

Add `#include "update/UpdateCheckController.h"` alongside the other controller includes near `:9`. Then trigger the startup check once the app is up, next to the existing `QTimer::singleShot(400, ...)` at `:414`:

```cpp
    QTimer::singleShot(0, &updateCheckController, [&updateCheckController]() {
        updateCheckController.checkNow();
    });
```

- [ ] **Step 7: Run the test and build the app**

```bash
cmake --build build-e1 --target UpdateCheckControllerTest kypost
ctest --test-dir build-e1 -R UpdateCheckControllerTest --output-on-failure
```

Expected: PASS, and `kypost` links.

- [ ] **Step 8: Commit**

```bash
git add app/update/ tests/app/update/ app/CMakeLists.txt tests/CMakeLists.txt app/main.cpp
git commit -m "feat(update): compare this build against what the server reports"
```

---

### Task 7: Client — the About section and the startup toast

**Files:**
- Modify: `app/qml/pages/Settings.qml`
- Modify: `app/qml/DesktopRoot.qml` (near the `StatusBanner` at `:1553`), `app/qml/MobileRoot.qml`
- Create: `tests/qml/tst_UpdateNotice.qml`
- Modify: `tests/qml/FakeSingletons.h` (add a `FakeUpdateCheck`)

No `tests/CMakeLists.txt` change: the `QmlTests` target discovers `tst_*.qml` through `QUICK_TEST_SOURCE_DIR` (`tests/CMakeLists.txt:370-372`), so a new file is picked up automatically. What is **not** automatic is the singleton — the runner registers its own fakes for `com.kysecurity.mail` (`tests/qml/FakeSingletons.h`), so a QML file binding `UpdateCheck` fails until a fake exists.

**Interfaces:**
- Consumes: the `UpdateCheck` QML singleton from Task 6 — `installedVersion`, `latestVersion`, `updateAvailable`, `checkedAt`, `releaseUrl`, `updateBecameAvailable()`.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Write the failing QML test**

Create `tests/qml/tst_UpdateNotice.qml`, following the existing files in `tests/qml/` for their `TestCase` conventions.

```qml
import QtQuick 2.15
import QtTest 1.15
import com.kysecurity.mail 1.0

TestCase {
    name: "UpdateNotice"
    when: windowShown

    // The version on screen must be the build's own, not a literal someone
    // has to remember to edit.
    function test_installedVersionIsPresent() {
        verify(UpdateCheck.installedVersion.length > 0)
    }

    // No update, no notice. A user who is current must not see an update row
    // at all -- a permanently-present "you might be behind" is noise.
    function test_noNoticeWhenCurrent() {
        compare(UpdateCheck.updateAvailable, false)
    }

    function test_releaseUrlPointsAtTheReleasesPage() {
        verify(UpdateCheck.releaseUrl.indexOf("KyPost-for-Linux/releases") >= 0)
    }
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build-e1 --target QmlTests
ctest --test-dir build-e1 -R QmlTests --output-on-failure
```

Expected: FAIL — `UpdateCheck is not defined`, because the runner has no such fake singleton yet.

- [ ] **Step 3: Add `FakeUpdateCheck` to `tests/qml/FakeSingletons.h`**

Follow the file's existing convention: only the members components actually bind to, `MEMBER`-backed and writable so a test can drive them.

```cpp
class FakeUpdateCheck : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString installedVersion MEMBER m_installedVersion CONSTANT)
    Q_PROPERTY(QString latestVersion MEMBER m_latestVersion NOTIFY changed)
    Q_PROPERTY(bool updateAvailable MEMBER m_updateAvailable NOTIFY changed)
    Q_PROPERTY(QString checkedAt MEMBER m_checkedAt NOTIFY changed)
    Q_PROPERTY(QString releaseUrl MEMBER m_releaseUrl CONSTANT)

public:
    using QObject::QObject;

signals:
    void changed();
    void updateBecameAvailable();

private:
    QString m_installedVersion = QStringLiteral("0.2.0");
    QString m_latestVersion;
    bool m_updateAvailable = false;
    QString m_checkedAt;
    QString m_releaseUrl =
        QStringLiteral("https://github.com/Busness-app/KyPost-for-Linux/releases");
};
```

Register it as `"UpdateCheck"` alongside the other fakes in `tests/qml/main.cpp` — copy the registration line used for `AppLock` and change the type and name.

Every property the QML binds must exist here. `QmlPropertyLint` (`tests/CMakeLists.txt:385-402`) fails a `.qml` file that assigns a property that does not exist, so a missing one is caught, but only if the fake is complete.

- [ ] **Step 4: Add the About section to `Settings.qml`**

Place it at the end of the settings column, following the existing `SectionLabel` + content pattern used by the surrounding sections.

```qml
SectionLabel { text: i18n("About") }

ColumnLayout {
    Layout.fillWidth: true
    spacing: 6

    Text {
        textFormat: Text.PlainText
        text: i18n("KyPost %1", UpdateCheck.installedVersion)
        color: Theme.inkStrong
        font.family: Theme.fontUi
        font.pixelSize: 14
    }

    // Three distinct states, deliberately worded apart: an update exists, we
    // are current, or we do not know. "We do not know" is the state of every
    // client paired to a server too old to answer, and it must not read as a
    // failure.
    Text {
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: UpdateCheck.updateAvailable ? Theme.accent : Theme.inkMuted
        font.family: Theme.fontUi
        font.pixelSize: 12
        text: UpdateCheck.updateAvailable
              ? i18n("Version %1 is available.", UpdateCheck.latestVersion)
              : UpdateCheck.latestVersion.length > 0
                ? i18n("This is the newest released version.")
                : i18n("No update information available from this server.")
    }

    // When the check last succeeded. Without it, "This is the newest released
    // version" is indistinguishable from a check that has been silently
    // failing for a month -- failures are dropped rather than surfaced, so
    // this timestamp is the only evidence the user has that the answer is
    // fresh.
    Text {
        textFormat: Text.PlainText
        visible: UpdateCheck.checkedAt.length > 0
        text: i18n("Last checked %1", UpdateCheck.checkedAt)
        color: Theme.inkMuted
        font.family: Theme.fontUi
        font.pixelSize: 11
    }

    GhostButton {
        visible: UpdateCheck.updateAvailable
        text: i18n("Open the releases page…")
        onClicked: Qt.openUrlExternally(UpdateCheck.releaseUrl)
    }

    MutedHint {
        Layout.fillWidth: true
        visible: UpdateCheck.updateAvailable
        text: i18n("KyPost cannot update itself. Download the new version from the "
                   + "releases page, or update through your package manager.")
    }
}
```

The last hint is not optional politeness: a Flatpak user who expects the button to install something will otherwise report the button as broken.

- [ ] **Step 5: Add the startup toast to both roots**

In `DesktopRoot.qml`, near the existing `StatusBanner` at `:1553` — **as a separate `Toast`, not a `StatusBanner` row**:

```qml
    // Deliberately a Toast rather than a StatusBanner row. StatusBanner is
    // danger-styled and cannot be dismissed, and is reserved for conditions
    // that stop the app working. A routine "a new version exists" painted in
    // the same red, unclearable strip would devalue the warnings that matter.
    Toast {
        id: updateToast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        z: 940
    }

    Connections {
        target: UpdateCheck
        function onUpdateBecameAvailable() {
            updateToast.show(i18n("KyPost %1 is available.", UpdateCheck.latestVersion))
        }
    }
```

Repeat verbatim in `MobileRoot.qml`. The signal fires on the transition into "available", which is once per app run per version — the startup check raises it, and the hourly tick raises it again only if a newer release appears while the app is open.

- [ ] **Step 6: Run the QML test and the full suite**

```bash
cmake --build build-e1
ctest --test-dir build-e1 --output-on-failure
```

Expected: PASS, whole suite. A pre-existing failure unrelated to this change is not a pass — check it against `main` before dismissing it.

- [ ] **Step 7: Verify against the running app, not the test**

```bash
cmake --build build-e1 --target kypost
./build-e1/app/kypost
```

Open Settings, scroll to About. Confirm the installed version matches `project(KyPost VERSION ...)` in `CMakeLists.txt`. Against a server without the endpoint you must see "No update information available from this server." and **no error**. This step is the deliverable: the tests do not prove the section renders.

- [ ] **Step 8: Commit**

```bash
git add app/qml/pages/Settings.qml app/qml/DesktopRoot.qml app/qml/MobileRoot.qml \
        tests/qml/tst_UpdateNotice.qml tests/qml/FakeSingletons.h tests/qml/main.cpp
git commit -m "feat(settings): show the installed version and any newer release"
```

---

### Task 8: File Bug 2 as a tracked issue

Linux never reads `pin=` from the pairing URI, so it cannot fail closed on a mismatch as `PLATFORM_BASELINE.md` §1 requires. It uses TOFU instead. This is the status quo and the documented fallback, and honouring the pin means adding a fail-closed path to registration — the one flow whose failure mode is "nobody can pair at all". Not a change to make days before launch.

**Files:** none. This creates a GitHub issue.

**Interfaces:** none.

- [ ] **Step 1: Confirm the finding still holds**

```bash
cd /home/yoshi/busness.app/kypost-Linux
grep -rn "\"pin\"" app core
```

Expected: no output. If that changes, the issue text below is wrong — re-verify before filing.

- [ ] **Step 2: File the issue**

```bash
gh issue create --repo Busness-app/KyPost-for-Linux \
  --title "Honour pin= from the pairing URI, and fail closed on a mismatch" \
  --body 'The server publishes a certificate pin in the pairing URI. Android honours it. Linux does not read it.

`app/pairing/PairingController.cpp:130-148` parses the URI with `QUrlQuery`, requires `sub`/`srv`/`pt`, reads `reg` optionally, and ignores unknown parameters. An unrecognised `pin=` is silently dropped, so **pairing is not broken** by the server publishing one.

What Linux does instead is trust-on-first-use: it captures the SPKI of whatever certificate served registration (`core/net/NativeRegistrationClient.h:62-70`) and pins later calls to it via `core/net/CertificatePinSink.cpp`.

The gap: the pairing request is the one call carrying the pairing token, the push endpoint and the push credentials together, and TOFU trusts the certificate only *after* those are disclosed. On a network with a locally trusted CA — enterprise MDM, a user-installed root, a hostile captive portal — an interceptor reads the token and registers its own device first. The pin exists so a client can pin *before* disclosing.

`docs/PLATFORM_BASELINE.md` §1 requires a client that reads `pin` to **fail closed** on a mismatch. Linux cannot fail closed on something it never reads.

**Not launch-blocking.** It is the status quo, and TOFU is the documented fallback. Filed so the difference between channels is tracked rather than unrecorded.

Target: 0.4.0. The work touches the registration path, whose failure mode is "nobody can pair", so it needs its own testing pass rather than riding along with a release.

Context: `docs/superpowers/specs/2026-08-25-linux-update-check-design.md`, "Bug 2".' \
  --label security
```

If the `security` label does not exist in the repo, drop the `--label` flag rather than creating one.

- [ ] **Step 3: Record the issue number in the spec**

Add the resulting issue URL under the spec's "Bug 2" heading so the two are linked, then commit.

```bash
git add docs/superpowers/specs/2026-08-25-linux-update-check-design.md
git commit -m "docs(spec): link the filed pin= issue"
```

---

## Verification before calling this done

- [ ] `cd /home/yoshi/busness.app/kypost-server && go test ./backend/internal/api/` passes, including `TestEveryRouteDeclaresItsAuthModel` and `TestLockOrderIsRespected`.
- [ ] `cd /home/yoshi/busness.app/kypost-Linux && ctest --test-dir build-e1 --output-on-failure` passes.
- [ ] The running app's About section shows the version from `CMakeLists.txt`, and reads as "no information" — not an error — against a server without the endpoint.
- [ ] The server ships before the client.
