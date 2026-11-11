# ASAP7 SRAM Compiler

This is an SRAM compiler designed for the ASAP7 PDK, leveraging the GDSII Tool Kit (GDSTK) to create SRAM layouts.

# Usage

```
git clone --recursive https://github.com/shao-chien-lu/OpenFinRAM.git
cd OpenFinRAM
mkdir build
cd build
cp -r ../tech .
cmake ..
make

./OpenFinRAM --num-wls 2 --num-data-bits 4 --num-banks 1 --single-port
```

## Open-Source Flow (single-port ASAP7)

```
./OpenFinRAM --num-wls 2 --num-data-bits 4 --num-banks 1 --single-port --openroad
```

Runs Yosys synthesis -> OpenROAD P&R/CTS/STA -> KLayout DEF->GDS streaming,
then assembles the macro. Requires `yosys`, `openroad` (or `--openroad-path`),
and KLayout's Python bindings — see External Dependencies.

Notes:
- The commercial flow remains the default when `--openroad` is omitted.
- The build hard-fails on missing timing paths, setup/hold, or slew/capacitance/fanout violations.
- STA uses routed global parasitics at TT only — a conservative implementation target, not a characterized frequency claim. Use `scripts/characterize_read.py` for the array datapath.

## Dual-port 8T flow (2RW, ASAP7)

For measured schematic read timing and Liberty generation from an existing
8T result, see [8T Liberty characterization](docs/liberty_8t.md). The flow
sweeps clock slew and output load separately for both ports and identifies
the timing and power fields that remain estimated or uncharacterized.

Dual-port is the default mode (omit `--single-port`): two independent
read/write ports on an 8T bitcell. Run it from the repository root, since the
binary finds `scripts/`, `tech/` and `.venv/` there and writes `results/`
beside them:

```
./build/OpenFinRAM --num-wls 8 --num-data-bits 16 --num-banks 1 --openroad
```

1. Yosys and OpenROAD synthesize, place, route and time the controller and
   decoders (`tmp/openroad_<timestamp>/`). Without `--openroad` the commercial
   flow builds the controller instead (`tmp/innovus_<timestamp>/`).
2. `scripts/compile_asap7_2rw.py` assembles and routes the macro with OpenROAD
   (`tmp/macro_2rw_<timestamp>/`) and re-extracts every pin connection from
   the streamed GDS before publishing it.
3. The macro lands in `results/sram_x<2*wls>x<bits>x<banks>_<timestamp>/`:
   `.gds`, `.lef`, estimated `.lib`, `.sp` and `.physical.json`.

Sizes: `--num-wls` is the rows one row-select address field covers; each
array has twice that, so the example is `sram_x16x16x1`. Wordlines and bits
must be even and at least 2, banks a power of two, and the column mux four
rows.

Options:
- `--share-port-b`: banks in pairs, mirrored about one two-sided port-B IO
  block (needs an even bank count).
- `--row-predecode-bits 2|3|4`: address bits per predecode group in the
  controller's wordline decode (default 3). 4-bit groups only pay off at 256 or
  more wordlines a bank; on an x64x8x1 they decode slower (327 vs 276 ps, OpenSTA)
  and take more area.

Floorplan: each data bit is a column tile, `port-A IO | cap | array | port-B
IO`, with port A's IO at one end of the bitlines and port B's at the other.
The tiles form two abutted stacks with the controller band between them;
wordlines run through each stack by abutment from a pair of driver strips at
its edge. Each IO block carries full-height VDD/VSS M3 straps, tied to the
array's supply bars at every seam, so a stack's supplies join end to end and
only its end tiles' are routed.

Power grid: vdd and vss are full-height stripes on the top routing layer (M7
by default), alternating, one vdd every 1.28 um with vss halfway between.
Every stripe is a shape of its pin in the LEF, so the chip's horizontal M8
straps can drop vias onto them anywhere along the macro; the router joins each
block's supply pins to them. They are minimum width, as ASAP7's V6 must be
exactly as wide as the M7 it lands on. The assembler's `--power-pitch` sets
the spacing, and `--power-pitch 0` gives the old single pin per supply.

The assembler can be run on its own against an existing controller GDS, which
skips synthesis; it then writes only the `.gds` and `.physical.json`:

```
.venv/bin/python scripts/compile_asap7_2rw.py --wordlines 8 --bits 16 --banks 1 \
  --controller tmp/openroad_<timestamp>/ctrl_decode.gds \
  --work tmp/macro_2rw_manual --output results/manual/sram_x16x16x1.gds
```

