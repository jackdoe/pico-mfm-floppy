#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

fixture="${SCP_FIXTURE:-$(pwd)/../system-shock-multilingual-floppy-ibm-pc/disk1.scp}"
if [[ ! -s "$fixture" ]]; then
    printf 'Required SCP fixture missing or empty: %s\n' "$fixture" >&2
    exit 1
fi
export SCP_FIXTURE="$fixture"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DSCP_FIXTURE:FILEPATH="$fixture" "$@"
cmake --build build --parallel

TEST_BUILD_DIR="$(pwd)/build" exec ./run_tests.sh
