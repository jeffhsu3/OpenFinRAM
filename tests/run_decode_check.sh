#!/bin/bash
# Exhaustive ctrl_decode address-decode correctness regression (Icarus Verilog).
# Sweeps a few (NUM_WL, COLUMN_MUX) geometries; each run checks the full address
# space (one-hot wlt/wlb at the decoded row on the selected half, one-hot ysel +
# complement, no wordline when de-selected). Exit 0 iff every config PASSes.
set -u
cd "$(dirname "$0")/.."

SRC="tests/prim_models.v tests/tb_ctrl_decode.sv tech/verilog_sp/sram_control.v tech/verilog_sp/delay_cell.v"
CONFIGS=("8 4" "16 4" "32 8" "64 16" "128 4")

fail=0
for cfg in "${CONFIGS[@]}"; do
    set -- $cfg
    out="$(mktemp)"
    if ! iverilog -g2012 -Ptb_ctrl_decode.NUM_WL=$1 -Ptb_ctrl_decode.COLUMN_MUX=$2 \
            -o /tmp/tb_ctrl_decode $SRC 2>"$out"; then
        printf "NUM_WL=%-3s MUX=%-2s: COMPILE ERROR\n" "$1" "$2"; cat "$out"; fail=1; continue
    fi
    res="$(vvp /tmp/tb_ctrl_decode 2>&1)"
    if grep -q "^PASS" <<<"$res"; then
        printf "NUM_WL=%-3s MUX=%-2s: %s\n" "$1" "$2" "$(grep '^PASS' <<<"$res")"
    else
        printf "NUM_WL=%-3s MUX=%-2s: FAIL\n" "$1" "$2"; grep -E "^FAIL" <<<"$res" | head; fail=1
    fi
    rm -f "$out"
done

[ "$fail" -eq 0 ] && echo "ALL DECODE CHECKS PASSED" || echo "DECODE CHECKS FAILED"
exit $fail