It starts at a 0.3 um pin margin and doubles it up to `--max-margin` if
routing fails. Python needs `gdstk`, KLayout's bindings and `chipforge_asap7`
(the driver strips); `.venv/bin/python` is used when present.

Verify a result with transistor-level LVS and gdscheck DRC (the default):

```
.venv/bin/python scripts/verify_macro.py results/sram_x16x16x1_<timestamp>
```

Known limits: the public DRC is not clean (remaining findings are mostly the
dummy end rows' gate pitch and router-level M1/V1/M4 spacing); the band
controller fails max-fanout on larger macros such as x32x8x2 and x16x32x2;
middle tiles take their supply only through the IO straps, so IR drop on tall
stacks is unmeasured.

## Single-port 6T flow (generated, `--bitcell 6t`)

The two-port machinery with port A alone, on the released ASAP7 6T cell:

```
./build/OpenFinRAM --num-wls 8 --num-data-bits 16 --num-banks 1 --bitcell 6t --openroad
```

- Column tile: `edge filler | cap | 2*NUM_WL 6T bitcells | dummy | tap | IO`,
  all released `srambank_32b` cells except the IO block, which is chipforge_asap7's
  `StaggeredIoColumnSpec`. The 6T row (270 nm) is shorter than any IO leaf the
  dense row style can draw (297 nm), so the even rows' leaves stand in one column
  and the odd rows' in a second, the bitlines lifted to M4 at the block's edge.
  Written by `scripts/generate_asap7_6t_iocolumn.py` (`tech/gds/sram_6t_iocolumn.gds`,
  `tech/spice/sram_6t_iocolumn.sp`).
- Floorplan as the two-port macro: two stacks of abutted tiles with the controller
  band between them, one driver strip on each side, released dummy rows at each
  stack's ends. Controller: `tech/verilog_dp/sram_control_1p.v`.
- Pins: `clk rst_n ce_n we_n oe_n A[] D[] Q[]` (port A of the two-port
  machinery inside; the GDS/LEF, the deck's top subckt and the `.lib` agree).
  D[] and Q[] are on the right edge, at the IO end of the bitlines, each bit's
  pair level with its tile; the address and controls are on the left, level
  with the controller's inputs. (The two-port macro has port A's pins on the
  left and port B's on the right, at their IO blocks' ends.)
- `--single-port` with `--openroad` builds this macro; without it (the commercial
  flow) it is still the legacy srambank flow.
- Larger banks: `--segment-bits N` divides each stack's wordlines into segments of
  N data bits, a strip pair between segments; `--num-rows-per-mux 8|16` for deeper
  column muxes; `--num-banks` side by side.  Each strip joins its slices'
  predecode inputs on M4 rails, and the controller's array-wide outputs (sel_lo,
  sae, blprechn, ...) get drivers sized for their counted gate load.
- Select buffers (`--select-buffers auto|on|off`, default `auto`): every slice of
  every strip hangs on the controller's four sel_lo lines, so their load grows
  with the macro's bits, and one output cannot drive it past ~175 fF (0.15 ns
  slew; about 16 kbit).  A buffered strip takes each line once, on an M4 rail
  over a row of standard cells under its slices (0.378 um a strip), and a
  BUFx8 a line re-drives every group of at most 16 slices (BUFx4 for groups
  under 8).  `auto` adds them when the counted sel_lo load is past that limit;
  a strip needs 4 slices (16 wordlines a stack) to hold them.  x64x8x1
  `--segment-bits 2 --select-buffers on` matches strict LVS with and without
  `--strips-in-controller`, at the unbuffered macro's DRC count, and passes the
  whole-macro Xyce program (clk->Q ~240 ps, no wordline glitch); with them the
  controller passes its slew check at 32 and 64 kbit (x256x32x1 and x256x64x1,
  `--segment-bits 8`); at 128 kbit blprechn is the next line past the limit
  (precharge buffers, below).
