#!/bin/sh
set -e

cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

cd build
exec ../run_tests.sh "$@"
