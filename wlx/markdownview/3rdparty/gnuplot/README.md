# Vendored gnuplot (2D-only, minimal build)

`gnuplot-linux-x86_64` is gnuplot 6.0.3, built from unmodified upstream
source with a stripped-down configuration -- no X11 terminals, no
wxWidgets/Qt terminals, no Lua/TikZ, no legacy dot-matrix/Tektronix/gd-based
image terminals, no readline. Only the cairo/pango-based terminals remain
(`pngcairo`, used for chart rendering), which reuse the exact same
cairo/pango/freetype/fontconfig/harfbuzz/glib libraries this plugin already
links for diagram rendering and font handling -- so the only genuinely new
runtime library beyond what markdownview already requires is `libwebp`.

Built with:

```bash
./configure --without-x --without-lua --disable-wxwidgets --with-qt=no \
  --without-latex --disable-x11-mbfonts --disable-x11-external \
  --without-tektronix --with-gd=no --without-readline
make -j$(nproc)
strip src/gnuplot
```

Result: a 1.5MB stripped binary (vs. the distro `gnuplot-nox` package's
1.7MB binary + ~2.6MB of installed extras this plugin doesn't need).

Self-seeded to `~/.config/doublecmd/markdownview_gnuplot/gnuplot` on first
chart render that needs it (see `chart_render.cpp`), the same pattern the
8 embedded math fonts already use -- embedded in the compiled `.wlx`
binary, written out once, never re-downloaded or dependent on the user's
system having gnuplot installed. If self-seeding fails for any reason
(read-only filesystem, unsupported architecture, etc.), chart rendering
falls back to the plugin's own native Cairo renderer -- gnuplot/Matplot++
is the preferred path when available, never a hard requirement.

License: gnuplot's own permissive license (see `LICENSE` in this
directory) -- unmodified upstream source, built with different configure
flags only, so redistributing the resulting binary carries no obligation
to also ship a source patch.

Source: https://sourceforge.net/projects/gnuplot/files/gnuplot/6.0.3/
