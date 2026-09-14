# markdownview — handover: online / offline editions

Working notes for the next work session. Current branch: `markdownview-vegalite`.

## Where things stand

Diagram rendering is **one service per notation, every endpoint
configurable** — and **the offline edition now exists and works**. Three
editions build from one source tree, from two CMake options:

| Edition | `ENABLE_VLCONVERT` | `ENABLE_LOCAL_DIAGRAMS` | Size (Qt6) | Network |
|---|---|---|---|---|
| `markdownview` | `OFF` | `OFF` | 10 MB | all three notations |
| `markdownview-vlcharts` | `ON` | `OFF` | 94 MB | Mermaid + PlantUML |
| `markdownview-offline` | `ON` | `ON` | 103 MB | **none** |

| Notation | Service | Ini key | Default |
|---|---|---|---|
| Mermaid | mermaid.ink | `mermaid_url` | `https://mermaid.ink` |
| PlantUML | plantuml.com | `plantuml_url` | `http://www.plantuml.com/plantuml` |
| Vega-Lite | Kroki (light edition only) | `kroki_url` | `https://kroki.io` |

`build.sh` builds all three, each into its own release directory so the zip
loop emits one download apiece. The offline variant is **skipped with a
message** when `3rdparty/supramark/libmarkdownview_diagrams.a` is absent,
so a checkout without the vendored archive still produces the other two.

### The all-Kroki experiment, and why it was reverted

Routing all three notations through one Kroki endpoint was built, measured,
and rolled back. Be precise about which half of the result holds — the two
notations failed for completely different reasons:

- **Mermaid: unusable, not merely slow.** Every request to `kroki.io` came
  back HTTP 500 after ~30 s. Confirmed server-side, outside the plugin
  entirely: raw `curl`, both the POST and the GET/deflate forms, and
  `graph TD\nA-->B` — the simplest diagram that exists. `/health` reported
  `status: pass` with `mermaid: 11.16.0` registered the whole time, so its
  mermaid companion container is simply down on the public instance.
  mermaid.ink answers the same diagram in ~2.4 s.
- **PlantUML: no latency argument at all.** Measured warm, Kroki was
  *slightly faster* — ~0.18 s vs plantuml.com's ~0.22 s. Reverting this one
  is a consistency choice, not a performance one. Worth knowing before
  anyone "re-optimises" it back.

What survived the revert is the part that mattered: **all three base URLs
are settable**, so a self-hosted mermaid.ink / PlantUML server / Kroki
container each drop in the same way. Kroki does speak all three notations,
so one container can still back all three keys if that is preferred.

### What is in the tree now

- `src/core/vegalite_spec.{h,cpp}` — **new, always compiled.** The pure JSON
  transform (provenance-key stripping, theme defaults, title/subtitle
  wrapping, legend placement) plus `captionOf()`, lifted out of
  `chart_render_vegalite.cpp`. It had to move: that file isn't compiled at
  all in the `OFF` build, and the Kroki path needs the identical treatment
  applied before POSTing so both editions lay a figure out the same way.
- `diagram_render.cpp` — `krokiRenderSvg()` POSTs Vega-Lite specs; Mermaid
  and PlantUML use the restored `httpGet` + `base64UrlEncode` / `toHex`
  paths against their own bases. The POST body goes through a 0600 temp
  file, not curl's argv, which any process can read via `/proc`.
- `VegaLite::isCompiledIn()` — new, alongside `isAvailable()`. The figure
  path keys the local-vs-Kroki choice on **isCompiledIn**, so a vlcharts
  build whose vl-convert self-seeding fails renders nothing rather than
  quietly falling back to the network. That distinction is the point of the
  function; don't "simplify" it to `isAvailable()`.
- Settings keys are **unchanged from before this work** — `chart_renderer`,
  `enable_mermaid`, `enable_plantuml`, `enable_latex`, all defaulting on —
  plus the three new `*_url` keys. An interim revision renamed them and
  flipped the defaults to off; that was reverted, so existing ini files keep
  working untouched.
- The "Chart Renderer" submenu (Matplot++ / Cairo Only / Disabled) is gone,
  replaced by a plain **Render Vega-Lite Figures** checkbox. Those backends
  no longer exist, so it was a menu of one. `chart_renderer` stays a string
  under its legacy name: the menu writes `on`/`off`, and any legacy
  `cairo`/`auto` still reads as "render".

## Verified (2026-09-08)

`verify_diagram_services.cpp`, next to `CMakeLists.txt` — **untracked**,
commit it or don't. Links only `libmarkdownview_core.a`: no toolkit, and no
`QGuiApplication`, since it never touches the LaTeX path.

