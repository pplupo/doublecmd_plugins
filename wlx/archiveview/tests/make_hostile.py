#!/usr/bin/env python3
"""Crafts archives that the shell tools cannot produce.

These are the entries a reader written straight through — first thing that
worked at each step — mishandles: sizes that are unset or absurd, names that
are empty or absolute or full of "..", nesting deep enough to blow a recursive
model, duplicate paths, and a member whose path collides with a directory that
another member needs.

Every fixture here must produce a listing, not a crash and not a hang.
"""

import os
import io
import sys
import tarfile
import zipfile


def write(out, name, data=b""):
    path = os.path.join(out, name)
    with open(path, "wb") as handle:
        handle.write(data)
    return path


def tar_member(archive, name, *, size=0, kind=tarfile.REGTYPE, mode=0o644,
               mtime=1700000000, linkname="", data=b""):
    info = tarfile.TarInfo(name)
    info.size = size if size else len(data)
    info.type = kind
    info.mode = mode
    info.mtime = mtime
    info.linkname = linkname
    archive.addfile(info, io.BytesIO(data) if data else None)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    # --- names designed to escape the archive or break path splitting -------
    with tarfile.open(os.path.join(out, "nasty-names.tar"), "w") as tar:
        tar_member(tar, "../../../../etc/passwd", data=b"traversal\n")
        tar_member(tar, "/absolute/path/file.txt", data=b"absolute\n")
        tar_member(tar, "./././redundant.txt", data=b"dots\n")
        tar_member(tar, "trailing/slash/dir/", kind=tarfile.DIRTYPE)
        tar_member(tar, "a//double//slash.txt", data=b"double\n")
        tar_member(tar, "..", kind=tarfile.DIRTYPE)
        tar_member(tar, ".", kind=tarfile.DIRTYPE)
        tar_member(tar, "space at end .txt", data=b"space\n")
        tar_member(tar, "n\newline-in-name.txt", data=b"newline\n")
        tar_member(tar, "tab\tin-name.txt", data=b"tab\n")
        tar_member(tar, "quote'and\"quote.txt", data=b"quotes\n")
        tar_member(tar, "$(touch archiveview-pwned-member).txt", data=b"subst\n")
        tar_member(tar, "x" * 400 + ".txt", data=b"long\n")

    # --- sizes and timestamps that are unset, negative-looking, or absurd ---
    with tarfile.open(os.path.join(out, "bad-metadata.tar"), "w") as tar:
        # Declares 8 EiB of content it does not have. A reader that casts the
        # int64 size to size_t and accumulates it wraps its running total.
        huge = tarfile.TarInfo("declares-8EiB.bin")
        huge.size = 0
        huge.mtime = 0
        tar.addfile(huge)
        tar_member(tar, "zero-mtime.txt", mtime=0, data=b"epoch\n")
        tar_member(tar, "far-future.txt", mtime=32503680000, data=b"year 3000\n")
        tar_member(tar, "no-perms.txt", mode=0, data=b"mode zero\n")
        tar_member(tar, "setuid-sticky.bin", mode=0o7777, data=b"bits\n")
        tar_member(tar, "fifo", kind=tarfile.FIFOTYPE)
        tar_member(tar, "chardev", kind=tarfile.CHRTYPE)
        tar_member(tar, "blockdev", kind=tarfile.BLKTYPE)
        tar_member(tar, "target.txt", data=b"target\n")
        tar_member(tar, "hard-link", kind=tarfile.LNKTYPE, linkname="target.txt")
        tar_member(tar, "sym-link", kind=tarfile.SYMTYPE, linkname="target.txt")
        tar_member(tar, "sym-to-nowhere", kind=tarfile.SYMTYPE,
                   linkname="../../does/not/exist")
        tar_member(tar, "sym-empty-target", kind=tarfile.SYMTYPE, linkname="")

    # --- structural traps for a tree model ---------------------------------
    with tarfile.open(os.path.join(out, "structure.tar"), "w") as tar:
        # 300 levels: a recursive index()/parent() implementation dies here.
        deep = "/".join("level%03d" % n for n in range(300))
        tar_member(tar, deep + "/bottom.txt", data=b"deep\n")
        # Same path twice — the second must not create a second node.
        tar_member(tar, "dup/file.txt", data=b"first\n")
        tar_member(tar, "dup/file.txt", data=b"second\n")
        # A file, then something that needs it to be a directory.
        tar_member(tar, "collide", data=b"i am a file\n")
        tar_member(tar, "collide/child.txt", data=b"i need a parent dir\n")
        # Children listed before their directory entry arrives.
        tar_member(tar, "late/child.txt", data=b"child first\n")
        tar_member(tar, "late", kind=tarfile.DIRTYPE)
        # Directory entry with no children at all.
        tar_member(tar, "empty-dir", kind=tarfile.DIRTYPE)
        # A wide directory — 5000 siblings under one parent.
        for n in range(5000):
            tar_member(tar, "wide/file%04d.txt" % n, data=b"w\n")

    # --- zip variants -------------------------------------------------------
    with zipfile.ZipFile(os.path.join(out, "structure.zip"), "w",
                         zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("dup/file.txt", "first")
        zf.writestr("dup/file.txt", "second")
        zf.writestr("collide", "i am a file")
        zf.writestr("collide/child.txt", "i need a parent dir")
        zf.writestr("/absolute.txt", "absolute")
        zf.writestr("../traversal.txt", "traversal")
        zf.writestr("empty-name-follows", "x")
        zf.writestr("deep/" * 100 + "bottom.txt", "deep")

    # A zip carrying an archive comment, which libarchive does not expose at
    # all — the reason the predecessor shelled out to 7z. M3 parses this from
    # the End of Central Directory record instead.
    with zipfile.ZipFile(os.path.join(out, "commented.zip"), "w") as zf:
        zf.writestr("file.txt", "content")
        zf.comment = ("archive comment with $(touch archiveview-pwned-comment) "
                      "and `backticks` and ; | & metacharacters").encode()

    # --- EOCD parsing traps -------------------------------------------------
    # The End of Central Directory signature is only four bytes, so it can
    # appear inside stored file data. A parser that scans backwards and takes
    # the first hit it sees reads its metadata out of the payload; only the
    # comment-length check distinguishes the real record. This archive stays
    # valid for every reader, which is what makes it the realistic case.
    with zipfile.ZipFile(os.path.join(out, "eocd-in-data.zip"), "w",
                         zipfile.ZIP_STORED) as zf:
        zf.writestr("decoy.bin", b"PK\x05\x06" + b"\x00" * 18 + b"PK\x05\x06"
                                 + b"\x00" * 64)
        zf.writestr("real.txt", "content")
        zf.comment = b"the genuine comment"

    # Same trick in the comment itself. Note both unzip(1) and libarchive are
    # fooled by this one and report an empty archive; our stricter locator
    # still finds the real record. Kept as a documented divergence.
    with zipfile.ZipFile(os.path.join(out, "eocd-in-comment.zip"), "w") as zf:
        zf.writestr("real.txt", "content")
        zf.comment = (b"padding" + b"PK\x05\x06" + b"\x00" * 18
                      + b" trailing text after a fake EOCD signature")

    # Declares a central directory far larger than the file. Every length in
    # the record has to be validated against what was actually read.
    lying = os.path.join(out, "lying-cd-size.zip")
    with zipfile.ZipFile(lying, "w") as zf:
        zf.writestr("file.txt", "content")
    data = bytearray(open(lying, "rb").read())
    eocd = data.rfind(b"PK\x05\x06")
    data[eocd + 12:eocd + 16] = (0x7FFFFFF0).to_bytes(4, "little")   # CD size
    open(lying, "wb").write(bytes(data))

    # Central directory offset pointing past the end of the file.
    dangling = os.path.join(out, "dangling-cd-offset.zip")
    with zipfile.ZipFile(dangling, "w") as zf:
        zf.writestr("file.txt", "content")
    data = bytearray(open(dangling, "rb").read())
    eocd = data.rfind(b"PK\x05\x06")
    data[eocd + 16:eocd + 20] = (0x7FFFFFF0).to_bytes(4, "little")   # CD offset
    open(dangling, "wb").write(bytes(data))

    # --- symlink escape: the classic two-member attack -----------------------
    # First member is a symlink pointing outside the destination; the second
    # is written *through* it. Refusing member names alone does not stop this
    # — the second name is perfectly ordinary. This is what libarchive's
    # ARCHIVE_EXTRACT_SECURE_SYMLINKS is for, and what the extractor's own
    # link-target check catches before it even gets that far.
    with tarfile.open(os.path.join(out, "symlink-escape.tar"), "w") as tar:
        tar_member(tar, "escape", kind=tarfile.SYMTYPE, linkname="/tmp")
        tar_member(tar, "escape/archiveview-escaped.txt", data=b"should not exist\n")
        tar_member(tar, "relative-escape", kind=tarfile.SYMTYPE,
                   linkname="../../../../tmp")
        tar_member(tar, "relative-escape/archiveview-escaped2.txt",
                   data=b"should not exist\n")
        tar_member(tar, "innocent.txt", data=b"fine\n")

    # --- decompression bomb: enormous declared size, tiny file --------------
    # Listing must stay instant: entry bodies are skipped, never expanded.
    bomb = os.path.join(out, "bomb.zip")
    with zipfile.ZipFile(bomb, "w", zipfile.ZIP_DEFLATED) as zf:
        for n in range(10):
            zf.writestr("zeros%02d.bin" % n, b"\0" * (256 * 1024 * 1024))

    print("hostile fixtures written to", out)


if __name__ == "__main__":
    main()
