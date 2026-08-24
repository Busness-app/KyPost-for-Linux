# Distribution: the self-hosted Flatpak channel

KyPost is distributed from its own signed Flatpak remote, published to this
repository's `gh-pages` branch by `.github/workflows/flatpak.yml`.

> **Status, 2026-08-24: nothing has been published yet.**
> `FLATPAK_GPG_PRIVATE_KEY` is not configured, so `steps.mode.outputs.publish` is
> false on every run: there is no `gh-pages` branch, no GitHub Release, and both
> `.github.io` URLs below return 404. A green build is not a shipped app. The
> three one-time maintainer steps that change this are in the next section.
>
> **Also unfixed: `PAGES_URL` in `.github/workflows/flatpak.yml` still names the
> pre-transfer owner** (`https://yoshiofthewire.github.io/KyPost-for-Linux`). The
> repository now lives at `Busness-app/KyPost-for-Linux` — the old URL 301s on
> github.com but GitHub Pages does not follow a transfer, so Pages will serve at
> `busness-app.github.io`, which is what this document now says. The workflow bakes
> `PAGES_URL` into the generated `.flatpakrepo`'s `Url=` field, so the first publish
> would advertise a host that does not serve the repo. Fix that one line before
> step 2, or every early adopter adds a remote that cannot update.

**KyPost will never be on Flathub.** Flathub bans applications that use AI, and
KyPost's backend does (the Ollama-backed classifier). That is a policy ban, not a
packaging gap — no amount of manifest compliance changes it. Don't spend effort on
Flathub-specific requirements or `flathub/` scaffolding.

Because there is no store, this repo *is* the distribution channel. That's why the
workflow publishes an OSTree repository rather than only a single-file bundle: a
`.flatpak` bundle installs once and then never updates, while a remote gives users a
working `flatpak update`.

---

## For users

```sh
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --if-not-exists kypost https://busness-app.github.io/KyPost-for-Linux/kypost.flatpakrepo
flatpak install kypost com.kysecurity.mail
```

Updates are then just `flatpak update`. The Flathub remote is required for the
`org.kde.Platform//6.11` runtime; the app itself is not on Flathub.

The repo is GPG-signed and the public key is embedded in the `.flatpakrepo`, so
`flatpak` refuses anything not signed by the release key.

---

## One-time maintainer setup

Three steps, in this order. The workflow builds and smoke-tests without any of them —
it just skips publishing and logs a warning — so a missing key never breaks CI.

### 1. Generate the signing key

```sh
./packaging/flatpak/gen-signing-key.sh --upload
```

This creates a passphraseless, non-expiring RSA-4096 signing key, writes it to
`~/kypost-flatpak-signing-key/`, and sets the `FLATPAK_GPG_PRIVATE_KEY` and
`FLATPAK_GPG_KEY_ID` repository secrets via `gh`. Omit `--upload` to print the
commands instead of running them.

**Back up `private.asc` offline immediately.** GitHub Actions secrets are write-only —
you cannot read the key back out. Losing it means you cannot sign updates for the
existing remote, and every installed user would have to remove and re-add it by hand.

The key is passphraseless because CI signs unattended; a passphrase stored as a second
secret protects nothing, since an attacker with one secret has both. It never expires
because an expired signing key doesn't just block new releases — it makes
`flatpak update` reject the repo for everyone already installed.

### 2. Run the workflow once

Push to `main`, or trigger it manually:

```sh
gh workflow run flatpak.yml
```

This creates the `gh-pages` branch. It must exist before step 3.

### 3. Enable GitHub Pages

Settings → Pages → Source: *Deploy from a branch* → `gh-pages` / `/ (root)`. Or:

```sh
gh api -X POST repos/Busness-app/KyPost-for-Linux/pages \
  -f 'source[branch]=gh-pages' -f 'source[path]=/'
```

Then verify:

```sh
curl -fsS https://busness-app.github.io/KyPost-for-Linux/kypost.flatpakrepo
flatpak remote-add --user --if-not-exists kypost-test \
  https://busness-app.github.io/KyPost-for-Linux/kypost.flatpakrepo
flatpak install --user kypost-test com.kysecurity.mail
```

---

## How the workflow works

Two jobs. `build` is a matrix over both shipped architectures, each on its own native
runner; `publish` runs once, after both, and is the only thing that touches gh-pages.

| Job | Trigger | Builds | Smoke-tests | Publishes | Release asset |
|---|---|---|---|---|---|
| `build` | every run | both arches | yes | no | no |
| `publish` | `pull_request` | — | — | skipped entirely | no |
| `publish` | `push` to `main` | — | — | yes | no |
| `publish` | `push` tag `v*` | — | — | yes | yes |
| `publish` | nightly / manual | — | — | yes | no |

Every run uploads one `kypost-<arch>.flatpak` bundle per architecture as an artifact.
Tagged runs attach both to the GitHub Release, for people who'd rather not add a remote.

### Architectures

`x86_64` and `aarch64`. Plasma Mobile is a target, so arm64 is not optional.

Each builds natively — `ubuntu-24.04` and `ubuntu-24.04-arm`, both free for public
repos. No qemu: emulating a QtWebEngine build would take hours and would make the
launch smoke test meaningless, since it would no longer be running the code a user
runs.

Users do not choose an arch. `flatpak install` resolves it from the one repo, which
carries both.

