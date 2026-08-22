#!/bin/bash
# Exhaustive ctrl_decode address-decode correctness regression (Icarus Verilog).
# Sweeps a few (NUM_WL, NUM_BANK, COLUMN_MUX) geometries; each run checks reads
# and writes over the full address space, including inactive banks/halves and
# deselected/no-op behavior. Exit 0 iff every configuration passes.
set -u
cd "$(dirname "$0")/.."

SRC="tests/prim_models.v tests/tb_ctrl_decode.sv tech/verilog_sp/sram_control.v tech/verilog_sp/delay_cell.v"
CONFIGS=("2 1 2" "4 1 2" "8 1 4" "8 2 4" "16 4 4" "32 1 8" "64 2 16" "128 1 4" "128 1 8")

fail=0
for cfg in "${CONFIGS[@]}"; do
    set -- $cfg
    out="$(mktemp)"
    if ! iverilog -g2012 -Ptb_ctrl_decode.NUM_WL=$1 -Ptb_ctrl_decode.NUM_BANK=$2 \
            -Ptb_ctrl_decode.COLUMN_MUX=$3 \
            -o /tmp/tb_ctrl_decode $SRC 2>"$out"; then
        printf "NUM_WL=%-3s BANKS=%-2s MUX=%-2s: COMPILE ERROR\n" "$1" "$2" "$3"; cat "$out"; fail=1; continue
    fi
    res="$(vvp /tmp/tb_ctrl_decode 2>&1)"
    if grep -q "^PASS" <<<"$res"; then
        printf "NUM_WL=%-3s BANKS=%-2s MUX=%-2s: %s\n" "$1" "$2" "$3" "$(grep '^PASS' <<<"$res")"
    else
        printf "NUM_WL=%-3s BANKS=%-2s MUX=%-2s: FAIL\n" "$1" "$2" "$3"; grep -E "^FAIL|^FATAL" <<<"$res" | head; fail=1
    fi
    rm -f "$out"
done

[ "$fail" -eq 0 ] && echo "ALL DECODE CHECKS PASSED" || echo "DECODE CHECKS FAILED"
exit $fail
