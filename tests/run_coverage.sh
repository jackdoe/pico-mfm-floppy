#!/bin/sh
set -e

cd "$(dirname "$0")"

BUILD_DIR=build_cov
GCOVR="${GCOVR:-gcovr}"
if ! command -v "$GCOVR" >/dev/null 2>&1; then
    if [ -x "$HOME/.local/bin/gcovr" ]; then
        GCOVR="$HOME/.local/bin/gcovr"
    else
        echo "gcovr not found. Install with: pip3 install --user gcovr" >&2
        exit 1
    fi
fi

rm -rf "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="--coverage -O0 -g" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage" >/dev/null
cmake --build "$BUILD_DIR" --parallel >/dev/null

cd "$BUILD_DIR"
../run_tests.sh "$@" >/dev/null

mkdir -p coverage
"$GCOVR" --root .. --filter '../../src/' \
    --txt coverage/summary.txt \
    --html-details coverage/report.html \
    --json-summary coverage/summary.json >/dev/null

"$GCOVR" --root .. --filter '../../src/' --print-summary 2>&1 | tail -30
echo
echo "HTML report: $(pwd)/coverage/report.html"
echo "Text report: $(pwd)/coverage/summary.txt"