- Precharge buffers (`--precharge-buffers auto|on|off`, default `auto`): the
  controller's blprechn hangs on every column's precharge gates (6 fins a
  column: 323 fF at 64 bits and 8:1).  One more buffer at the IO end of every
  strip's select-buffer row takes the bank's line (`PCH`) and re-drives it
  (`PCHL`) for the IO blocks of the wordline segment that strip serves, on the
  segment's own net (`blprechn_A_<lo|hi>_s<k>`), so the controller sees a
  buffer input a strip (28 fF for that macro).  A segment between two strips
  has both their buffers on its net; the one farthest from the band has one,
  which sizes the cell: BUFx8 to 80 fF a segment, BUFx12f to 115, BUFx24 to
  175, and past that fewer bits a segment (`--segment-bits`).  They stand in
  the select buffers' row, so they turn those on and add no height; the row
  has room when the strip's smallest group of slices has 9 (10 and 11 for the
  larger cells), which rules out stacks of 68 wordlines or fewer than 36.
  `auto` adds them when the counted blprechn load is past the 175 fF limit
  and warns when it cannot.  x64x8x1 `--segment-bits 2 --precharge-buffers on`
  matches strict LVS with and without `--strips-in-controller`, at the same
  size and with no DRC marker of the buffers' own, and passes the whole-macro Xyce
  program (clk->Q 234-240 ps): the copies release 15 ps after the controller's
  line and 64-80 ps before a wordline rises (in the first two cycles 80 ps,
  against 70 at FF and 90 at SS; probed at the 5.40 um band the macro had
  before the height rule below, which moved clk->WL by 1 ps).  With them the
  controller passes its slew check at 128 kbit (x256x64x1 at 8:1 and
  x256x32x1 at 16:1, `--segment-bits 4`), and x256x64x1 at 8:1 assembles and
  routes: 16622 nets connected, no routing violation, 30.5 x 200.8 um.  Its
  detailed route takes about 3 h on the assembler's two threads (2 h 41 min
  and 3 h 09 min in two runs on a loaded machine), past the one-hour
  `--route-timeout` the flow leaves at its default, so that macro was
  assembled by `scripts/compile_asap7_2rw.py` directly with a longer one; LVS,
  DRC and simulation are not run at that size.
- Controller band height (standard floorplan): the taller of two rules, in whole
  row pairs.  Pins: port A's inputs (address, ce_n, we_n, oe_n) stand in order
  on the band's left edge 0.192 um apart, which pin placement seats once the
  band is 1.28 + 0.192 um an input tall (it was a 0.4 um allowance an input).
  Cells: the synthesized logic at 50 %, as before, plus the BUFx24 output
  drivers (the lines over 60 fF) at 60 %.  The synthesized area has no output
  drivers, and at 128 kbit there are 78 of those, 34 um² beside 40 um² of
  logic.  That makes the band 3.78 um at 32 kbit (x256x32x1,
  `--segment-bits 8`; it was 6.48 um) and 4.86 um at 128 kbit (x256x64x1 at
  8:1 and x256x32x1 at 16:1; 6.48 and 7.02 um), where 4.32 um ends one select
  line over its slew limit; x64x8x1 and x256x2x1 are 3.78 um (5.40 and 6.48).
  Macros whose logic sets the height (x4x2x1, x16x16x1, x4x2x2, x4x8x2) keep
  theirs.  A run that fails place-and-route is retried with a lower target,
  or two more rows where the pins set the height.
- Outer strips (`--outer-strips`, standard floorplan): a strip at each stack's
  far end as well, so the segment farthest from the band is driven from both
  ends like the others, and every slice is sized for half a segment.  A
  segment twice as long then has the wordline of today's slowest one: 64
  cells driven from both ends by 32-cell slices reach half swing in 18.3 ps
  at the middle, and 32 cells driven from one end in 18.3 ps at the far end
  (Xyce at TT with the M3 at 233 ohm/um; 20.0 and 20.0 ps at SS), where a
  32-cell segment between two strips takes 14.4 ps.  The strips about halve:
  x256x64x1 at 8:1 with `--segment-bits 8 --outer-strips` has 16 for the 30
  of `--segment-bits 4` and is 30.5 x 174.1 um against 200.8 (13 % less); it
  assembles and routes like that one (10522 nets connected, no routing
  violation, 2 h 46 min of detailed route).  x256x32x1 with whole stacks has
  4 for the 6 of `--segment-bits 8` and places at 30.9 x 47.5 um against
  51.8.  Both controllers pass place-and-route.  x64x8x1 with them matches
  strict LVS, with whole stacks and with
  `--segment-bits 2 --precharge-buffers on`, at the DRC rules of the macros
  without (59 and 63 markers), and with whole stacks passes the whole-macro
  Xyce program (clk->Q 237-243 ps).  A small macro gains nothing: x64x8x1
  grows from 17.19 to 18.30 um.  Not with `--strips-in-controller`.
