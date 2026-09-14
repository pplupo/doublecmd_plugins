#!/bin/bash
# Runs the fixture corpus through scan_smoke and checks the results.
#
#   ./tests/run_suite.sh <fixtures-dir> [path-to-scan_smoke]
#
# Fixtures come from make_fixtures.sh + make_hostile.py. Every case here is a
# thing that either crashed, hung, silently lost data, or executed the
# filename in the plugin this one replaces.
#
# The suite runs from a scratch directory and fails if any archive's *name* or
# any member's name manages to create a file there.
set -uo pipefail

FIXTURES="${1:?usage: run_suite.sh <fixtures-dir> [scan_smoke]}"
SMOKE="${2:-$(dirname "$0")/../build/scan_smoke}"
SMOKE="$(readlink -f "$SMOKE")"
# Resolved after SMOKE is absolute: the suite cd's into a scratch directory,
# so a relative path here would break every extraction case into a SKIP.
EXTRACT="$(dirname "$SMOKE")/extract_smoke"
FIXTURES="$(readlink -f "$FIXTURES")"

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
cd "$SCRATCH"

PASS=0
FAIL=0
TIMEOUT=120

# check <fixture> <description> <extra-args> <expected-regex>...
check() {
    local fixture="$1"; shift
    local description="$1"; shift
    local extra="$1"; shift

    if [ ! -e "$FIXTURES/$fixture" ]; then
        printf '  \033[33mSKIP\033[0m %-28s %s (fixture missing)\n' "$fixture" "$description"
        return
    fi

    local output
    output="$(timeout "$TIMEOUT" "$SMOKE" "$FIXTURES/$fixture" $extra 2>&1)"
    local status=$?

    if [ $status -eq 124 ]; then
        printf '  \033[31mFAIL\033[0m %-28s %s (timed out after %ss)\n' \
            "$fixture" "$description" "$TIMEOUT"
        FAIL=$((FAIL + 1))
        return
    fi
    if [ $status -ge 128 ]; then
        printf '  \033[31mFAIL\033[0m %-28s %s (killed by signal %s)\n' \
            "$fixture" "$description" "$((status - 128))"
        FAIL=$((FAIL + 1))
        return
    fi

    local pattern missing=""
    for pattern in "$@"; do
        grep -qE "$pattern" <<<"$output" || missing="$missing\n      expected: $pattern"
    done

    if [ -n "$missing" ]; then
        printf '  \033[31mFAIL\033[0m %-28s %s' "$fixture" "$description"
        printf "$missing\n"
        FAIL=$((FAIL + 1))
    else
        printf '  \033[32mPASS\033[0m %-28s %s\n' "$fixture" "$description"
        PASS=$((PASS + 1))
    fi
}

echo "== well-formed archives =="
check tree.zip        "tree structure, symlink target" "--list 20" \
    "^entries +: 8 " "TREE OK" "link-to-deep.*a/b/c/deep.txt"
check tree.tar.gz     "gzip filter reported"           "" \
    "^entries +: 8 " "filters +: gzip" "TREE OK"
check many.zip        "100k entries, progressive"      "" \
    "^entries +: 100100 " "TREE OK" "batches +: [0-9]{2,}"
check many.zip        "flat mode consistency"          "--flat" \
    "^entries +: 100100 " "TREE OK"

