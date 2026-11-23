# Runtime requirements

Each plugin is a shared library that Double Commander `dlopen()`s into its own
process. Plugins link the **host toolkit stack dynamically** and rely on the
system loader to supply it — there is no bundled copy and no `RPATH`/`RUNPATH`.
This is deliberate: Double Commander is itself a Qt or GTK process, and loading
a second, private copy of Qt, GTK, glib or libstdc++ into that process corrupts
shared runtime state (a documented libstdc++ facet-registry crash is exactly why
the toolkit libraries must stay system-shared). Only niche, self-contained
renderers (md4c, MicroTeX, tinyxml2, minizip, a statically-built MuPDF, …) are
vendored/static — those are not dynamic dependencies and never collide with the
host.

So the runtime requirement for every plugin is: **install the system packages
listed below for the variant you use.** Package names are given for
Debian/Ubuntu (what the release binaries are built against — Ubuntu 24.04, Qt
6.4.2) with the Arch/CachyOS equivalent in parentheses where it differs.

The release ships plain zips, not distro packages, so there is no package
manifest to carry these as declared dependencies; this document is the
dependency list.

## Common to all Qt6 (`*_qt6.wlx`) variants
- Qt 6 Core/Gui/Widgets — `libqt6core6 libqt6gui6 libqt6widgets6` (Arch: `qt6-base`)

## Common to all GTK3 (`*_gtk3.wlx`) variants
- GTK 3 — `libgtk-3-0` (Arch: `gtk3`), which pulls in glib2, cairo, pango, gdk-pixbuf

## Per-plugin extras

| Plugin | Variant | Additional runtime libraries (system packages) |
|---|---|---|
| **csvview** | qt6 | Qt6 PrintSupport (`libqt6printsupport6`) |
| | gtk3 | — |
| **dbview** | qt6 | Qt6 Sql (`libqt6sql6`). DB engines (SQLite, LMDB, DuckDB, Berkeley DB) are vendored/static. |
| | gtk3 | — |
| **structview** | qt6 | Qt6 Xml + PrintSupport. toml++/nlohmann-json/tinyxml2 are vendored. |
| | gtk3 | — |
| **certview** | qt6/gtk3 | none beyond the toolkit; mbed-TLS is vendored/static |
| **sourceview** | gtk3 | GtkSourceView 4 (`libgtksourceview-4-0`), libxml2 (`libxml2`) |
| **htmlview** | qt6 | Qt6 WebEngine (`libqt6webenginewidgets6`, Arch: `qt6-webengine`) + PrintSupport |
| | gtk3 | WebKitGTK 4.1 (`libwebkit2gtk-4.1-0`, Arch: `webkit2gtk-4.1`) |
| **kpartview** | qt6 | KDE Frameworks 6: Parts, KIO, CoreAddons, XmlGui (`libkf6parts6 libkf6kio6 libkf6coreaddons6 libkf6xmlgui6`, Arch: `kparts kio kcoreaddons kxmlgui`). Hosts KParts (Okular, …) installed separately. |
| **officeview** | qt6/gtk3 | freetype + harfbuzz (`libfreetype6 libharfbuzz0b`); MuPDF built static. At runtime shells out to LibreOffice / EuroOffice for conversion. |
| **logview** | qt6 | Qt6 WaylandClient (`libqt6waylandclient6`, optional). abseil + RE2 are vendored/static. |
| | gtk3 | — |
| **mpv_wayland** | qt6 | Qt6 OpenGLWidgets; libmpv (`libmpv2`, Arch: `mpv`) |
| | gtk3 | libmpv (`libmpv2`) |
| **kate** | qt6 | KDE Frameworks 6: TextEditor, Config (`libkf6texteditor6 libkf6configcore6`, Arch: `ktexteditor kconfig`) |
| **diagramview** | qt6 | Qt6 Svg + SvgWidgets (`libqt6svg6 libqt6svgwidgets6`). Renders Mermaid/PlantUML via external CLIs when configured. |
| | gtk3 | — |
| **mdk** | qt6 | Qt6 OpenGL/OpenGLWidgets; **libmdk** (`libmdk.so`) is a `dlopen()`-at-runtime soft dependency — install the MDK SDK runtime for playback, otherwise the plugin loads but reports MDK unavailable. |
| | gtk3 | libmdk (as above) |
| **markdownview** | qt6 | Qt6 PrintSupport; cairo, librsvg2, freetype (for MicroTeX math). md4c/MicroTeX/tinyxml2 vendored. |
| | gtk3 | cairomm (`libcairomm-1.0-1v5`), pangomm (`libpangomm-1.4-1v5`), librsvg2, cairo, freetype |
| **archiveview** | qt6 | Qt6 PrintSupport + DBus; libarchive (`libarchive13`, Arch: `libarchive`) |
| | gtk3 | libarchive |
| **svgview** | qt6/gtk3 | librsvg2 (`librsvg2-2`), cairo (`libcairo2`) |
| **pdfview** | qt6 | Qt6 PrintSupport; MuPDF (`libmupdf`) + djvulibre (`libdjvulibre21`, Arch: `djvulibre`) |
| | gtk3 | MuPDF + djvulibre |
| **gstplayer** | gtk2 | GTK 2 (`libgtk2.0-0`), GStreamer 1.0 + base plugins (`libgstreamer1.0-0 libgstreamer-plugins-base1.0-0`) |
| **fileinfo** | — | POSIX shell + common CLI utilities (`file`, `stat`, …); no toolkit libraries |

## WDX / WFX / WCX (Pascal/FPC, C, Rust)

These are small and link only what they need; the loader resolves it from the
system. None bundles a toolkit.

| Plugin | Language | Additional runtime libraries |
|---|---|---|
| **wdx/crx_wdx** | Rust | none (self-contained) |
| **wdx/exif** | C | none beyond libc |
| **wdx/ooinfo** | Pascal/FPC | none (FPC RTL is static) |
| **wdx/ooxml** | C/C++ | libz (`zlib1g`); minizip/tinyxml vendored |
| **wdx/similarity** | C++ | none beyond libstdc++ |
| **wdx/xpi_wdx** | Pascal/FPC | none |
| **wdx/mediainfo** | Lua | `mediainfo` CLI, run by a Lua-hosting plugin |
| **wdx/translitwdx** | Lua | run by a Lua-hosting plugin |
| **wcx/diskdir** | Pascal/FPC | none |
| **wfx/gvfs** | C | glib2 + gio (`libglib2.0-0`) |
| **wfx/rclone** | Pascal/FPC | the `rclone` CLI at runtime |