```
mermaid.ink           PASS   2441 ms, 22896 bytes   (72 ms warm)
plantuml.com          PASS    273 ms,  2629 bytes
kroki (vegalite)      PASS    743 ms, 13744 bytes
```

Also passing: spec normalisation strips `_caption`/`_src` before send, wraps
the title, places legends, rejects malformed JSON; every SVG rasterizes
through the real librsvg path; and the failure contract holds for a
malformed spec and for all three hosts unreachable — each returns empty, so
the block stays plain text. All three editions build clean, Qt6 and GTK3.

## The offline edition (was "Phase 2")

**The old note here was wrong twice over, in opposite directions.**

It said "there is no render CLI — a small Rust wrapper binary is needed".
Wrong: supramark ships ready-made C-ABI `staticlib` wrappers per renderer
(`crates/<name>/packages/native`), headers included.

But a wrapper crate *was* needed anyway, for a different reason. Linking
both of supramark's archives into one `.wlx` fails — each is its own
`staticlib`, so each carries its own Rust `std` and `supramark-font-metrics`:

```
multiple definition of `supramark_install_metrics_callback'
multiple definition of `rust_eh_personality'
multiple definition of `std::panicking::EMPTY_PANIC'
```

`3rdparty/supramark/markdownview-diagrams/` is the answer: one crate
depending on both renderers, producing one `staticlib`, so `std` links
once. **24 MB archive; 4.18 MB (mermaid) + 9.91 MB (plantuml) once linked
and stripped.** Against vl-convert's 84 MB, going fully offline is cheap.

Full build instructions, licence notes and the Graphviz story are in
`3rdparty/supramark/README.md`. The archive is vendored; the C++ build
never needs a Rust toolchain.

### Three things not to undo

- **`panic = "unwind"` in the wrapper crate's `Cargo.toml`.** supramark's
  own native packages use `panic = "abort"` — correct for a React Native
  host, fatal here, because an abort takes Double Commander down with it.
  `catch_unwind` in the wrapper turns a panic on a malformed diagram into
  an error code instead. This is the single most important line in that
  file.
- **The host must install a text-measurement callback before the first
  render.** Both crates use `metrics-ffi-callback`, so they do not measure
  text at all: node widths, lifeline spacing and label boxes all come from
  what the callback returns. `diagram_render_local.cpp` supplies it via
  Cairo's toy text API, deliberately rather than the `metrics-ttf-parser`
  feature — librsvg draws with the local fontconfig/FreeType stack, so
  measuring through that same stack is what keeps geometry and glyphs
  agreeing.
- **`LocalDiagram::isCompiledIn()` gates the choice, not render success.**
  Same rule as `VegaLite::isCompiledIn()`: an edition that ships local
  renderers must never fall back to the network, so a failed local render
  degrades to plain text.

### Verified (2026-09-08)

`verify_local_diagrams.cpp`, next to `CMakeLists.txt` — **untracked**. It
points every service base URL at an unresolvable host *first*, so anything
that still renders provably never left the machine:

```
mermaid   flowchart 247x336  sequence 450x283  state 142x298  dark 204x66
plantuml  sequence  111x163  class    164x85   activity 182x261  dark 111x131
mermaid web / plantuml web -> empty, as they must be
ALL PASS
```

Confirmed again at syscall level with `strace -f -e trace=execve,connect,socket`:
across all eight local renders the process spawns **no curl and opens no
socket**. Exactly two `curl` processes execute in the whole run, and they
are the harness's own deliberate "these must fail" web calls at the end.
(Beware the raw counts: `posix_spawnp` probes every `PATH` entry, so a
single curl invocation shows up as ~12 `execve` lines, all but one
`ENOENT`. Count the `-> 0` results, not the `execve` lines.)

**One real leak, and it is not ours to close.** Vega-Lite specs can name
their own data source, and vl-convert honours it: a spec with
`"data": {"url": "https://…"}` makes the offline build connect out
(traced to 185.199.110.153:443 loading a vega-datasets file, chart rendered
fine). That is Vega's data model, not a fallback in this plugin. Specs with
inline `data.values` -- what these report figures use -- are fully local.
Documented in README.md; worth remembering before anyone claims the offline
edition cannot make a network request at all.

Dark mode included on purpose: the `%%{init}%%` theme block and the
skinparams were split out of the web renderers into shared helpers
(`mermaidWithTheme` / `plantUmlWithTheme`) so the local path is styled
identically rather than rendering unthemed defaults.

### Still open: fidelity

Upstream reports `plantuml-little` byte-exact against Java PlantUML across
337 tests, but `mermaid-little` at **51/150 on sequence diagrams** (~90.4%
overall, mindmaps 7/25). Byte-exactness is a much stricter bar than
"renders correctly", and every case tried here rendered at a sensible size
— but **nobody has yet compared real documents side by side against
mermaid.ink**. That comparison is the thing to do before shipping the
offline edition as a default rather than an option.

### supramark still cannot shrink the vl-convert half

- **It does not render Vega-Lite.** The `vison` crate is *not* Vega-Lite —
  it's its own spec format (upstream `kookyleo/vison`), a card-rendering
  system for Markdown. The project overview listing "Vega-Lite" among its
  diagram types is misleading; vison's own docs never mention it.
  **vl-convert stays.**
- **It has no LaTeX/math renderer.** `supramark-markdown` only *parses*
  math, and `mermaid-little` lists KaTeX rendering as an explicit non-goal.
  MicroTeX stays — and isn't the size problem anyway.

## Also outstanding

- **The 15 s timeout is a UI-freeze budget.** These renders run
  synchronously on DC's UI thread, so a document with N unrenderable
  diagram blocks blocks for up to N × 15 s. Not a regression — this is the
  long-standing `httpGet` timeout, and the Kroki POST was given the same
  number rather than the 20 s it was first written with. Rendering
  off-thread is the real fix and has never been attempted. Every offline
  renderer landed in Phase 2 removes blocks from this budget.
- **Kroki lays text out with its own font metrics while librsvg rasterises
  with local fonts.** Where they disagree, labels can shift or collide.
  `renderVegaLiteWeb` pins `config.font` to `sans-serif` to reduce it, which
  also means the two editions can differ slightly on the same figure.
- **`build.sh` variant build dirs are keyed on the variant name**, not on
  the option values -- `build-markdownview`, `build-markdownview-vlcharts`,
  `build-markdownview-offline`. It has to be that way: vlcharts and offline
  both set `ENABLE_VLCONVERT=ON`, so the old `build-${vlconvert}` scheme
  would have made them share and clobber one directory.
- **Don't run two `build.sh` instances at once.** The first attempt here hit
  a 10-minute tool timeout; the script survived it and kept running, then a
  second run's `rm -rf release` raced it and both trashed each other's
  output. Symptom was a vanished `release/` and a log ending mid-zip.
  `pgrep -f "bash build.sh"` before starting.
- **Several unrelated plugins fail in `build.sh`** (gvfs won't compile
  against the current VFSNew signature; logview/mpv_wayland/dbview fail
  CMake's compiler test). Pre-existing, nothing to do with markdownview,
  but they make the log noisy -- grep for the markdownview section rather
  than scanning for the word "error".
- **Licence check before shipping offline binaries.** `mermaid-little` is
  MIT and `plantuml-little` offers Apache-2.0/MIT in its disjunction, but
  the Graphviz code linked inside the archive is **EPL-1.0**. Worth
  confirming that against how this repo distributes builds.
- **The vendored archive is x86-64 Linux only**, like `vl-convert` beside
  it. Any other target needs its own build of the wrapper crate.

## Useful commands

```sh
# build one edition
cmake -DENABLE_VLCONVERT=OFF .. && make -j$(nproc) markdownview_qt6

# the service harness; optional args override the three bases in order
g++ -std=c++17 -O1 -o verify_diagram_services verify_diagram_services.cpp \
  -Isrc/core $(pkg-config --cflags cairo librsvg-2.0) \
  build_online/libmarkdownview_core.a $(pkg-config --libs cairo librsvg-2.0)
./verify_diagram_services https://mermaid.ink http://localhost:8080 http://localhost:8000

# the offline harness -- proves rendering is local by making the network
# unreachable first. Needs a core built with -DENABLE_LOCAL_DIAGRAMS=ON.
g++ -std=c++17 -O1 -o verify_local_diagrams verify_local_diagrams.cpp \
  -Isrc/core $(pkg-config --cflags cairo librsvg-2.0) \
  build_offline/libmarkdownview_core.a 3rdparty/supramark/libmarkdownview_diagrams.a \
  $(pkg-config --libs cairo librsvg-2.0) -lz -lexpat -lstdc++ -lm -lpthread -ldl

# render a document through the engine outside DC.
# NOTE: that harness needs a QGuiApplication -- renderLatexToPng() bails at
# `if (!qApp)`, which silently disables ALL LaTeX and looks like a font bug.
# See the vfr4.cpp pattern from the 2026-09-08 session.
```
