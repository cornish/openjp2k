#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Toby Cornish
#
# Configure, build, and run the openjp2k ctest suite against the cross-domain
# corpus in the sister openjp2k-data repo.
#
# Without OPJ_DATA_ROOT, ~1200 conformance tests fail because they can't find
# their input files. With it (pointing at the nested openjpeg-data inside
# openjp2k-data), the full ~1587-test suite runs.
#
# Usage:
#   scripts/run-conformance.sh                  # default Release build
#   scripts/run-conformance.sh -B build-foo     # alternate build dir
#   OPJ_DATA_ROOT=/path scripts/run-conformance.sh  # explicit corpus path
#
# Extra ctest args may be appended:
#   scripts/run-conformance.sh -- -R NR-DEC -j4
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"

# Parse a small subset of flags. Everything after `--` is forwarded to ctest.
CTEST_ARGS=()
while (( $# )); do
  case "$1" in
    -B|--build) BUILD_DIR="$2"; shift 2 ;;
    --) shift; CTEST_ARGS=("$@"); break ;;
    -h|--help)
      sed -n '4,16p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

# Locate the corpus.
if [[ -z "${OPJ_DATA_ROOT:-}" ]]; then
  for candidate in \
    "${REPO_ROOT}/../openjp2k-data/corpus/conformance/openjpeg-data" \
    "${REPO_ROOT}/../openjpeg-data"; do
    if [[ -d "$candidate" ]]; then
      OPJ_DATA_ROOT="$(cd "$candidate" && pwd)"
      break
    fi
  done
fi

if [[ -z "${OPJ_DATA_ROOT:-}" || ! -d "${OPJ_DATA_ROOT}" ]]; then
  cat >&2 <<EOF
error: OPJ_DATA_ROOT not set and no corpus found at the expected sibling
       paths. Clone openjp2k-data alongside this repo, or set OPJ_DATA_ROOT
       to a directory containing input/{conformance,nonregression}/.
EOF
  exit 1
fi

echo "Corpus root: ${OPJ_DATA_ROOT}"
echo "Build dir:   ${BUILD_DIR}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DOPJ_DATA_ROOT="${OPJ_DATA_ROOT}" >/dev/null
cmake --build "${BUILD_DIR}" -j

ctest --test-dir "${BUILD_DIR}" --output-on-failure "${CTEST_ARGS[@]}"