Note that **`ci.yml` is x86_64-only**, and deliberately so: it builds natively against
KDE neon's apt archive, which does not carry a complete KF6 set for arm64 (`kf6-ki18n`,
`kf6-knotifications`, `kf6-kcmutils` and `kf6-extra-cmake-modules` among others are
absent, so apt cannot resolve the dependency closure even though the leaf package names
all exist). That costs nothing in coverage: this workflow builds arm64 against
`org.kde.Sdk` from Flathub instead, and the manifest sets `run-tests: true`, so the
whole ctest suite plus the launch smoke test run on real arm64 hardware here.

### How two build jobs become one published repo

Two matrix jobs cannot both force-push `gh-pages` — the workflow-level `concurrency`
group serialises whole runs, not jobs inside one. So the matrix only *builds*, each arch
exporting to its own unsigned OSTree repo that it hands over as an artifact, and the
single `publish` job merges both in with `flatpak build-commit-from` before pushing once.

That merge is also where signing happens, which means **the signing key is never
present in a build job**. It has to be `build-commit-from` rather than
`ostree pull-local`: pull-local copies commits verbatim, so unsigned build output would
stay unsigned, and a client configured with `GPGKey=` verifies the *commit*, not just
the summary — it would refuse to install.

### Why a `gh-pages` branch instead of `actions/deploy-pages`

An OSTree repo must **accumulate**. `flatpak update` computes a delta from the commit
the user already has, so older commits must stay reachable. The artifact-based Pages
deploy replaces the site wholesale each run, which would strand every existing install
on a repo whose history vanished.

So each run clones the currently-published repo, exports the new build *into* it, and
force-pushes the result back as a single **orphan** commit. OSTree keeps its own
history inside the repo (bounded by `--prune-depth=3`), while git history stays one
commit deep so the branch doesn't grow without bound as pruned objects accumulate.

### The empty-directory trap

git cannot store an empty directory, and a fresh OSTree repo has six of them
(`extensions/`, `state/`, `tmp/`, `tmp/cache/`, `refs/mirrors/`, `refs/remotes/`).
Restoring by `git clone` silently drops them, and the *next* publish then dies with:

```
error: Listing refs: opendir(refs/remotes): No such file or directory
```

That failure mode is nasty because the first publish succeeds — it's the first
*update* that breaks. The restore step recreates the directories and runs
`ostree fsck` before building. Verified locally: without the fix the second publish
fails, with it the second publish succeeds and generates a proper v1→v2 static delta.

### Size

GitHub Pages caps a published site at **1 GB**. The runtime is not in this repo (users
get `org.kde.Platform//6.11` from Flathub); it holds only KyPost plus its five
from-source modules (libargon2, qtkeychain, sqlcipher, zxing-cpp, kunifiedpush) — for two
architectures, but without debuginfo (see below). `--prune-depth=3` bounds growth to the three most recent
commits per ref. The publish step logs the site size
on every run — if it approaches the cap, lower `--prune-depth`, or move the repo to
object storage and change `Url=` in the generated `.flatpakrepo`.

### Debuginfo is not published

The merge step drops every `runtime/*.Debug` ref. Debuginfo was the largest thing the
repo carried, and going dual-arch would have meant two copies of it; dropping it is
what keeps two architectures from costing much more than one did.

It is cut at the merge, not with flatpak-builder's `--disable-debuginfo`. Separating
debug symbols out into that ref is *how* flatpak-builder produces stripped binaries, so
turning the mechanism off risks shipping fat ones instead. Filtering afterwards cannot
affect the app ref at all.

Safe for users: the extension is declared `no-autodownload=true` and `autodelete=true`
(verified against the real installed `com.kysecurity.mail` metadata), so no install or
update ever pulls it and its absence cannot break either. Anyone who explicitly
installed `com.kysecurity.mail.Debug` from the remote will simply stop receiving updates
for it. `.Locale` is unaffected — translations still publish.

The first publish after this change also deletes the `.Debug` refs already on gh-pages.
Skipping the new ones only stops the repo growing; the old refs would otherwise keep
their objects alive against `--prune` indefinitely. That deletion is a no-op on every
run after the first. Measured on a synthetic repo: 20M → 272K, fsck clean.

Getting debuginfo back for a build means building the manifest locally
(`packaging/flatpak/build.sh`), which still produces it.

---

## Cutting a release

Three things carry the version and all three must agree, or the build fails before
it publishes anything — `scripts/verify-version.sh` runs first in both workflows.

1. Bump `project(KyPost VERSION ...)` in `CMakeLists.txt`. This is the source of
   truth; it reaches C++ as `KYPOST_VERSION` and ends up in the User-Agent header.
2. Add a matching `<release>` entry, newest first, to
   `packaging/flatpak/com.kysecurity.mail.metainfo.xml`. This decides what Discover
   shows and whether a user is offered the update at all. `appstreamcli validate`
   runs in CI and is blocking.
3. Tag and push:
   ```sh
   git tag -a v0.2.0 -m "KyPost 0.2.0" && git push origin v0.2.0
   ```
   The tag must be `v<version>`, or `v<version>-<prerelease>` for an rc.

The workflow then publishes both architectures to the remote and attaches
`kypost-x86_64.flatpak` and `kypost-aarch64.flatpak` to the release.

Check locally before tagging:

```sh
./scripts/verify-version.sh v0.2.0
```

## Rotating the signing key

Avoid this if at all possible — it is a breaking change. Every user must run
`flatpak remote-delete kypost` and re-add the remote, because `flatpak` will reject a
repo signed by an unknown key. If unavoidable, announce it, then regenerate the key
(delete `~/kypost-flatpak-signing-key/` first, since the script refuses to overwrite)
and re-run the workflow.
