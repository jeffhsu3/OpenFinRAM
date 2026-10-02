#!/usr/bin/env bash
# Single-port ctrl_decode functional regression (Icarus Verilog), --bitcell 6t.
set -u
cd "$(dirname "$0")/.."

SRC="tests/prim_models.v tests/tb_ctrl_decode_1p.sv tech/verilog_dp/sram_control_1p.v tech/verilog_dp/delay_cell.v tech/verilog_dp/row_decoder.v"
# NUM_WL BANKS MUX
CONFIGS=("2 1 4" "8 1 4" "8 2 4" "16 4 4" "32 1 2")

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fail=0
for cfg in "${CONFIGS[@]}"; do
    set -- $cfg
    if ! iverilog -g2012 -s tb_ctrl_decode_1p \
            -Ptb_ctrl_decode_1p.NUM_WL="$1" \
            -Ptb_ctrl_decode_1p.NUM_BANK="$2" \
            -Ptb_ctrl_decode_1p.COLUMN_MUX="$3" \
            -o "$work/tb_ctrl_decode_1p" $SRC >"$work/compile.log" 2>&1; then
        printf "NUM_WL=%-3s BANKS=%-2s MUX=%-2s: COMPILE ERROR\n" "$1" "$2" "$3"
        cat "$work/compile.log"
        fail=1
        continue
    fi
    result="$(vvp "$work/tb_ctrl_decode_1p" 2>&1)"
    if grep -q '^PASS' <<<"$result"; then
        printf "NUM_WL=%-3s BANKS=%-2s MUX=%-2s: %s\n" "$1" "$2" "$3" "$(grep '^PASS' <<<"$result")"
    else
        printf "NUM_WL=%-3s BANKS=%-2s MUX=%-2s: FAIL\n" "$1" "$2" "$3"
        grep -E '^FAIL|^FATAL' <<<"$result" | head
        fail=1
    fi
done

[ "$fail" -eq 0 ] && echo "ALL 1RW DECODE CHECKS PASSED" || echo "1RW DECODE CHECKS FAILED"
exit "$fail"
