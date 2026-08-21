# OpenFinRAM open-source SRAM tools

Analysis/characterization tools for the open-source (Yosys + OpenROAD + Xyce)
flow, complementing the C++ generator. All are self-contained Python; no
Cadence dependency.

## `gds2lef.py` — GDS → LEF abstract (no Cadence)

The C++ flow builds a real macro GDS but generates the LEF via Cadence
`strmin` + Abstract Generator. This reads the GDS back with `gdstk` and writes
the LEF (SIZE, PINs from the texttype-251 labels, OBS) directly, so the
open-source path yields a P&R-consumable abstract.

```
uv run python scripts/gds2lef.py results/<run>/sram_x1024x64x1.gds -o out.lef
```

Pins land on their label's metal layer (ASAP7 layer 30=M3, 50=M5, ...);
`--obs M1,M2` sets blanket obstruction. Validated in OpenROAD `read_lef`.

## `characterize_read.py` — real read-path SPICE characterization

Real transient measurement of the read critical path, sweeping column depth to
show the timing is NOT constant (what a FakeRAM `.lib` cannot capture). Modes:

- `--mode access` — WL → BL sense-margin develop.
- `--mode clkq` — full read through the REAL sense amp + io_nand latch +
  tristate buffer to Q (2-cycle: read-1 pre-sets Q high, then read-0 times the
  fall; SAE is replica-timed per depth from the access measurement).
- `--mode setuphold` — A/D/WE input-register setup/hold via bisection on the
  real ASAP7 `DFFHQNx1` flop (capture-failure boundary).

### Simulator note (important)

ASAP7 devices are BSIM-CMG (FinFET). **ngspice (v45) has no BSIM-CMG** — it
runs a planar stand-in (harness exercised, numbers NOT ASAP7). For real numbers
use **Xyce** (`--simulator xyce --real-device --models tech/models/hspice/7nm_TT.pm`).
The harness auto-adapts the card: HSPICE `level 72` → Xyce BSIM-CMG `level 110`,
and emits `NFIN` (BSIM-CMG rejects `W`). `SS`/`FF` `.pm` give the other corners.

```
uv run python scripts/characterize_read.py --mode clkq --simulator xyce \
  --real-device --models tech/models/hspice/7nm_TT.pm
```

## `lvs_instance_check.py` — GDS instance-level LVS audit

Stage-1 layout verification for the routed controller (`ctrl_decode.gds`):
confirms the placed-cell multiset exactly matches the reference gate netlist
(`netlist_for_lvs.v`) and that every distinct placed cell's geometry is
bit-faithful to its ASAP7 library master. Physical-only cells inserted after
synthesis (fillers, tapcells, routing vias) are allowlisted.

This catches dropped/extra/swapped instances and corrupted cell layouts.
It does not trace routing connectivity between instances — that remains
open (full extraction needs Calibre or a complete Magic ASAP7 extraction
tech; neither is available). Combined with `tests/run_equiv_check.sh`
(netlist ≡ RTL) and OpenROAD emitting DEF and GDS from one database, the
residual exposure is limited to routing defects.

```
python3 scripts/lvs_instance_check.py \
    --macro-gds tmp/openroad_<ts>/ctrl_decode.gds --top ctrl_decode \
    --netlist tmp/openroad_<ts>/netlist_for_lvs.v \
    --library-gds tech/gds/asap7sc7p5t_28_R_220121a.gds
```

## Periphery status and remaining signoff

The open-source `ctrl_decode` flow now preserves and checks its 108 physical
delay inverters, uses a dedicated `BUFx4` stage on every WLT/WLB with predicted
array capacitance, performs propagated-clock CTS, and fails on post-route
setup/hold or electrical violations. Its current 2.5 ns SDC is a conservative
TT implementation target using routed global parasitics; it is not yet an
extracted-SPEF, multi-corner frequency characterization.

`characterize_read` still measures the separate datapath/array and deliberately
uses an ideal replica-timed SAE. Correlating that replica delay to the selected
physical `sdel` taps across PVT, producing a characterized macro `.lib`, and
transistor-level LVS remain signoff work.
