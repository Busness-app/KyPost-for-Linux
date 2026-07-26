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
`applicationName`, so devices stayed paired across the rename.

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
