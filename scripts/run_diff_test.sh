#!/usr/bin/env bash
# Decode one or more files via legacy and fast paths in separate
# processes and assert byte-identical output. Runs every input through
# both code paths, reports per-file pass/fail, and exits non-zero at
# the end if any file failed (does not stop at the first mismatch).
#
# Usage:
#   scripts/run_diff_test.sh <file> [file ...]
#   scripts/run_diff_test.sh --include-from manifest.txt
#
# The dump binary must already be built (cmake --build build).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUMP="$ROOT/build/bin/test_mqc_dump"

if [ ! -x "$DUMP" ]; then
    echo "missing $DUMP; run cmake --build build first" >&2
    exit 2
fi

files=()
if [ "${1:-}" = "--include-from" ]; then
    shift
    while IFS= read -r line || [ -n "$line" ]; do
        [ -z "$line" ] && continue
        case "$line" in '#'*) continue;; esac
        files+=("$line")
    done < "$1"
else
    files=("$@")
fi

if [ ${#files[@]} -eq 0 ]; then
    echo "no files to test" >&2
    exit 2
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

n_ok=0; n_fail=0; failures=()
for f in "${files[@]}"; do
    legacy="$tmpdir/legacy.bin"
    fast="$tmpdir/fast.bin"
    OPJ_T1_FAST=0 "$DUMP" "$f" > "$legacy" 2> "$tmpdir/legacy.err" || {
        n_fail=$((n_fail+1)); failures+=("$f (legacy decode failed)"); continue
    }
    OPJ_T1_FAST=1 "$DUMP" "$f" > "$fast" 2> "$tmpdir/fast.err" || {
        n_fail=$((n_fail+1)); failures+=("$f (fast decode failed)"); continue
    }
    if cmp -s "$legacy" "$fast"; then
        n_ok=$((n_ok+1))
    else
        n_fail=$((n_fail+1))
        failures+=("$f (pixel mismatch)")
    fi
done

echo "diff-test: $n_ok ok, $n_fail failed (of $((n_ok + n_fail)))"
if [ $n_fail -gt 0 ]; then
    printf '  %s\n' "${failures[@]}" >&2
    exit 1
fi
