#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

fixture="${SCP_FIXTURE:-$(pwd)/../system-shock-multilingual-floppy-ibm-pc/disk1.scp}"
if [[ ! -s "$fixture" ]]; then
    printf 'Required SCP fixture missing or empty: %s\n' "$fixture" >&2
    exit 1
fi
export SCP_FIXTURE="$fixture"

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
    -DSCP_FIXTURE:FILEPATH="$fixture" \
    -DCMAKE_C_FLAGS="--coverage -O0 -g" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage"
cmake --build "$BUILD_DIR" --parallel

TEST_BUILD_DIR="$(pwd)/$BUILD_DIR" ./run_tests.sh "$@"

mkdir -p "$BUILD_DIR/coverage"
"$GCOVR" --root . --filter '../src/' \
    --txt "$BUILD_DIR/coverage/summary.txt" \
    --html-details "$BUILD_DIR/coverage/report.html" \
    --json-summary "$BUILD_DIR/coverage/summary.json" >/dev/null

"$GCOVR" --root . --filter '../src/' --print-summary 2>&1 | tail -30
echo
echo "HTML report: $(pwd)/$BUILD_DIR/coverage/report.html"
echo "Text report: $(pwd)/$BUILD_DIR/coverage/summary.txt"
