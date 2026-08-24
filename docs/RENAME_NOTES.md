# Llama Mail → KyPost rename: what moved, and what is still orphaned

The rename (2026-07-17 … 07-19) changed more than the product name. This is the
record of what actually moved on disk, because one part of it was missed at the
time and the fix has a limit worth knowing about.

## The profile moved twice, not once

Two independent changes, a day apart:

| | Before | After | Commit |
|---|---|---|---|
| `applicationName` | `LlamaMail` | `mail` | `e0dde4e`, 2026-07-17 |
| Database filename | `llamamail.db` | `kypost.db` | rename sweep, 2026-07-18 |

`applicationName` is what `QStandardPaths` derives the profile directories from,
so changing it moved **the whole profile**, not just the database:

```
~/.local/share/LlamaMail/   →  ~/.local/share/mail/     (AppDataLocation: database, contact-photos)
~/.config/LlamaMail/        →  ~/.config/mail/          (AppConfigLocation: settings.ini, cursors.ini)
```

Measured, not assumed — `QStandardPaths::writableLocation(AppDataLocation)`
returns `~/.local/share/LlamaMail` with the old name and `~/.local/share/mail`
with the new one.

**Pairing credentials were unaffected.** `SecureStoreKeychain` uses a fixed
Secret Service name (`com.urlxl.mail`) that never depended on
`applicationName`, so devices stayed paired across the rename. That name held
until the app-id rename below, which moved it and had to carry the secrets
across explicitly.

## The original migration could not work

`7941e92` ("Migrate llamamail.db to kypost.db on first launch after the rename")
handled only the *filename*, looking for `llamamail.db` inside the **new**
directory. A genuinely pre-rename profile has it in the **old** directory, so
the migration could never find it — the exact orphaning the commit was written
to prevent.

Fixed 2026-07-25 in `app/main.cpp`: both candidate locations are now checked,
and the file is **copied**, not renamed, so a bad migration is always
recoverable by hand.

## The limit of the fix

The migration only runs when **no `kypost.db` exists yet**.

An install that already launched a post-rename build has an empty `kypost.db`,
and this deliberately will not overwrite it. Choosing between two populated
databases is a data-merge decision, not a rename fix, and silently picking one
could destroy mail. Those profiles keep their old data orphaned on disk and
re-sync from the relay instead.

Practically that costs: cached mail/contacts (re-synced automatically), and
`settings.ini` preferences — theme, keyword visibility, tray options — which
reset to defaults. Nothing is deleted; the old directories are left intact.

## Recovering an orphaned profile by hand

Only if you want the old cache back rather than re-syncing. Quit KyPost first.

```sh
# Inspect before deciding — the higher row count is not always the one you want.
sqlite3 "file:$HOME/.local/share/LlamaMail/llamamail.db?mode=ro" \
  'select (select count(*) from emails), (select count(*) from contacts);'
sqlite3 "file:$HOME/.local/share/mail/kypost.db?mode=ro" \
  'select (select count(*) from emails), (select count(*) from contacts);'

# Back up the current one, then promote the old one.
cp ~/.local/share/mail/kypost.db ~/.local/share/mail/kypost.db.bak
cp ~/.local/share/LlamaMail/llamamail.db ~/.local/share/mail/kypost.db

# Old settings/cursors, if wanted:
cp ~/.config/LlamaMail/settings.ini ~/.config/mail/settings.ini
cp ~/.config/LlamaMail/cursors.ini  ~/.config/mail/cursors.ini
```

Cursors and the database must be moved **together** — a database from one era
with a sync cursor from another will skip messages.

Once you are satisfied, the legacy directories can be removed:

```sh
rm -rf ~/.local/share/LlamaMail ~/.config/LlamaMail
```

## Stale build trees

Pre-rename build directories may still exist in a working copy
(`build-local/`, `build-qt6/`, `build-final-review/`, `build-local-install/`,
`build-flatpak/`), containing artifacts like `libllamacore.a` and a `llamamail`
binary. They are covered by `.gitignore`'s `/build*/` and affect nothing, but
they are dead weight and can be deleted.

# com.urlxl.mail → com.kysecurity.mail: the app-id rename

2026-08-23. Unlike the rename above, this one changed the application's
**identity**, not its display name, so it is three separate migrations wearing
one string.

## What the one string actually was

