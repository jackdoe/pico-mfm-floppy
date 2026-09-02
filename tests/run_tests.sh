#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "$(uname -s)" == "Darwin" ]]; then
    export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1}"
else
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1:abort_on_error=1}"
    export LSAN_OPTIONS="${LSAN_OPTIONS:-exitcode=23}"
fi
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

if [[ -n "${TEST_BUILD_DIR:-}" ]]; then
    build_dir="$TEST_BUILD_DIR"
elif [[ -f "$PWD/CTestTestfile.cmake" ]]; then
    build_dir="$PWD"
else
    build_dir="$script_dir/build"
fi

if [[ ! -f "$build_dir/CTestTestfile.cmake" ]]; then
    printf 'CTest build directory not found: %s\n' "$build_dir" >&2
    exit 1
fi

listing="$(ctest --test-dir "$build_dir" -N)"
if [[ ! "$listing" =~ Total\ Tests:\ ([1-9][0-9]*) ]]; then
    printf 'No tests registered in %s\n' "$build_dir" >&2
    exit 1
fi

exec ctest --test-dir "$build_dir" --output-on-failure "$@"