- The `.lib` timing and energy come from `scripts/timing_model_6t.py`, fit to
  whole-macro simulations (`tech/timing/sram_6t_timing.json`).
- Verified: strict LVS match (`scripts/verify_macro.py`) on x4x2x1, x16x16x1,
  x64x8x1 (segmented), x256x2x1 and multi-bank x4x2x2/x4x8x2; the whole-macro Xyce
  program (`scripts/simulate_macro.py`) passes on x4x2x1, x16x16x1 and x256x2x1
  (clk->Q ~250 ps; x256x2x1 221-227 ps with its 3.78 um band).  On x256x2x1
  the sense enable follows the wordline by only 19-39 ps: 30-48 ps at the
  6.48 um band, where place-and-route put no repeater on the wordline enable,
  and 67-69 ps in the October build.  The deck has no bitline wire RC, with
  which 256 cells take 26 ps to split 25 mV, so that margin is not proven.
  x16x16x1 is 4.94 x 27.72 um against 6.18 x 49.37 um for the 8T x16x16x1.
- `--strips-in-controller` works with one bank, segmented or not (see
  [Controller band](#controller-band)); `--share-port-b`
  does not apply.
- Limits so far: the timing model is fit to small macros and to the old BUFx2 controller outputs.
  The public DRC count is the released 6T cells' own findings, the end rows'
  gate pitch, standard cells, and a few router M1 markers.

## Controller band

8T OpenROAD builds always place and route the controller around the wordline
driver strips. The controller band fills the space between the two tile stacks,
with the strips aligned to the arrays and abutting the IO blocks. This includes
multiple bank columns and multiple shared-B pairs. The old
`--strips-in-controller` flag is accepted for compatibility and hidden from help;
8T builds do not need a placement option. Generated 6T retains its existing
single-bank opt-in behavior.

```
# 8T two-port: two pairs sharing port B, integrated by default
./build/OpenFinRAM --num-wls 2 --num-data-bits 2 --num-banks 4 --share-port-b --openroad

# 6T single-port: one bank, optionally segmented
./build/OpenFinRAM --num-wls 32 --num-data-bits 8 --num-banks 1 --bitcell 6t \
  --num-rows-per-mux 4 --segment-bits 2 --strips-in-controller --openroad
```

The flow has three steps:

1. `compile_asap7_2rw.py --plan-band` writes the strip placement (`plan.txt`),
   the strips' abstract (`strips.lef`) and the floorplan script (`band.tcl`).
2. OpenROAD builds the controller on that band die, keeping the strips fixed,
   and reports the die it used (`die.txt`).
3. The assembler (`--band`) abuts the two tile stacks on the band's edges.

Each bank column repeats the strips and dummy-row keep-outs at a stride that is
a multiple of 1.728 um. The band height accounts for cell area, opposing strips
and side-pin capacity. The physical manifest records `wordline_driver_placement`,
`bank_columns`, `bank_column_stride_um` and `controller_band_size_um`.

The tiles and strips sit half a nanometre off the whole-nm grid in their
masters, with or without the band (see the DRM grid note below). For the 6T
tile, the shift puts its M4 pins off the LEF's 1 nm grid. Each pin therefore
also gets a V4 and a short M5 landing on an M5 track, so the router can reach
it.

**DRM grid.** The macro's frame is the ASAP7 DRM's absolute grid, so a chip
must place the macro's origin on a multiple of 1.728 um (the least common
multiple of the gate pitch and the M1-M7 track pitches):
- **Rules:**
  - fins centred 13.5 nm past a multiple of 27 (FIN.S.1);
  - gates 27 nm past a multiple of 54 (GATE.S.1);
  - M4/M5 tracks 12 nm past a multiple of 48, and M6/M7 tracks 16 nm past a
    multiple of 64 (AUX.2).
- **How the assembler meets them:**
  - The controller is routed on those tracks.
  - Each block's origin snaps onto the grids. The bitcell's fins sit half a
    fin pitch off the standard cells', so the tiles and strips also take the
    half-nanometre shift.
  - The margins and channels grow by up to 0.43 um to make room.
  - The supply stripes and side pins sit on the tracks.
  - The IO blocks route on M4, so their lines go on the tracks too. Where a
    tile lands is known only once the floorplan is laid out, and a stack's
    tiles land on different tracks: a middle 8T tile is 2376 nm, half a
    track pitch past a multiple of 48. Each tile is therefore rebuilt for
    its place, its blocks drawn for that grid (chipforge's `m4_grid`), and
    the masters are named for it (`dp_column_g0x19p5`).
- **Array edge:** the array's edge cells all carry the SRAMDRC marker, the 6T
  tile's blank and edge fillers included.
  - The 8T strip pairs climb to port B's M5 wordlines on via stacks, one
    row of them per pair, on M4 too. After the floorplan each pair's row
    moves onto the track it lands nearest, the M3 stubs under it grown to
    reach (`retrack_strip_vias`). The stubs reach 17-62 nm from the pair's
    edge, so 4 of the 96 half-nanometre placements have no track in reach;
    outside the band the assembler moves the pair to the next fin-grid
    place, and in the band it warns and leaves the row as drawn. The band's
    controller is routed before the row moves, so the band plan obstructs
    the row's whole reach on M3 and M4.

Results (`scripts/verify_macro.py`, `scripts/simulate_macro.py`):

- 8T x4x2x2 with `--share-port-b`: 20 % smaller than without the band, LVS match.
  x8x2x2 also matches LVS.
- 6T x64x8x1 with `--segment-bits 2`: 10.12 x 20.40 um (206 um²) against
  10.12 x 21.00 um (212 um²) without the band, 3 % smaller (it was 11 % before
  the standard floorplan's band took its height from the measured pin
  spacing). LVS matches; the macro is too large to simulate at transistor
  level.
- 6T x16x8x1: 4.81 x 16.91 um, LVS match. The Xyce program passes, with clk->WL
  128 ps, clk->SAE 223 ps and clk->Q about 260 ps.
- DRC findings fall in the same categories as without the band: 52 against 63
  on x64x8x1.

### 8T cell libraries

The flow builds on generated native ASAP7 cells (schematics in `tech/spice/`).
The GDS libraries are build output: the `tech_gds` target (part of the default
build when `.venv/bin/python`, or `python3`, can import `gdstk` and
`chipforge_asap7`) runs `scripts/tech_gds.py`, which writes them into
`tech/gds/` in about 15 s, and `OpenFinRAM` writes any that are missing before
a 2RW or 6T run. Git tracks only their geometry digests
(`tech/gds/digests.json`); after an intended generator change, run
`scripts/tech_gds.py --update`.

- `tech/gds/sram_cell_8t.gds`: the 8T bitcell and its mirrored-slot variants,
  built from the published ASAP7 6T core with ASAP7-native port-B geometry and
  the public OpenRAM dual-port topology. The matching dummy, row-cap,
  column-cap and corner family is in `tech/gds/sram_cell_8t_edges.gds`. See
  [docs/asap7_8t_bitcell.md](docs/asap7_8t_bitcell.md) for the pin map,
  provenance, rebuild commands and verification scope.
- `tech/gds/sram_wordline_arrays.gds`: parameterized `sramcol_xN` and
  `array_xNxM` hierarchy for 8T and 6T. The tracked ladder has 2/32/64/128
  wordlines; `scripts/generate_asap7_wordline_arrays.py` accepts any positive
  count, and the macro compiler generates the tapped arrays it needs on demand.
- `tech/gds/sram_8t_iocolumn.gds`: the port-A, port-B and two-sided port-B IO
  blocks (`iocol_sram_8t_a/b/b2`) and the `colgrp_x{N}x4_sram_8t` column
  groups that join them to an array, written with their SPICE by
  `scripts/generate_asap7_8t_iocolumn.py`.
- `tech/gds/sram_8t_wl_slices.gds` and `tech/gds/sram_6t_iocolumn.gds`: the
  wordline driver slices and the 6T IO block (both chipforge_asap7 devices).

## Tests

Device DRC uses a pinned [gdscheck](https://github.com/aesc-silicon/gdscheck)
dependency and a local ASAP7 width/spacing deck. Install with
`bash scripts/install_gdscheck.sh`, then run
`bash tests/run_8t_device_drc_check.sh`. See [gdscheck DRC](docs/gdscheck.md)
for coverage, calibration, reference cross-checks, and macro usage. The full
public ASAP7 runset and LVS continue to use KLayout.

Run via CTest (requires `iverilog` and `yosys` in `PATH`):

```
cmake -S . -B build && ctest --test-dir build --output-on-failure
```

- `tests/run_decode_check.sh`: exhaustive RTL decode-correctness sweep of
  `ctrl_decode` (Icarus Verilog) over read/write, deselect and no-op cases.
- `tests/run_equiv_check.sh`: post-synthesis formal equivalence (Yosys
  `equiv_*`) between the `ctrl_decode` RTL and the mapped ASAP7 netlist,
  including the production structural signoff assertions. This catches
  synthesis-introduced decode bugs that RTL simulation cannot see.
- `asap7_tech_gds_check`: the built `tech/gds` libraries against their
  recorded geometry digests.
- `tests/run_8t_bitcell_check.sh`: ASAP7 8T bitcell/edge GDS regeneration
  against its digests, focused rules, extracted connectivity, and abutment checks.
- `tests/run_wordline_array_check.sh`: deterministic parameterized 8T/6T
  wordline-row and array hierarchy, including a non-standard x18/mux-2 case.
- `tests/run_8t_iocolumn_check.sh`: deterministic dual-port IO-column routing,
  capped array abutment, internal read-port ties, and a non-standard x18 case.
- `asap7_2rw_physical_check` (`tests/tools/test_2rw_physical.py`): the 2RW
  assembler's tiles, abstracts and abutments without running the router.
- `asap7_2rw_macro_check`: builds and routes a small 2RW macro end to end and
  checks its GDS, LEF, Liberty and SPICE agree (slow; needs OpenROAD).
- `python3 tests/check_macro_pins.py macro.gds macro.lef`: checks that a
  macro's GDS pin labels match its LEF pins and sit on metal of their own
  layer, including routed M4/M5 control pins and the M7 power stripes. The
  macro check runs it; it is a geometry check, not connectivity LVS.

## Commercial Flow (Cadence / Synopsys, default)

```
./OpenFinRAM --num-wls 2 --num-data-bits 4 --num-banks 1 --single-port
# uses dc_shell + innovus and optional calibre/siliconsmart as before;
# LEF export itself is native and no longer needs Cadence Abstract
```

## External Dependencies

**Build (all flows):** CMake >= 3.22, C++17 compiler, Python 3 (for the
helper scripts in `scripts/`).

**Commercial flow (default):** Synopsys `dc_shell`, Cadence `Innovus`;
optional Calibre (LVS/DRC) and SiliconSmart (.lib characterization).
LEF export is native GDSTK and no longer needs Cadence Abstract or `tcsh`.

**Open-source flow (`--openroad`):**
| Tool | Tested with | Purpose |
|---|---|---|
| [Yosys](https://github.com/YosysHQ/yosys) | 0.69 (SiliconCompiler's pin) | RTL synthesis of `ctrl_decode` |
| [OpenROAD](https://github.com/The-OpenROAD-Project/OpenROAD) | `755f5778c4` (SiliconCompiler's pin) | P&R, CTS, STA; pin `--openroad-path` |
| ASAP7 platform | OpenROAD `platform/asap7` | LEF/lib/GDS/RC; pin `--platform-path` |
| KLayout Python bindings (`pip install klayout`) | 0.29+ | DEF -> merged GDS streaming (`scripts/def_to_gds.py`) |

**Tests (`ctest`):**
| Tool | Tested with | Used by |
|---|---|---|
| Icarus Verilog (`iverilog`/`vvp`) | 13.x | `decode_check` (RTL decode sweep) |
| Yosys | 0.69 | `equiv_check` (post-synthesis equivalence) |
| googletest | submodule | `unit_tests` (TCL generator goldens) |

## TODO / Future Work

- **Array decoupling capacitors**: industrial SRAM macros embed MOSCAP or MOM
  decap in dummy rows/columns and periphery strips to suppress di/dt droop.
  ASAP7 provides no cap device (no MIM layer exists at 7nm; the GF180/Sky130
  style metal-insulator-metal option disappeared after 28nm), so this requires
  characterizing a gate-only NMOS MOSCAP dummy cell or an interdigitated
  M3/M4/M5 MOM cell (DRC + LVS + extraction) before it can be tiled by the
  compiler. The current dummy-fill cells provide the structure but no
  intentional capacitance.

## References

- **ASAP7 PDK**: https://github.com/The-OpenROAD-Project/asap7
- **GDSTK**: https://github.com/heitzmann/gdstk
- **BSG FakeRAM**: https://github.com/bespoke-silicon-group/bsg_fakeram
- **PLOG**: https://github.com/SergiusTheBest/plog

## License

This project is licensed under the BSD 3-Clause License - see the [LICENSE](LICENSE) file for details.
