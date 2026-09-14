# Vendored supramark renderers (Mermaid / PlantUML -> SVG)

`libmarkdownview_diagrams.a` (24 MB, the only file here that matters at
build time) wraps [supramark](https://github.com/Actrium/supramark)'s
`mermaid-little` and `plantuml-little` crates. They render Mermaid and
PlantUML source to SVG **in-process**, which is what lets the offline
edition stop sending diagram sources to mermaid.ink and plantuml.com.

Linked in by CMakeLists.txt's `ENABLE_LOCAL_DIAGRAMS` option (default OFF).
Unlike `vl-convert`, this is not embedded-and-self-seeded: it is a C ABI
library, not an executable, so there is no binary to write out and exec —
it links straight into the `.wlx`.

Cost, measured stripped and linked: **mermaid-little 4.18 MB,
plantuml-little 9.91 MB**. For comparison the vl-convert binary next door
is ~84 MB, which is why figures still shell out to it while these do not.

## Why a wrapper crate, given supramark ships its own

supramark ships a ready-made C ABI `staticlib` per renderer
(`crates/<name>/packages/native`), and the first attempt just linked both.
**That does not work.** Each is its own `staticlib`, so each bundles its own
copy of Rust's `std` and of `supramark-font-metrics`, and linking both into
one `.wlx` fails outright:

```
multiple definition of `supramark_install_metrics_callback'
multiple definition of `rust_eh_personality'
multiple definition of `std::panicking::EMPTY_PANIC'
```

`markdownview-diagrams/` is the fix: one small crate depending on both
renderers, producing one `staticlib`, so `std` is linked once. Its C ABI is
the same shape as upstream's two headers, minus the duplication, under
`markdownview_diagrams_*` names so it is unambiguous at the link line which
archive was used.

It also sets **`panic = "unwind"`**, deliberately overriding what
supramark's own native packages use. They build with `panic = "abort"`,
which is right for a React Native host and wrong here: this is linked into
a file-manager plugin, and aborting takes Double Commander down with it.
With unwinding, `catch_unwind` turns a panic on a malformed diagram into an
error code and the block falls back to plain text like any other failure.

## How to reproduce

```sh
# from this directory
git clone --depth 1 https://github.com/Actrium/supramark.git supramark-src
cd markdownview-diagrams
# rust-toolchain.toml in supramark pins 1.97.0; RUSTUP_TOOLCHAIN=stable
# builds fine on a locally installed stable and avoids rustup fetching a
# second toolchain.
RUSTUP_TOOLCHAIN=stable GRAPHVIZ_ANYWHERE_ALLOW_DOWNLOAD=1 cargo build --release
cp target/release/libmarkdownview_diagrams.a ..
strip --strip-debug ../libmarkdownview_diagrams.a
```

`supramark-src/` and `markdownview-diagrams/target/` are gitignored — only
the built archive is kept. The wrapper crate's `Cargo.toml` path
dependencies expect the checkout at `supramark-src` exactly.

`GRAPHVIZ_ANYWHERE_ALLOW_DOWNLOAD=1` is needed because `plantuml-little`
depends, non-optionally, on `graphviz-anywhere` for layout, whose build
script wants a prebuilt `libgraphviz_api.a`. That is a **custom C wrapper
API around Graphviz, not stock Graphviz** — a system `libgvc.so` does not
satisfy it. The flag fetches `graphviz-native-linux-x86_64.tar.gz` from
`https://github.com/Actrium/supramark/releases/download`. The alternative
is building it from source: `git submodule update --init` (Graphviz comes
from `https://gitlab.com/graphviz/graphviz.git`) then
`crates/graphviz-anywhere/scripts/build-linux.sh`, which builds Graphviz
14.1.5 plus expat via CMake.

Those Graphviz objects end up *inside* the archive, which is why
CMakeLists.txt names `z` and `expat` alongside it — `crc32` in
`gvdevice.c` and `XML_GetErrorCode` in `htmllex.c` are otherwise undefined
at link time. Both are already in the process via librsvg/GTK, so this
adds no new runtime dependency; the linker just needs them named.

See `PROVENANCE.txt` for the exact upstream commit these were built from.

## The host must install a text-measurement callback

Both crates are built with supramark's `metrics-ffi-callback` feature, so
they do **not** measure text themselves. Every layout decision — node
widths, lifeline spacing, label boxes — comes from whatever
`supramark_install_metrics_callback` was given, and it must be installed
before the first render. `src/core/diagram_render_local.cpp` does this with
Cairo's toy text API, via
`markdownview_diagrams_install_metrics_callback`.

Cairo deliberately, rather than the sibling `metrics-ttf-parser` feature
that would embed a ~130 KB DejaVu subset: librsvg draws the returned SVG
with the local fontconfig/FreeType stack, so measuring through that same
stack is what keeps the laid-out geometry and the drawn glyphs agreeing.
Measuring against a font that is not the one drawn is how labels end up
overflowing the boxes sized for them.

## Fidelity caveat

Upstream reports `plantuml-little` at byte-exact SVG parity with Java
PlantUML v1.2026.2 across 337 reference tests (29 diagram types, omitting
only DITAA and JCCKIT). `mermaid-little` is ~90.4% byte-exact overall
(1200/1328), but **sequence diagrams are 51/150** and mindmaps 7/25.
Explicit non-goals: KaTeX formulas, hand-drawn styling, ELK layout,
architecture diagrams.

Byte-exactness is a stricter bar than "renders correctly", and all eight
Mermaid types tried here — including three sequence diagrams — rendered and
rasterized at sensible sizes. Judging real fidelity still means rendering
actual documents both ways and comparing.

## Licences

- `mermaid-little`: MIT.
- `plantuml-little`: `GPL-3.0-or-later OR LGPL-3.0-or-later OR Apache-2.0
  OR EPL-2.0 OR MIT` — a disjunction, so Apache-2.0 or MIT is selectable.
- The Graphviz code linked inside the PlantUML archive is **EPL-1.0**.

That last one is worth checking against how this repo distributes builds
before shipping `ENABLE_LOCAL_DIAGRAMS=ON` binaries.
