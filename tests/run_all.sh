#!/bin/sh
set -e

cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
cmake --build build --parallel > /dev/null 2>&1

cd build

./test_lru
./test_mfm
./test_fat12
./test_f12
./test_robustness
./test_fuzz "$@"
./test_flux_sim

SCP_DIR="../../system-shock-multilingual-floppy-ibm-pc"
for disk in "$SCP_DIR"/disk*.scp; do
    [ -f "$disk" ] && ./test_scp_fat12 "$disk"
done

./test_scp_roundtrip "$@"
./test_pio_sim
./test_pio_emu
./test_write_verify
