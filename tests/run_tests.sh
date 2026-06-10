#!/bin/sh
set -e

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
./test_floppy_underrun
./test_floppy_timeouts
./test_e2e_corruption