echo
echo "== shell metacharacters in the ARCHIVE name =="
for nasty in "$FIXTURES"/*'touch archiveview-pwned'*.zip "$FIXTURES"/*'$('*.zip; do
    [ -e "$nasty" ] || continue
    name="$(basename "$nasty" | tr '\n' '~')"
    if timeout "$TIMEOUT" "$SMOKE" "$nasty" 2>&1 | grep -q "TREE OK"; then
        printf '  \033[32mPASS\033[0m %-28s listed, no shell\n' "${name:0:28}"
        PASS=$((PASS + 1))
    else
        printf '  \033[31mFAIL\033[0m %-28s did not list\n' "${name:0:28}"
        FAIL=$((FAIL + 1))
    fi
done

echo
echo "== hostile contents =="
check nasty-names.tar "traversal/absolute names shown verbatim" "--flat --list 20" \
    "TREE OK" "\.\./\.\./\.\./\.\./etc/passwd" "/absolute/path/file\.txt"
check nasty-names.tar "double slashes collapsed"       "--list 20" \
    "TREE OK" "^ +a \|"
check bad-metadata.tar "unset sizes, odd modes, devices" "" \
    "^entries +: 13 " "TREE OK"
check structure.tar   "300-deep, dup, collide, 5k wide" "" \
    "^entries +: 5008 " "^duplicates +: 1$" "TREE OK"
check structure.zip   "duplicate paths kept separate"  "" \
    "^entries +: 8 " "^duplicates +: 1$" "TREE OK"
check bomb.zip        "2.5 GiB declared, listed instantly" "" \
    "^entries +: 10 " "uncompressed +: 2\.50 GiB" "TREE OK" "total elapsed +: [0-9]{1,3} ms"
check badnames.zip    "non-UTF-8 member names"         "" \
    "^entries +: 2 " "TREE OK"

echo
echo "== ZIP central directory (comment, packed size, CRC) =="
# Cross-checked against unzip -v by hand; the CRCs below are its output.
check commented.zip   "comment read without a shell"   "" \
    "comment +: archive comment with .* metacharacters" "TREE OK"
check structure.zip   "packed size and CRC per entry"  "--flat --list 10" \
    "dup/file\.txt \| 5 bytes \| 7 bytes \| 140\.0% \| 9271EE57" \
    "dup/file\.txt \| 6 bytes \| 8 bytes \| 133\.3% \| B61F1169"
check many.zip        "ZIP64 (over 65535 entries)"     "" \
    "zip64 +: yes" "packed \(entries\): 663\.09 KiB"
check bomb.zip        "real per-entry ratio"           "--flat --list 3" \
    "zeros00\.bin \| 256\.00 MiB \| 254\.80 KiB \| 0\.1% \| 2A0E7DBB"
check tree.tar.gz     "tar: no CD, columns stay blank" "--flat --list 3" \
    "packed \(entries\): -" "top\.txt \| 4 bytes \|  \|  \| "
check eocd-in-data.zip "decoy EOCD signature in payload" "" \
    "^entries +: 2 " "comment +: the genuine comment" "TREE OK"
check eocd-in-comment.zip "decoy EOCD inside the comment" "" \
    "comment +: .*trailing text after a fake EOCD signature" "TREE OK"
check lying-cd-size.zip "CD size larger than the file"  "" \
    "^entries +: 1 " "TREE OK"
check dangling-cd-offset.zip "CD offset past end of file" "" \
    "^entries +: 1 " "TREE OK"

echo
echo "== encryption =="
# Listing an encrypted archive needs no passphrase: names, sizes and CRCs live
# in headers, and entry bodies are skipped rather than decrypted. The prompt
# exists for the paths that do read data.
check encrypted-zipcrypto.zip "ZipCrypto: lists, flags locked, no prompt" "--flat --list 5" \
    "^entries +: 2 " "encrypted +: yes" "L  secret\.txt" "TREE OK"
check encrypted-aes.zip   "ZIP AES: lists, flags locked"    "--flat --list 5" \
    "^entries +: 2 " "encrypted +: yes" "L  secret\.txt" "TREE OK"
check encrypted-entries.7z "7z entry encryption: lists"     "--flat --list 5" \
    "^entries +: 2 " "encrypted +: yes" "L  secret\.txt" "TREE OK"
# libarchive cannot read 7z encrypted headers at all, passphrase or not. What
# matters is that this is a clean immediate error: the predecessor shelled out
# to `7z l`, which prompted on an unconnected stdin and froze DC for the full
# 30-second QProcess timeout.
check encrypted-header.7z "7z header encryption: clean refusal" "" \
    "ERROR +: The archive header is encrypted" "total elapsed +: [0-9]{1,3} ms"
# Packed size is the figure the central directory stores, which includes
# per-entry encryption overhead. unzip -v subtracts it (28 -> 16 here), so
# these values deliberately differ from unzip's for encrypted entries only.
check encrypted-zipcrypto.zip "packed includes crypto overhead" "--flat --list 5" \
    "secret\.txt \| 11 bytes \| 23 bytes"
check tree.zip            "directories: no fake 0-byte/CRC"  "--flat --list 4" \
    "^ +a \|  \|  \|  \|  \|"

echo
echo "== damaged and degenerate input =="
check empty.tar       "empty archive"                  "" \
    "^entries +: 0 " "TREE OK"
check plain.txt       "not an archive"                 "" \
    "ERROR +: Unrecognized archive format"
check truncated.zip   "truncated mid-archive"          "" \
    "ERROR" "TREE OK"
check truncated.tar.bz2 "cut off inside the stream"    "" \
    "ERROR" "TREE OK"
check corrupt.zip     "garbage in the middle"          "" \
    "ERROR" "TREE OK"

echo
echo "== limits and cancellation =="
check many.zip        "entry ceiling honoured"         "--max-entries 1000" \
    "^entries +: 1000 " "truncated +: yes" "TREE OK"
check slow.tar.bz2    "cancel interrupts decompression" "--cancel-after 300" \
    "cancelled +: yes" "cancel latency +: [0-9]{1,3} ms"

echo
echo "== ListLoad's bounded format probe =="
# Each of these runs on DC's UI thread before ListLoad returns, so the cost
# matters as much as the verdict. Two digits of milliseconds, at most.
probe() {
    local fixture="$1" expected="$2"
    [ -e "$FIXTURES/$fixture" ] || return
    local output
    output="$(timeout 60 "$SMOKE" "$FIXTURES/$fixture" --probe 2>&1)"
    if grep -qE "probe +: $expected +\\([0-9]{1,2} ms\\)" <<<"$output"; then
        printf '  \033[32mPASS\033[0m %-28s %s\n' "$fixture" "$output"
        PASS=$((PASS + 1))
    else
        printf '  \033[31mFAIL\033[0m %-28s %s (wanted %s, under 100ms)\n' \
            "$fixture" "$output" "$expected"
        FAIL=$((FAIL + 1))
    fi
}
probe plain.txt      "not an archive"
probe many.zip       "archive"
probe slow.tar.bz2   "archive"
probe solid.tar.xz   "archive"
probe empty.tar      "archive"
probe corrupt.zip    "archive"

echo
echo "== exported C ABI, driven like Double Commander does =="
WLX="$(dirname "$SMOKE")/archiveview_qt6.wlx"
HOST="$(dirname "$SMOKE")/wlx_host"
if [ -x "$HOST" ] && [ -e "$WLX" ]; then
    host_output="$(timeout 300 "$HOST" "$WLX" \
        "$FIXTURES/tree.zip" "$FIXTURES/many.zip" "$FIXTURES/plain.txt" \
        "$FIXTURES/structure.tar" "$FIXTURES/corrupt.zip" 2>&1)"
    host_status=$?
    if [ $host_status -eq 0 ] && grep -q "WLX HOST OK" <<<"$host_output" \
       && grep -q "ListLoad declined" <<<"$host_output" \
       && ! grep -q "DOES NOT FILL THE PANE" <<<"$host_output"; then
        printf '  \033[32mPASS\033[0m %-28s load/search/commands/reload/fill/teardown\n' "wlx_host"
        PASS=$((PASS + 1))
    else
        printf '  \033[31mFAIL\033[0m %-28s exit=%s\n%s\n' "wlx_host" "$host_status" "$host_output"
        FAIL=$((FAIL + 1))
    fi
else
    printf '  \033[33mSKIP\033[0m %-28s (build the wlx_host target)\n' "wlx_host"
fi

echo
echo "== configuration (ini) =="
INI="$SCRATCH/archiveview.ini"
cat > "$INI" <<'INIEOF'
[archiveview]
FlatView=true
FilterBox=false
MaxEntries=250
NameCodec=windows-1251
HiddenColumns=CRC-32,Owner,Mode
INIEOF
check many.zip        "ini: flat, ceiling, hidden columns" "--ini $INI" \
    "settings +: flat=yes filter=no maxEntries=250 codec=windows-1251 hidden=CRC-32\+Owner\+Mode" \
    "^entries +: 250 " "truncated +: yes"
# The legacy-codepage gap documented since M1: without NameCodec these names
# are lossless but wrong-looking; with it they are correct.
check badnames.zip    "NameCodec decodes CP1251 names"  "--ini $INI --flat --list 3" \
    "привет\.txt"
cat > "$SCRATCH/single.ini" <<'INIEOF'
[archiveview]
HiddenColumns=Owner
MaxEntries=0
INIEOF
check tree.zip        "ini: single value, invalid ceiling ignored" "--ini $SCRATCH/single.ini" \
    "maxEntries=500000 codec=- hidden=Owner"

echo
echo "== selecting a directory means everything under it =="
# Regression: the Qt widget used to find children by walking the view's rows,
# but Qt returns selections in the name column and only column-0 indexes have
# children -- so extracting a directory extracted just the directory.
SELECT="$(dirname "$SMOKE")/select_smoke"
select_case() {
    local fixture="$1" root="$2" expected="$3" description="$4"
    if [ ! -e "$FIXTURES/$fixture" ] || [ ! -x "$SELECT" ]; then
        printf '  \033[33mSKIP\033[0m %-28s %s\n' "$fixture" "$description"
        return
    fi
    local output
    output="$(timeout 300 "$SELECT" "$FIXTURES/$fixture" "$root" "$expected" 2>&1)"
    if grep -q "SELECTION OK" <<<"$output"; then
        printf '  \033[32mPASS\033[0m %-28s %s\n' "$fixture" "$description"
        PASS=$((PASS + 1))
    else
        printf '  \033[31mFAIL\033[0m %-28s %s\n%s\n' "$fixture" "$description" "$output"
        FAIL=$((FAIL + 1))
    fi
}
select_case structure.tar wide     5000 "5000 files under one directory"
# 1001, not 1000: zip -r stores an explicit dir7/ entry alongside its files.
select_case many.zip      dir7     1001 "1000 files plus the stored dir entry"
select_case structure.tar level000    1 "300 levels deep, one real member"
select_case structure.tar dup         1 "duplicate paths collapse to one target"

echo
echo "== extraction: containment is the whole point =="
# The listing shows hostile member names verbatim on purpose. Extraction is
# where that becomes dangerous, so the property under test is the opposite
# one: nothing may land outside the destination.
extract_case() {
    local fixture="$1" description="$2"; shift 2
    if [ ! -e "$FIXTURES/$fixture" ] || [ ! -x "$EXTRACT" ]; then
        printf '  \033[33mSKIP\033[0m %-28s %s\n' "$fixture" "$description"
        return
    fi

    local dest
    dest="$(mktemp -d "$SCRATCH/extract.XXXXXX")"
    local output
    output="$(timeout 300 "$EXTRACT" "$FIXTURES/$fixture" "$dest" "$@" 2>&1)"
    local status=$?

    local pattern missing=""
    # Containment is asserted for every case, on top of whatever else.
    grep -q "CONTAINED OK" <<<"$output" || missing="$missing\n      containment FAILED"
    if [ $status -ge 128 ]; then
        missing="$missing\n      killed by signal $((status - 128))"
    fi

    if [ -n "$missing" ]; then
        printf '  \033[31mFAIL\033[0m %-28s %s' "$fixture" "$description"
        printf "$missing\n"
        FAIL=$((FAIL + 1))
    else
        printf '  \033[32mPASS\033[0m %-28s %s\n' "$fixture" "$description"
        PASS=$((PASS + 1))
    fi
}

extract_expect() {
    local fixture="$1" description="$2" pattern="$3"; shift 3
    if [ ! -e "$FIXTURES/$fixture" ] || [ ! -x "$EXTRACT" ]; then
        printf '  \033[33mSKIP\033[0m %-28s %s\n' "$fixture" "$description"
        return
    fi
    local dest
    dest="$(mktemp -d "$SCRATCH/extract.XXXXXX")"
    local output
    output="$(timeout 300 "$EXTRACT" "$FIXTURES/$fixture" "$dest" "$@" 2>&1)"
    if grep -q "CONTAINED OK" <<<"$output" && grep -qE "$pattern" <<<"$output"; then
        printf '  \033[32mPASS\033[0m %-28s %s\n' "$fixture" "$description"
        PASS=$((PASS + 1))
    else
        printf '  \033[31mFAIL\033[0m %-28s %s\n      wanted: %s\n%s\n' \
            "$fixture" "$description" "$pattern" "$output"
        FAIL=$((FAIL + 1))
    fi
}

extract_expect nasty-names.tar "traversal/absolute refused" "^refused +: [1-9]"
extract_expect nasty-names.tar "selecting a traversal member writes nothing" \
    "^extracted +: 0" --member "../../../../etc/passwd"
extract_expect symlink-escape.tar "symlink out of tree refused" "^refused +: [1-9]"
extract_expect tree.zip        "ordinary archive extracts"    "^extracted +: [1-9]"
extract_expect many.zip        "100k members extracted"       "^extracted +: 100100"
extract_expect encrypted-zipcrypto.zip "encrypted: extracts with passphrase" \
    "^extracted +: 2" --passphrase "correct horse battery staple"
extract_expect encrypted-aes.zip "AES: extracts with passphrase" \
    "^extracted +: 2" --passphrase "correct horse battery staple"
extract_expect encrypted-zipcrypto.zip "encrypted: declines without one, no hang" \
    "ERROR +: Passphrase required"
extract_expect encrypted-zipcrypto.zip "wrong passphrase stops at the retry cap" \
    "passphrase asked: attempt 5" --passphrase "wrong"
extract_case   bomb.zip        "bomb: extraction is bounded by selection" \
    --member "zeros00.bin"

echo
echo "== nothing escaped to /tmp during any of that =="
if compgen -G "/tmp/archiveview-escaped*" > /dev/null; then
    printf '  \033[31mFAIL\033[0m symlink escape wrote: %s\n' "$(echo /tmp/archiveview-escaped*)"
    FAIL=$((FAIL + 1))
else
    printf '  \033[32mPASS\033[0m no files written outside any destination\n'
    PASS=$((PASS + 1))
fi

echo
echo "== GTK3 variant: same core, different toolkit =="
GTKWLX="$(dirname "$SMOKE")/archiveview_gtk3.wlx"
GTKHOST="$(dirname "$SMOKE")/gtk_host"
if [ -x "$GTKHOST" ] && [ -e "$GTKWLX" ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    gtk_output="$(GDK_BACKEND=x11 timeout 600 "$GTKHOST" "$GTKWLX" \
        "$FIXTURES/structure.zip" "$FIXTURES/many.zip" "$FIXTURES/plain.txt" 2>&1)"
    gtk_status=$?
    # The walk exercises get_path/get_iter round-tripping, which is the pair a
    # hand-written GtkTreeModel most easily gets wrong.
    if [ $gtk_status -eq 0 ] && grep -q "GTK HOST OK" <<<"$gtk_output" \
       && grep -q "rows visible    : 100100" <<<"$gtk_output" \
       && ! grep -q "path round-trip : FAILED" <<<"$gtk_output" \
       && ! grep -q "MISMATCH" <<<"$gtk_output" \
       && grep -q "filter 'deep'   : 101 of 110 rows" <<<"$gtk_output" \
       && grep -q "drag source     : text/uri-list advertised" <<<"$gtk_output" \
       && grep -q "activate dir    : expanded" <<<"$gtk_output" \
       && grep -q "ListLoad declined" <<<"$gtk_output"; then
        printf '  \033[32mPASS\033[0m %-28s model, filter, activation, drag, 100k rows\n' "gtk_host"
        PASS=$((PASS + 1))
    else
        printf '  \033[31mFAIL\033[0m %-28s exit=%s\n%s\n' "gtk_host" "$gtk_status" "$gtk_output"
        FAIL=$((FAIL + 1))
    fi
else
    printf '  \033[33mSKIP\033[0m %-28s (no display, or GTK3 variant not built)\n' "gtk_host"
fi

echo
echo "== no shell was ever involved =="
if compgen -G "archiveview-pwned*" > /dev/null; then
    printf '  \033[31mFAIL\033[0m marker files created: %s\n' "$(echo archiveview-pwned*)"
    FAIL=$((FAIL + 1))
else
    printf '  \033[32mPASS\033[0m no marker files created in the scratch directory\n'
    PASS=$((PASS + 1))
fi

echo
echo "-------------------------------------------"
printf 'passed: %d   failed: %d\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
