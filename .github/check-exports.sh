#!/bin/bash
# Symbol-export + runtime-dependency guard for Double Commander plugin artifacts.
#
# Part 1 (export isolation): for every .wlx/.wdx/.wfx/.wcx given, run
# `nm -D --defined-only` and FAIL if any exported DEFINED symbol is outside that
# plugin type's allowed Total Commander ABI set. This catches a regression of
# the linker version scripts in sdk/exports/*.map (e.g. a new target that forgot
# to wire one up, or a build that re-exports a vendored/C++-runtime symbol that
# could be interposed against Double Commander's own in-process toolkit stack).
#
# Part 2 (runtime deps): run `ldd` on each artifact and FAIL on any "not found".
# In the clean CI build container this proves every dynamic dependency the
# plugin was linked against is resolvable from system-provided libraries (the
# host toolkit stack MUST stay dynamic and shared; niche renderers are the only
# things static-vendored, and those are not dynamic deps).
#
# Usage:
#   check-exports.sh <file-or-dir> [more ...]
# With a directory, every *.wlx/*.wdx/*.wfx/*.wcx under it is checked.
# With no arguments, defaults to ./release (what build.sh produces).
#
# Exit status: 0 if all artifacts pass, 1 otherwise.

set -uo pipefail

# ABI allow-list per plugin type, as an extended-regex a DEFINED exported symbol
# must match. Kept in lockstep with sdk/exports/*.map. ExtensionInitialize /
# ExtensionFinalize are the optional DC "extension" API, valid for any family.
EXT='^(ExtensionInitialize|ExtensionFinalize)$'
wlx_ok="^List|$EXT"
wdx_ok="^Content|SetDefaultParams$|$EXT"
wfx_ok="^Fs|$EXT"
# WCX has no common prefix -- explicit set, must match sdk/exports/wcx.map.
wcx_ok="^(OpenArchive|OpenArchiveW|ReadHeader|ReadHeaderEx|ReadHeaderExW|ProcessFile|ProcessFileW|CloseArchive|PackFiles|PackFilesW|DeleteFiles|DeleteFilesW|GetPackerCaps|ConfigurePacker|StartMemPack|StartMemPackW|PackToMem|DoneMemPack|CanYouHandleThisFile|CanYouHandleThisFileW|PackSetDefaultParams|SetChangeVolProc|SetChangeVolProcW|SetProcessDataProc|SetProcessDataProcW|GetBackgroundFlags|PkSetCryptCallback|PkSetCryptCallbackW)$|$EXT"

fail=0
checked=0

check_one() {
    local art="$1" ok pat_name
    case "$art" in
        *.wlx) ok="$wlx_ok"; pat_name="List*" ;;
        *.wdx) ok="$wdx_ok"; pat_name="Content*" ;;
        *.wfx) ok="$wfx_ok"; pat_name="Fs*" ;;
        *.wcx) ok="$wcx_ok"; pat_name="packer ABI" ;;
        *) return 0 ;;
    esac
    checked=$((checked + 1))

    # Exported DEFINED symbols (dynsym is present even in a stripped .so).
    # Column layout: "<value> <type> <name>"; take the name.
    local defined bad
    defined=$(nm -D --defined-only "$art" 2>/dev/null | awk '{print $NF}' | grep -c .)
    bad=$(nm -D --defined-only "$art" 2>/dev/null | awk '{print $NF}' | grep -vE "$ok" || true)

    if [ -n "$bad" ]; then
        fail=1
        echo "FAIL  $art"
        echo "      exports $defined symbol(s); the following are outside the allowed $pat_name ABI set:"
        echo "$bad" | sed 's/^/        /' | head -40
        local n; n=$(printf '%s\n' "$bad" | grep -c .)
        [ "$n" -gt 40 ] && echo "        ... and $((n - 40)) more"
    else
        echo "ok    $art  ($defined exported ABI symbol(s))"
    fi

    # Runtime dependencies must all resolve in this (CI) environment.
    local missing
    missing=$(ldd "$art" 2>/dev/null | grep "not found" || true)
    if [ -n "$missing" ]; then
        fail=1
        echo "FAIL  $art  has unresolved runtime dependencies:"
        echo "$missing" | sed 's/^/        /'
    fi
}

targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=("release")

for t in "${targets[@]}"; do
    if [ -d "$t" ]; then
        while IFS= read -r -d '' f; do
            check_one "$f"
        done < <(find "$t" -type f \( -name '*.wlx' -o -name '*.wdx' -o -name '*.wfx' -o -name '*.wcx' \) -print0 | sort -z)
    elif [ -f "$t" ]; then
        check_one "$t"
    else
        echo "warning: '$t' is not a file or directory, skipping" >&2
    fi
done

echo
if [ "$checked" -eq 0 ]; then
    echo "check-exports: no plugin artifacts found to check" >&2
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "check-exports: FAILED ($checked artifact(s) checked)"
    exit 1
fi
echo "check-exports: all $checked artifact(s) passed"
