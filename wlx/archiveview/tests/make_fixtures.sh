#!/bin/bash
# Generates archive fixtures for the archiveview harness.
#
# Output goes to $1 (default: ./fixtures-out), which is deliberately not
# committed — several fixtures are large, and all of them are reproducible.
#
# The shell-injection fixture is the important one: it is a file whose *name*
# is a command substitution. Previewing it must list an archive, and must not
# create /tmp/archiveview-pwned.
set -euo pipefail

OUT="${1:-$(dirname "$0")/fixtures-out}"
mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "fixtures → $OUT"

# --- ordinary small zip with a directory tree ---------------------------------
mkdir -p "$WORK/tree/a/b/c" "$WORK/tree/a/d"
echo "hello" > "$WORK/tree/a/b/c/deep.txt"
echo "world" > "$WORK/tree/a/d/other.txt"
echo "top"   > "$WORK/tree/top.txt"
ln -sf a/b/c/deep.txt "$WORK/tree/link-to-deep"
(cd "$WORK/tree" && zip -qry "$OUT/tree.zip" . --symlinks)
(cd "$WORK/tree" && tar czf "$OUT/tree.tar.gz" .)

# --- shell injection: the filename IS the payload -----------------------------
# Anything that splices this name into a shell command line executes it.
# The payloads use relative paths because a filename cannot contain '/' —
# a successful exploit drops its marker in whatever directory the host ran from.
cp "$OUT/tree.zip" "$OUT/x\$(touch archiveview-pwned-subst).zip"
cp "$OUT/tree.zip" "$OUT/y\`touch archiveview-pwned-backtick\`.zip"
cp "$OUT/tree.zip" "$OUT/z;touch archiveview-pwned-semi;.zip"
cp "$OUT/tree.zip" "$OUT/w|touch archiveview-pwned-pipe.zip"
cp "$OUT/tree.zip" "$OUT/v&touch archiveview-pwned-amp&.zip"
cp "$OUT/tree.zip" "$OUT/u
newline.zip"

# --- many entries: the case QTableWidget could not survive ---------------------
if [ ! -f "$OUT/many.zip" ]; then
    mkdir -p "$WORK/many"
    (cd "$WORK/many" && for d in $(seq 0 99); do
        mkdir -p "dir$d"
        for f in $(seq 0 999); do echo "$d/$f" > "dir$d/file$f.txt"; done
    done)
    (cd "$WORK/many" && zip -qry "$OUT/many.zip" .)
fi

# --- solid stream: listing requires decompressing everything -------------------
# The cancellation test target. Incompressible payload so the archive stays
# large enough that a cancel lands mid-stream.
if [ ! -f "$OUT/solid.tar.xz" ]; then
    mkdir -p "$WORK/solid"
    for i in $(seq 0 60); do
        head -c 8M /dev/urandom > "$WORK/solid/blob$i.bin"
    done
    (cd "$WORK/solid" && tar cf - . | xz -0 -T0 > "$OUT/solid.tar.xz")
fi

# --- slow solid stream: the cancellation target --------------------------------
# solid.tar.xz above decodes at memcpy speed because its payload is
# incompressible, which is too fast to cancel mid-stream. Real compressible
# content through bzip2 decodes at tens of MB/s, so listing it takes seconds
# and a cancel lands squarely inside archive_read_next_header().
if [ ! -f "$OUT/slow.tar.bz2" ]; then
    mkdir -p "$WORK/slow"
    for i in $(seq 0 7); do
        # Compressible but not degenerate: repeated text with varying content.
        # Generated with awk rather than `yes | head`, which dies on SIGPIPE
        # under `set -o pipefail`.
        awk -v seed="$i" 'BEGIN {
            for (n = 0; n < 700000; n++)
                print "the quick brown fox jumps over the lazy dog " seed " " n
        }' > "$WORK/slow/text$i.txt"
    done
    (cd "$WORK/slow" && tar cf - . | bzip2 -9 > "$OUT/slow.tar.bz2")
fi

# --- non-UTF-8 member names ----------------------------------------------------
mkdir -p "$WORK/enc"
printf 'cyrillic\n' > "$WORK/enc/$(printf '\xef\xf0\xe8\xe2\xe5\xf2.txt')"   # CP1251
printf 'shiftjis\n' > "$WORK/enc/$(printf '\x83\x65\x83\x58\x83\x67.txt')"   # Shift-JIS
(cd "$WORK/enc" && zip -qry "$OUT/badnames.zip" .)

# --- encrypted archives ---------------------------------------------------
# Three cases that behave differently, not three flavours of one case:
#   * ZipCrypto  — entry data encrypted, names and sizes readable
#   * ZIP AES    — same shape, modern cipher, different libarchive path
#   * 7z -mhe=on — the header itself is encrypted, so *nothing* lists without
#                  the passphrase. This is the one that produced a 30-second
#                  frozen file manager in the predecessor: `7z l` prompted on
#                  a stdin that was not connected, and the UI thread waited
#                  out the full QProcess timeout.
PASSPHRASE="correct horse battery staple"
mkdir -p "$WORK/secret"
echo "classified" > "$WORK/secret/secret.txt"
echo "also classified" > "$WORK/secret/other.txt"

(cd "$WORK/secret" && zip -qr -P "$PASSPHRASE" "$OUT/encrypted-zipcrypto.zip" .)
7z a -tzip -mem=AES256 -p"$PASSPHRASE" "$OUT/encrypted-aes.zip" "$WORK/secret/"* >/dev/null 2>&1 || true
7z a -t7z -mhe=on -p"$PASSPHRASE" "$OUT/encrypted-header.7z" "$WORK/secret/"* >/dev/null 2>&1 || true
7z a -t7z -p"$PASSPHRASE" "$OUT/encrypted-entries.7z" "$WORK/secret/"* >/dev/null 2>&1 || true

# --- truncated / corrupt: the read must fail cleanly, not crash ----------------
# Cut off mid-stream, so the failure lands inside the decompressor rather
# than at a clean entry boundary.
head -c "$(( $(stat -c%s "$OUT/slow.tar.bz2") / 2 ))" "$OUT/slow.tar.bz2" > "$OUT/truncated.tar.bz2"
head -c 200000 "$OUT/many.zip" > "$OUT/truncated.zip"
# Intact at both ends, garbage in the middle: the central directory still
# parses, so the reader walks into damage rather than failing up front.
cp "$OUT/many.zip" "$OUT/corrupt.zip"
dd if=/dev/urandom of="$OUT/corrupt.zip" bs=4096 seek=512 count=8 conv=notrunc status=none

# --- empty archive and a non-archive -------------------------------------------
: > "$WORK/empty"
(cd "$WORK" && tar cf "$OUT/empty.tar" --files-from /dev/null)
echo "not an archive at all" > "$OUT/plain.txt"

ls -la "$OUT"
