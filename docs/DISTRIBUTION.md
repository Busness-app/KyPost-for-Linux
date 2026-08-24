# Distribution: the self-hosted Flatpak channel

KyPost is distributed from its own signed Flatpak remote, published to this
repository's `gh-pages` branch by `.github/workflows/flatpak.yml`.

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
flatpak remote-add --if-not-exists kypost https://yoshiofthewire.github.io/KyPost-for-Linux/kypost.flatpakrepo
flatpak install kypost com.kysecurity.mail
```

Updates are then just `flatpak update`. The Flathub remote is required for the
`org.kde.Platform//6.10` runtime; the app itself is not on Flathub.

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
gh api -X POST repos/Yoshiofthewire/KyPost-for-Linux/pages \
  -f 'source[branch]=gh-pages' -f 'source[path]=/'
```

Then verify:

```sh
curl -fsS https://yoshiofthewire.github.io/KyPost-for-Linux/kypost.flatpakrepo
flatpak remote-add --user --if-not-exists kypost-test \
  https://yoshiofthewire.github.io/KyPost-for-Linux/kypost.flatpakrepo
flatpak install --user kypost-test com.kysecurity.mail
```

---

## How the workflow works

| Trigger | Builds | Smoke-tests | Publishes | Release asset |
|---|---|---|---|---|
| `pull_request` | yes | yes | no (no secrets on forks) | no |
| `push` to `main` | yes | yes | yes | no |
| `push` tag `v*` | yes | yes | yes | yes |
| nightly / manual | yes | yes | yes | no |

Every run uploads the single-file `kypost.flatpak` bundle as an artifact; tagged runs
also attach it to the GitHub Release, for people who'd rather not add a remote.

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
get `org.kde.Platform//6.10` from Flathub); it holds only KyPost plus its three
from-source modules (qtkeychain, zxing-cpp, kunifiedpush). `--prune-depth=3` bounds
growth to the three most recent commits per ref. The publish step logs the site size
on every run — if it approaches the cap, lower `--prune-depth`, or move the repo to
object storage and change `Url=` in the generated `.flatpakrepo`.

---

## Cutting a release

1. Add a `<release>` entry to `packaging/flatpak/com.kysecurity.mail.metainfo.xml`.
   `appstreamcli validate` runs in CI and is blocking.
2. Tag and push:
   ```sh
   git tag -a v0.2.0 -m "KyPost 0.2.0" && git push origin v0.2.0
   ```

The workflow publishes to the remote and attaches the bundle to the release.

## Rotating the signing key

Avoid this if at all possible — it is a breaking change. Every user must run
`flatpak remote-delete kypost` and re-add the remote, because `flatpak` will reject a
repo signed by an unknown key. If unavoidable, announce it, then regenerate the key
(delete `~/kypost-flatpak-signing-key/` first, since the script refuses to overwrite)
and re-run the workflow.
