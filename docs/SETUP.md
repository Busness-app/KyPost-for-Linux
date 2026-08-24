# Local tooling gaps

Re-verified during Task 8 (Qt5 drop / Qt6-only build). All 5 of the original
items from the Task 1 checklist are now installed on this machine. The one
real gap tracked below — the `org.kde.Sdk` Flatpak runtime — is not one of
those original 5; it's a newly-identified item surfaced by this
re-verification pass.

- **`extra-cmake-modules`** — installed (`extra-cmake-modules 6.27.0-1`,
  verify: `pacman -Q extra-cmake-modules`). The top-level `CMakeLists.txt`
  still does `find_package(ECM QUIET NO_MODULE)` and only extends
  `CMAKE_MODULE_PATH` if found — this stays optional until a task starts
  using ECM's convenience macros (e.g. `ecm_add_test`, `KDECompilerSettings`).

- **`flatpak-builder`** — installed (`/usr/bin/flatpak-builder`, verify:
  `which flatpak-builder`). Unblocks the Flatpak packaging skeleton
  (`packaging/flatpak/`), but see the `org.kde.Sdk` gap below — the runtime
  needed to actually build the Flatpak is still missing.

- **`clickable`** — installed (`/usr/bin/clickable`, verify: `which
  clickable`). Unblocks the Clickable/Ubuntu Touch packaging tooling itself,
  though Ubuntu Touch as a build *target* is deferred per `AGENTS.md`
  Section 4 (Qt5 EOL; UT hasn't shipped its own Qt6 track yet) — clickable
  has nothing Qt6-buildable to package against right now.

- **A QtWebEngine package (Qt6 variant)** — installed as `qt6-webengine
  6.11.1-4` (verify with `pacman -Q qt6-webengine` or `pacman -Qs
  webengine` — note the old checklist's `pacman -Qs qtwebengine` command
  returns nothing because the actual package name is `qt6-webengine`, not
  `qtwebengine`; same package, the old verify command just didn't match the
  hyphenated name). The `io.qt.qtwebengine.BaseApp` Flatpak extension is a
  separate, still-unverified concern for the Flatpak build specifically —
  not needed for this pass. HTML mail rendering (the feature this blocks)
  is still a future phase.

- **`kunifiedpush`** — installed (`kunifiedpush 26.04.3-1.1`, verify:
  `pacman -Qs kunifiedpush`). Unblocks the KUnifiedPush push proofs in a
  future phase (still needs the user's real distributor/account to actually
  exercise it).

## `org.kde.Sdk` Flatpak runtime — resolved in Task 9

Was the one real gap surfaced by the Task 8 re-verification pass: `org.kde.Platform//6.10` was installed but not the matching `org.kde.Sdk`, so `flatpak-builder` had nothing to build against. Task 9 installed it (`flatpak install --user -y flathub org.kde.Sdk//6.10`, verify: `flatpak list --user --runtime | grep -i kde`) and used it to build `packaging/flatpak/com.kysecurity.mail.yaml` end to end.

One additional gap surfaced during that build, not predicted here: `Qt6Keychain` (installed natively via the `qtkeychain-qt6` pacman package) has no Flatpak/KDE-runtime equivalent, so the manifest builds it from source as a `qtkeychain` module pinned to release `0.17.0` (matches the native package's upstream version) ahead of the `kypost` module. The manifest also needed `app/CMakeLists.txt` to gain an `install(TARGETS kypost ...)` rule — the native build never needed one since it only ever ran the binary in-tree, but `flatpak-builder`'s `cmake-ninja` buildsystem runs `ninja install` unconditionally.
