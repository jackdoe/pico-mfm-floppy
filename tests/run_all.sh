#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

fixture="${SCP_FIXTURE:-$(pwd)/../system-shock-multilingual-floppy-ibm-pc/disk1.scp}"
if [[ -s "$fixture" ]]; then
    export SCP_FIXTURE="$fixture"
else
    printf 'SCP fixture missing; real-media tests are skipped: %s\n' "$fixture" >&2
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build --parallel

TEST_BUILD_DIR="$(pwd)/build" exec ./run_tests.sh
