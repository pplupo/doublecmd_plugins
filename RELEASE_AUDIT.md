# Release v0.01 zip audit (2026-08-26)

Every zip attached to [v0.01](https://github.com/pplupo/doublecmd_plugins/releases/tag/v0.01)
was downloaded and listed with `unzip -l` (no extraction) to check whether it actually
contains its plugin binary. Root causes below were found in the build run's log
(`gh run view 32974297597 --repo pplupo/doublecmd_plugins --log`).

## Zips missing their plugin binary

| Zip | Contains | Root cause |
|---|---|---|
| `exif` | only `README.txt`, `license.txt`, `exif.lng` — no `exif.wdx` | Compile error: `exif.c:292: error: conflicting types for 'strlcpy'`. Ubuntu 24.04's glibc now declares `strlcpy` itself, which conflicts with the plugin's own declaration/shim. Needs a source-level fix (guard the plugin's own declaration behind a glibc-version/`__GLIBC__` check, or feature-test for `HAVE_STRLCPY`). |
| `rclone` | only `rclone.ico`, `COPYING.txt` — no `rclone.wfx` | Lazarus compile error: `Fatal: (2013) Cannot open include file "calling.inc"`. A required FPC/LCL include path isn't being found on the `ubuntu-24.04` runner's Lazarus install. Needs investigation into which unit/package search path is missing (likely an `fpc.cfg`/`lazarus` package path difference between jammy and noble). |
| `officeview` | only `README.md` — no `officeview_qt6.wlx`/`officeview_gtk3.wlx` | `CMake Error: The current CMakeCache.txt directory ... is different than the directory ... where CMakeCache.txt was created.` **Cause: `wlx/officeview/build/CMakeCache.txt` is committed to git**, baked with an absolute path from a local dev machine (`/home/pplupo/repos/plugins/...`). When CI checks out the repo, that stale cache collides with the runner's own checkout path and CMake refuses to proceed. Confirmed via `git ls-files` that `wlx/officeview/build/` is the only plugin's build dir with `CMakeCache.txt` actually tracked (other plugins also have stray tracked files under `build/`, but not their `CMakeCache.txt`, so they don't hit this). **Fix: `git rm -r --cached` the tracked `build/` directories and add `build/` to `.gitignore`** — not done yet, flagging for a separate change since it touches ~760 tracked files across `wlx/csvview`, `wlx/kate`, `wlx/kpartview`, `wlx/officeview`, `wlx/structview`, `wlx/wayland_qt_base`, and `wfx/rclone`. |
| `logview` | only `README.md`, `logviewer.png`, `color_editor.png`, a stray `double commander wlx log viewer.md` — no `logviewer.wlx` | Compile error: `LogViewerWidget.cpp:398: error: 'beginFilterChange' was not declared in this scope` (and 3 more of the same). `QSortFilterProxyModel::beginFilterChange()`/`endFilterChange()` are newer Qt API (added in Qt 6.7) that don't exist in Qt 6.4.2, which is what `ubuntu-24.04`'s `qt6-base-dev` ships. Needs a source-level fix: either guard those calls behind `#if QT_VERSION >= QT_VERSION_CHECK(6,7,0)` or stop relying on them. |
| `mdk` | only `proposal.md`, `README.md` — no `mdk_qt6.wlx` | `fatal error: mdk/c/Player.h: No such file or directory`. The MDK SDK (`https://github.com/wang-bin/mdk-sdk`) is a third-party binary SDK the plugin links against; it isn't an apt package and CI never fetches/installs it. Needs either vendoring it via CMake `FetchContent`/`ExternalProject_Add` (like `officeview` does for `mupdf`), or documenting it as a runner-only limitation and skipping the plugin in CI the same tolerant way `kpartview`/`kate` skip on missing KDE Frameworks 6. |

## Confirmed correct (zip contains its plugin binary)

`crx_wdx`, `csvview` (both `_gtk3`/`_qt6`), `dbview` (both), `diagramview` (both), `diskdir`,
`fileinfo`, `gstplayer`, `gvfs`, `kate`, `kpartview`, `markdownview` (both),
`mpv_wayland` (both), `ooinfo`, `ooxml`, `similarity`, `sourceview`, `structview` (both),
`xpi_wdx`.

## Correctly binary-less (not a bug)

`mediainfo` and `translitwdx` are Lua scripts run by a Lua-hosting plugin, not
compiled `.wdx` binaries — their zips containing only `.lua`/text files is expected.
