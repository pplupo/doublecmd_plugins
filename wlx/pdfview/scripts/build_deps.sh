#!/bin/bash
set -e

DEPS_DIR="$(pwd)/deps_build"
INSTALL_DIR="$(pwd)/deps_install"

mkdir -p "$DEPS_DIR"
mkdir -p "$INSTALL_DIR"

export CFLAGS="-fPIC $CFLAGS"
export CXXFLAGS="-fPIC $CXXFLAGS"

echo "=== Building MuPDF ==="
if [ ! -d "$DEPS_DIR/mupdf" ]; then
    cd "$DEPS_DIR"
    git clone --recursive --depth 1 -b 1.24.0 https://github.com/ArtifexSoftware/mupdf.git
fi
cd "$DEPS_DIR/mupdf"
make HAVE_X11=no HAVE_GLUT=no prefix="$INSTALL_DIR" install-libs

echo "=== Building DjVuLibre ==="
if [ ! -d "$DEPS_DIR/djvulibre" ]; then
    cd "$DEPS_DIR"
    git clone --depth 1 https://git.code.sf.net/p/djvu/djvulibre-git djvulibre
fi
cd "$DEPS_DIR/djvulibre"
./autogen.sh
./configure --prefix="$INSTALL_DIR" --enable-static --disable-shared --with-pic
make -j$(nproc)
make install

# Ghostscript/libspectre (PostScript support) are intentionally not built:
# ghostpdl's `make install` only produces the `gs` CLI binary, not a
# linkable libgs, so there's nothing for libspectre to link against.
# pdfview handles PDF (mupdf) and DjVu (djvulibre) only.

echo "Dependencies built successfully."
