# archiveview — Archive Lister WLX Plugin

A WLX (Lister) plugin for [Double Commander](https://doublecmd.github.io/) that previews the
contents of an archive without entering it — format, member tree, sizes, permissions, link
targets — using [libarchive](https://www.libarchive.org/).

This is a **read-only viewer**, deliberately. Double Commander already browses archives through
its WCX plugins; the lister's distinct value is being the fast summary you get from `F3` without
navigating into the file. Adding, deleting, and renaming members is WCX territory.

Qt6 only. There is no GTK3 variant.

---

## Status

All planned milestones (**M0–M7**) are implemented, less the archive integrity test, which was
dropped from scope. 61 checks pass, clean under ASAN and UBSAN — including the built `.wlx`
itself, sanitizer-built and driven through its full C ABI.

## Features

- **Directory tree** built from member paths, so archives that store no explicit directory
  entries still get a hierarchy. `Ctrl+T` toggles between tree and flat views.
- **Off-thread scanning.** Entries stream into the view in batches as they are read; the file
  manager stays responsive throughout, and a 100k-entry archive shows its first rows in
  milliseconds rather than after the whole walk.
- **Real cancellation.** Closing the preview or switching files aborts the scan immediately,
  even mid-decompression of a solid stream.
- **Columns:** name, size, packed size, ratio, CRC-32, modified, mode, owner/group, link target.
  Packed size, ratio, and CRC come from the ZIP central directory and are blank for formats that
  do not record them — blank rather than fabricated.
- **Archive comment** for ZIP, read from the End of Central Directory record. No subprocess.
- **Encryption indicator** per entry (a lock in the leftmost column, distinguishing
  data-encrypted from metadata-encrypted), plus an archive-level marker in the status bar.
- **Find** via DC's own find-next (`F7` / `Ctrl+F` depending on configuration), which walks the
  whole tree and expands collapsed subtrees onto a hit.
- **Live filter** (`Ctrl+F`) — search-as-you-type over the whole tree, matching inside collapsed
  directories and showing the surviving branches.
- **Detail panel** for the selected member: path, type, link target, size, packed size, CRC,
  timestamp, owner, encryption. With nothing selected it describes the archive instead.
- **Extract selection to a directory**, **open a member with its default application**, and
  **drag members out** to any drop target as `text/uri-list`. All read-only: nothing writes back
  into the archive.
- **Copy selection** (`Ctrl+C`) as tab-separated rows, with full in-archive paths.
- **Configurable** through the ini DC supplies: starting view mode, detail panel and filter box
  visibility, hidden columns, entry ceiling, and a fallback codepage for member names.
- **Nothing is hidden.** Members with traversal (`../`) or absolute (`/`) paths are shown exactly
  as stored, and two members sharing one path get two rows rather than being folded into one.
  Both are things you want to see in an archive you did not create.

## Security

The plugin this one replaces
([`libarchive_qt_crap`](https://github.com/j2969719/doublecmd-plugins/tree/master/plugins/wlx/libarchive_qt_crap))
built a `/bin/sh -c` command line containing the previewed file's name in order to scrape an
archive comment out of `7z l` piped through `pcregrep`. Its escaping covered spaces and single
quotes; `$`, backticks, `;`, `|`, `&`, newlines and more were live. Previewing a file named
``x$(...).zip`` executed its name — with the trigger being nothing more than pressing `F3` on a
file you just extracted.

**archiveview spawns no processes at all.** There is no `QProcess`, no `system()`, no `/bin/sh`.
The archive comment comes from `ZipCentralDirectory`, which parses the End of Central Directory
record directly — about 300 lines that also yield the per-entry packed size and CRC libarchive
does not expose, and that behave identically on every machine, as against regex-scraping the
human-readable, locale-translated, version-dependent output of whatever `7z` is installed. This
is enforced at build time by `cmake/CheckNoSubprocess.cmake`, which fails the build if any
subprocess API appears in `src/` — the property is structural, so it should not depend on a
reviewer noticing a regression.

`tests/make_fixtures.sh` generates archives named after shell metacharacter payloads
(``x$(touch ...).zip``, `` y`touch ...`.zip ``, embedded newlines, `;`, `|`, `&`). Listing all of
them creates no marker files.

### Extraction is where hostile names become dangerous

The listing shows `../../../../etc/passwd` and `/absolute/path/file.txt` exactly as stored — that
is the most useful thing a viewer can tell you about an archive you did not create. Writing them
is another matter, so extraction refuses, in three independent layers:

1. **Member paths are validated** before extraction: any `..` component or leading `/` is
   refused and counted. Checked per path component, so `..hidden` and `a..b.txt` are unaffected.
2. **The resolved target must stay inside the destination**, compared after canonicalisation.
3. **libarchive's `ARCHIVE_EXTRACT_SECURE_SYMLINKS` and `SECURE_NODOTDOT`** are enabled, and
   stored symlink and hardlink targets are themselves checked — a symlink pointing out of the
   tree is refused before it can be created, which is what defeats the two-member attack where a
   symlink is planted and the next member is written through it.

Refusals are reported to the user, not swallowed: extracting fewer files than were selected
without saying why would be the wrong kind of quiet.

The process working directory is never changed. `bsdtar` `chdir()`s into the destination, which
is fine for a standalone tool and unacceptable inside a file manager's process, so absolute
target paths are built explicitly instead.

## Requirements

- Qt 6 (Core, Gui, Widgets)
- libarchive 3.x (`pkg-config libarchive`) — tested against 3.8.9
- `wlx/wlxbase_wlqt` from this repository (built automatically as a subdirectory)

No runtime dependency on `7z`, `pcregrep`, or any other external binary.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Produces `build/archiveview_qt6.wlx`. The build is `-Wall -Wextra` clean, compiles with
`-D_FILE_OFFSET_BITS=64` so archives over 2 GB are handled correctly on 32-bit builds, and links
with `-fvisibility=hidden` plus `-Wl,--exclude-libs,ALL` so only the seven `List*` entry points
are exported:

```bash
nm -D --defined-only build/archiveview_qt6.wlx | grep ' T '
```

## Testing

```bash
./tests/make_fixtures.sh /path/to/fixtures    # shell-generated corpus, ~540 MB
./tests/make_hostile.py  /path/to/fixtures    # crafted archives the shell tools cannot produce
cmake --build build -j$(nproc)
./tests/run_suite.sh /path/to/fixtures
```

Fixtures are reproducible and deliberately not committed. `run_suite.sh` reports 61 checks and
exits non-zero on any failure. Three harnesses sit under it:

- **`scan_smoke`** drives the scanner and model headless — batch count, time to first batch,
  cancel latency, probe latency, and a consistency check that every entry is reachable through
  the tree.
- **`wlx_host`** `dlopen()`s the built `.wlx` and drives its exported C ABI the way DC does:
  load, search forwards/backwards, every `lc_*` command, the destroy/recreate reload cycle,
  switching files on one parent, calls with null handles, and teardown by destroying the parent.
  This is the surface where a null dereference is a crash *in the file manager* — the
  predecessor dereferenced an unchecked `findChild<QTableWidget*>()` in three separate entry
  points.
- **`extract_smoke`** extracts to a scratch directory and verifies every written path is inside
  it, which is the containment property above stated as a test.

`ListPrint` is exercised with a printer name that does not exist, so the suite can check that an
unknown printer is *refused* rather than silently redirected to the default — no test run ever
queues a job on a real device.

Measured on the reference machine:

| Fixture | Result |
|---|---|
| `many.zip` (100,100 entries) | first rows at 185 ms, complete in 543 ms, 196 batches |
| `slow.tar.bz2` (2830 ms full walk) | cancel honoured 9–111 ms after the request |
| `bomb.zip` (2.5 GiB declared, 2.5 MB on disk) | listed in under 1 ms — bodies are skipped, never expanded |
| `structure.tar` (300 levels deep, 5000 siblings) | correct tree, duplicates preserved |
| `truncated.*`, `corrupt.zip` | partial listing retained, error reported, no crash |
| format probe, whole corpus | ≤ 8 ms, including the archive that takes 2830 ms to walk |
| `structure.zip`, `many.zip`, `bomb.zip` | packed sizes and CRCs match `unzip -v` exactly |
| `encrypted-{zipcrypto,aes}.zip`, `encrypted-entries.7z` | list without a passphrase, every entry flagged locked |
| `encrypted-header.7z` | clean refusal in under 1 ms, no freeze |
| `nasty-names.tar`, `symlink-escape.tar` | traversal, absolute, and symlink-escape members refused; nothing written outside the destination |
| `many.zip` extraction | 100,100 members in 2.4 s |
| wrong passphrase | stops at the 5-attempt cap instead of looping against the prompt |
| whole corpus + full ABI + extractor, ASAN + UBSAN | clean, including the mid-decompression cancel path |

## Design notes

**Cancellation.** libarchive has no cancel API, and a flag checked between entries is useless
while `archive_read_next_header()` is blocked decompressing a solid block. The archive is opened
through `archive_read_open2()` with our own read callback, which reports a fatal error when the
cancel flag is set — unwinding the decompressor from the inside. Latency is bounded by one
256 KiB read block.

**Seek callback.** Registering `archive_read_set_seek_callback()` is what selects libarchive's
seeking ZIP reader over its streaming one. Without it, symlinks in a ZIP come back as regular
files whose contents are the target path, because local file headers do not carry the type.

**Sizes.** `archive_entry_size()` returns `int64_t` and is legitimately unset for streamed ZIP
entries. It is never cast to an unsigned type here; unset means the column is blank rather than
`SIZE_MAX`.

**Whole-archive ratio** uses the file's own size, not `archive_filter_bytes(a, -1)`. The latter
counts bytes *consumed*, and the walk skips entry bodies — for a seekable ZIP it reads about half
the file, which would make the ratio nonsense.

**Member name encoding.** `archive_entry_pathname_utf8()` is preferred; when it returns NULL the
raw bytes are validated as UTF-8, then tried against the locale codec, then decoded as Latin-1 as
a lossless last resort. enca detection is deliberately not used per entry — it is unreliable on
short filenames and 100k detections would cost more than the entire walk. An archive whose names
are in a legacy codepage will therefore render as mojibake until a configurable codepage lands
via the `NameCodec` ini setting; the bytes are preserved either way. Setting
`NameCodec=windows-1251` turns `ïðèâåò.txt` back into `привет.txt` — the archive does not record
its encoding, so a user who knows where the file came from is the only reliable source.

**The `ListLoad` probe.** DC needs a yes/no answer before `ListLoad` returns so it can fall
through to another viewer; the walk cannot give one, since it only finds out at the end. So the
format is identified synchronously and only the walk is asynchronous. That probe deliberately
registers *no* seek callback — with one, the seeking ZIP reader parses the whole central
directory before yielding a header, measured at 122 ms for a 100k-entry archive, all of it on
DC's UI thread. Without it, every fixture probes in ≤ 8 ms.

**The ZIP central directory** is parsed on the scanner thread before the walk, and joined onto
entries by normalised path. Duplicate paths are matched positionally — the directory lists them
in the same order libarchive walks them. Everything in the parser treats the file as hostile:
every length is validated against the bytes actually read, a declared central directory larger
than the file is refused rather than allocated for, and the End of Central Directory record is
located by scanning backwards for a signature *whose declared comment length matches the bytes
remaining after it* — the four-byte signature alone occurs inside stored data often enough to
matter. When any of that fails the parse is abandoned and the extra columns stay blank, which is
also what happens for every non-ZIP format. Cost: about 60 ms added to a 100k-entry archive, all
of it off the UI thread.

**Encryption.** Listing an encrypted archive needs no passphrase at all: names, sizes, and CRCs
live in headers, and entry bodies are skipped rather than decrypted. So the viewer shows the
contents with lock indicators and does not prompt — prompting for something it does not need
would be worse than useless. The one format where listing genuinely requires decryption is 7z
with `-mhe=on`, and libarchive cannot read those headers at all, with or without a passphrase;
that is reported as an immediate error. The predecessor handled this case by shelling out to
`7z l`, which prompted on a stdin that was never connected and froze the file manager for the
full 30-second `QProcess` timeout.

The passphrase handshake itself (`archive_read_set_passphrase_callback` plus the prompt) is
therefore wired but **not reachable from any listing path** — its consumer is extraction in M7,
which does read entry data. It is deliberately *not* a `BlockingQueuedConnection`: that would
have the scanner thread block inside the signal while the GUI thread ran the dialog, and if the
GUI thread were simultaneously inside `cancelAndWait()` — which DC triggers merely by switching
files — the two would block on each other forever. Instead the scanner parks on a
`QWaitCondition` with a timed wait, `cancel()` wakes it, and `stopScan()` closes any open prompt
before joining. The GUI thread never blocks on the scanner.

**Packed size for encrypted entries** is the figure the central directory stores, which includes
the per-entry encryption overhead — a 12-byte ZipCrypto header, or an AES salt plus password
verifier. `unzip -v` subtracts that (reporting 16 where the record says 28). The stored figure is
the honest answer for a column labelled "Packed", and it is what makes the ratio reflect real
on-disk cost. Directories, whose stored size and CRC are always zero, show blank rather than
`0 bytes` and `00000000`.

**`ListLoadNext`** keeps the view, header layout, column widths, filter, and find panel, and
swaps only the model contents — so arrowing through a directory of archives no longer destroys
and recreates the whole widget tree per file. It validates the handle DC passes against the
instance actually registered for that parent, rather than trusting a `reinterpret_cast`.

**Widget teardown uses `deleteLater()`**, not `delete`. A passphrase prompt runs a nested event
loop, so `ListLoad` for a new file can be reached from inside the old widget's own slot;
deferring the deletion keeps that from freeing a widget whose stack frame is still live.

**The filter is a `QSortFilterProxyModel`** with recursive filtering, which means the view's
model is *not* `ArchiveModel`. Every index that reaches the model goes through `entryFor()` /
`pathFor()`, because passing a proxy index straight to the source model yields the wrong row
whenever a filter is active — a silent, plausible-looking wrong answer.

**Drag-out is eager**: files are extracted when the drag begins, not when it is dropped. The lazy
form is XDS (`XdndDirectSave0`), an X11 XDND protocol extension with no Wayland equivalent — and
this repository's Qt base targets Wayland, so it would mean carrying a path that does not run on
the primary target. Nothing is written merely by selecting rows.

**No `ListGetPreviewBitmap`.** Its contract returns an `HBITMAP`, which on Windows is a GDI
handle DC can use directly. On Linux DC is an LCL application and that handle would have to be an
LCL widgetset bitmap object, which a Qt plugin has no way to manufacture; no plugin in this
repository implements it. Leaving the symbol unexported makes DC fall back to its own thumbnailer,
which is the correct outcome — a stub returning nullptr would only add a failed call per
thumbnail.

**Plugin-to-DC `itm_*` messaging is receive-only.** `ListNotificationReceived` handles what DC
sends us (`itm_percent`, `itm_fit`). The reverse direction is `SendMessage(WM_COMMAND, ...)` in
the Total Commander API and has no transport a Qt plugin can use inside an LCL host, so the view
does not push its scroll position back to DC. `lc_setpercent` in the other direction works.

**Icons** are resolved once per file extension and cached. A per-row `QMimeDatabase` lookup plus
theme icon resolution was one of the two reasons the item-widget predecessor could not scale.

## Roadmap

| Milestone | Contents |
|---|---|
| M0–M1 ✅ | Hardened build, entry points, detect string, threaded scanner, tree/flat model |
| M2 ✅ | Hostile-fixture corpus, synchronous format probe, ABI harness, sanitizer sweep |
| M3 ✅ | ZIP central directory: archive comment, per-entry packed size, CRC-32, ZIP64 |
| M4 ✅ | Encryption indicators; passphrase handshake wired (exercised by M7 extraction) |
| M5 ✅ | `ListLoadNext(W)`, `ListSetDefaultParams`, `lc_setpercent`, `ListSearchDialog`, `ListPrint`, `ListNotificationReceived` |
| M6 ✅ | Live filter, detail panel, ini configuration (view mode, columns, codepage, ceiling) |
| M7 ✅ | Extract selection, open a member, drag out as `text/uri-list` (no integrity test, by request) |

Dark mode is handled by `wlxbase_wlqt`'s `ThemeManager`. The `lcp_darkmode` /
`lcp_darkmodenative` flags are not implemented because those constants do not exist in this
repository's `sdk/wlxplugin.h`; adding them is a shared-SDK change, tracked separately.

## Configuration

Settings live under `[archiveview]` in the ini file Double Commander passes to
`ListSetDefaultParams`. There is no settings dialog; this follows the repository's convention for
these plugins of a hand-edited section.

```ini
[archiveview]
; Open in a flat list instead of a directory tree
FlatView=false
; Per-entry detail panel on the right
DetailPanel=true
; Search-as-you-type box above the listing
FilterBox=true
; Give up after this many members (a hostile archive can declare far more
; than there is memory to model)
MaxEntries=500000
; Codec for member names that are neither valid UTF-8 nor decodable in the
; current locale, e.g. windows-1251 or Shift-JIS. Unset means a lossless
; Latin-1 fallback, which keeps the bytes but shows the wrong glyphs.
NameCodec=
; Columns hidden on open, by header name
HiddenColumns=CRC-32,Owner
```

## Installation

Copy `archiveview_qt6.wlx` somewhere permanent, then in Double Commander:
**Configuration → Plugins → Lister plugins (WLX) → Add**.

## License

Same terms as the surrounding repository.