| Role | Moves with the rename? | Consequence |
|---|---|---|
| Flatpak app id | Yes — it *is* the rename | New app. See "What is orphaned" below |
| D-Bus well-known name | Yes, and it must | Derived by `KDBusService` from `organizationDomain` + `applicationName`, so changing `organizationDomain` to `kysecurity.com` moves it automatically. `--own-name` in the manifest and `packaging/dbus/` were updated to match, or activation would break |
| QML module URI | Yes | Internal only. `import com.kysecurity.mail 1.0` across `app/qml`, `tests/qml`, `main.cpp`, `FakeSingletons.h` |
| Secret Service name | Yes, **with a fallback** | Covered below. This is the one that could have destroyed data |

## What is orphaned, and why it could not be helped

A Flatpak app id determines the sandbox home. `~/.var/app/com.urlxl.mail/`
does not become `~/.var/app/com.kysecurity.mail/`, and the new sandbox cannot
see the old directory *from inside* — that is what the sandbox is for. So for
Flatpak installs the database, `settings.ini` and `cursors.ini` are orphaned by
this rename, with no in-app migration possible. Mail and contacts re-sync from
the relay; preferences reset to defaults. Nothing is deleted.

Recovering by hand, if wanted (quit KyPost first):

```sh
cp -r ~/.var/app/com.urlxl.mail/data/mail  ~/.var/app/com.kysecurity.mail/data/
cp -r ~/.var/app/com.urlxl.mail/config/mail ~/.var/app/com.kysecurity.mail/config/
```

The database and `cursors.ini` must be moved **together**, for the same reason
as the previous rename: a database from one era with a sync cursor from another
skips messages.

The published OSTree channel is also a clean break — `flatpak update` computes
deltas per app id, so existing installs never see the new one and must install
it fresh. `v0.1-alpha` had zero downloads and the channel 404s, so the affected
population is test hardware.

## The keychain, which is not orphaned

Secrets are the exception, and deliberately so. The manifest grants
`--talk-name=org.freedesktop.secrets`, i.e. the **host** Secret Service
directly rather than the per-app portal, so entries are keyed by the service
string alone and survive an app-id change untouched. Renaming the service
string without more would therefore have stranded a perfectly readable
`db.encryptionKey` and a full set of `pairing.*` credentials — the app would
have reported itself unpaired while the old secrets sat in the keyring.

`SecureStoreKeychain` now takes a read-only `legacyService`. On `Absent` under
`com.kysecurity.mail` it retries under `com.urlxl.mail` and, on a hit, copies
the value forward so the next read is a single call.

Three details are load-bearing:

- **It is key-agnostic.** The obvious alternative — a one-shot migration over a
  list of key names — is a copy of the 19 keys `PairingStore`, `AppLockStore`
  and `DatabaseKeyStore` each define privately. A key that drifted out of that
  copy would be a credential silently lost with nothing failing.
- **`remove()` clears both services.** `PairingStore::clear()` and the
  ten-failure wipe are implemented as `remove()` over their keys, and the
  fallback would otherwise resurrect a wiped credential on the next launch. A
  legacy removal that fails makes `remove()` report false rather than claim a
  wipe that did not fully happen.
- **A `Failed` legacy read fails closed, once.** The primary answering `Absent`
  proves the daemon is reachable, so a `Failed` legacy read is anomalous and is
  reported as `Failed`, never `Absent` — `AppLockStore` would read the latter as
  "no PIN configured". It then latches the fallback off for the session, because
  an unreachable service costs ~25 s per missing key (`SecureStoreKeychain.h`)
  and 19 of those at startup is an eight-minute launch. The migration retries on
  the next launch.

Copied, not moved, following the same rule as the previous rename: a bad
migration stays recoverable by hand. The old entries remain in the keyring
until a wipe or an explicit `remove()` clears them.

## What deliberately did NOT change

- **`mail.urlxl.com`** — the relay hostname is infrastructure, not branding.
- **`com/urlxl/mail/...`** in `core/models/MfaChallenge.h` and the mobile plan —
  Java package paths pointing into the kypost-android repo. They rename when
  that repo does.
- **The historical records** — `docs/superpowers/plans/`, `docs/superpowers/specs/`
  and the top half of this file still say `com.urlxl.mail`, because they are
  accounts of what was true when they were written.
