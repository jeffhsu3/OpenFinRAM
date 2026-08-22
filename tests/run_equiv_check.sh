#!/bin/bash
# Post-synthesis formal equivalence regression (Yosys equiv_*).
# For each (NUM_WL, NUM_BANK, COLUMN_MUX) geometry this script:
#   1. synthesizes ctrl_decode exactly like the production Yosys flow
#      (ASAP7 Liberty read as blackboxes, dfflibmap + ABC technology
#      mapping, plus the same structural signoff assertions), and
#   2. proves the mapped gate netlist formally equivalent to the RTL,
#      using Liberty function models for the standard cells.
# tb_ctrl_decode.sv only exercises the RTL, but the GDS ships the
# *netlist*: a synthesis-introduced decode bug must be caught here.
# Exit 0 iff every configuration is proven equivalent.
set -u
cd "$(dirname "$0")/.."

LIB=tech/lib
RTL=tech/verilog_sp
CONFIGS=("8 1 4" "8 2 4" "16 4 4" "32 1 8" "64 2 16" "128 1 4")

command -v yosys >/dev/null || { echo "yosys not found in PATH"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

clog2() {
    local v=$1 n=0
    while (( v > 1 )); do v=$(( (v + 1) / 2 )); n=$(( n + 1 )); done
    echo "$n"
}

fail=0
for cfg in "${CONFIGS[@]}"; do
    set -- $cfg
    nw=$1 nb=$2 mux=$3
    # Must match get_addr_width / generate_parameter_string:
    # clog2(NUM_WL) + clog2(NUM_BANK) + 1 (top/bottom) + clog2(COLUMN_MUX).
    addr=$(( $(clog2 "$nw") + $(clog2 "$nb") + 1 + $(clog2 "$mux") ))
    label="NUM_WL=$nw BANKS=$nb MUX=$mux"

    cat > "$WORK/synth.ys" <<EOF
# --- synthesis, mirroring src/yosys_tcl_generator.cpp ---
read_liberty -lib $LIB/asap7sc7p5t_AO_RVT_TT.lib
read_liberty -lib $LIB/asap7sc7p5t_INVBUF_RVT_TT.lib
read_liberty -lib $LIB/asap7sc7p5t_OA_RVT_TT.lib
read_liberty -lib -ignore_miss_func -ignore_miss_data_latch $LIB/asap7sc7p5t_SEQ_RVT_TT.lib
read_liberty -lib $LIB/asap7sc7p5t_SIMPLE_RVT_TT.lib
read_verilog -sv $RTL/delay_cell.v
read_verilog -sv $RTL/sram_control.v
chparam -set ADDR_WIDTH $addr -set NUM_WL $nw -set NUM_BANK $nb -set COLUMN_MUX $mux ctrl_decode
hierarchy -check -top ctrl_decode
synth -top ctrl_decode -flatten
dfflibmap -liberty $LIB/asap7sc7p5t_SEQ_RVT_TT.lib
abc -liberty $LIB/asap7sc7p5t_AO_RVT_TT.lib -liberty $LIB/asap7sc7p5t_INVBUF_RVT_TT.lib -liberty $LIB/asap7sc7p5t_OA_RVT_TT.lib -liberty $LIB/asap7sc7p5t_SEQ_RVT_TT.lib -liberty $LIB/asap7sc7p5t_SIMPLE_RVT_TT.lib
opt
opt_clean -purge
# Structural signoff (same gates as the production flow):
select -assert-count $((addr + 7)) t:DFFHQNx1_ASAP7_75t_R
select -assert-min 108 t:INVx1_ASAP7_75t_R
select -assert-min $((2 * nw * nb)) t:BUFx4_ASAP7_75t_R
write_verilog -noattr -noexpr -nohex $WORK/netlist.v
EOF

    cat > "$WORK/equiv.ys" <<EOF
# --- gold: parameterized RTL, physical cells as behavioral models ---
read_verilog -sv tests/prim_models.v
read_verilog -sv $RTL/delay_cell.v
read_verilog -sv $RTL/sram_control.v
chparam -set ADDR_WIDTH $addr -set NUM_WL $nw -set NUM_BANK $nb -set COLUMN_MUX $mux ctrl_decode
prep -top ctrl_decode -flatten
design -stash gold

# --- gate: mapped netlist, standard cells modeled by Liberty functions ---
read_liberty $LIB/asap7sc7p5t_AO_RVT_TT.lib
read_liberty $LIB/asap7sc7p5t_INVBUF_RVT_TT.lib
read_liberty $LIB/asap7sc7p5t_OA_RVT_TT.lib
read_liberty -ignore_miss_func -ignore_miss_data_latch $LIB/asap7sc7p5t_SEQ_RVT_TT.lib
read_liberty $LIB/asap7sc7p5t_SIMPLE_RVT_TT.lib
read_verilog $WORK/netlist.v
hierarchy -top ctrl_decode
prep -top ctrl_decode -flatten
design -stash gate

design -copy-from gold -as gold ctrl_decode
design -copy-from gate -as gate ctrl_decode
equiv_make gold gate equiv
hierarchy -top equiv
opt_clean
equiv_simple -seq 8
equiv_induct -seq 8
equiv_status -assert
EOF

    if ! yosys -q "$WORK/synth.ys" >"$WORK/synth.log" 2>&1; then
        printf "%-28s: SYNTHESIS ERROR\n" "$label"
        tail -5 "$WORK/synth.log"
        fail=1
        continue
    fi
    if ! yosys -q "$WORK/equiv.ys" >"$WORK/equiv.log" 2>&1; then
        printf "%-28s: EQUIVALENCE FAILED\n" "$label"
        grep -E "unproven|ERROR" "$WORK/equiv.log" | head -3
        fail=1
        continue
    fi
    printf "%-28s: EQUIVALENT\n" "$label"
done

[ "$fail" -eq 0 ] && echo "ALL EQUIVALENCE CHECKS PASSED" || echo "EQUIVALENCE CHECKS FAILED"
exit $fail
