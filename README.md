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
- `--strips-in-controller`: put the wordline driver strips in the controller's
  band, abutting the IO blocks. Supported for one column of tiles (one bank,
  or one shared pair) so far.

Floorplan: each data bit is a column tile, `port-A IO | cap | array | port-B
IO`, with port A's IO at one end of the bitlines and port B's at the other.
The tiles form two abutted stacks with the controller band between them;
wordlines run through each stack by abutment from a pair of driver strips at
its edge. Each IO block carries full-height VDD/VSS M3 straps, tied to the
array's supply bars at every seam, so a stack's supplies join end to end and
only its end tiles' are routed.

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

Verify a result (transistor-level LVS plus the public KLayout DRC runset, or
`--drc-engine gdscheck` for the calibrated device subset):

```
.venv/bin/python scripts/verify_macro.py results/sram_x16x16x1_<timestamp>
```

Known limits: the public DRC is not clean (remaining findings are mostly the
dummy end rows' gate pitch and router-level M1/V1/M4 spacing); the band
controller fails max-fanout on larger macros such as x32x8x2 and x16x32x2;
middle tiles take their supply only through the IO straps, so IR drop on tall
stacks is unmeasured.

### 8T cell libraries

The flow builds on generated native ASAP7 cells (schematics in `tech/spice/`):

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
- `tech/gds/sram_8t_ioprech.gds`: standalone port-A and port-B IO/precharge
  wrappers.

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
- `tests/run_8t_bitcell_check.sh`: deterministic ASAP7 8T bitcell/edge GDS
  regeneration, focused rules, extracted connectivity, and abutment checks.
- `tests/run_8t_ioprech_check.sh`: deterministic port-A/port-B IO/precharge
  wrapper regeneration, pin/layer contracts, and pitch-adapter checks.
- `tests/run_wordline_array_check.sh`: deterministic parameterized 8T/6T
  wordline-row and array hierarchy, including a non-standard x18/mux-2 case.
- `tests/run_8t_iocolumn_check.sh`: deterministic dual-port IO-column routing,
  capped array abutment, internal read-port ties, and a non-standard x18 case.
- `asap7_2rw_physical_check` (`tests/tools/test_2rw_physical.py`): the 2RW
  assembler's tiles, abstracts and abutments without running the router.
- `asap7_2rw_macro_check`: builds and routes a small 2RW macro end to end and
  checks its GDS, LEF, Liberty and SPICE agree (slow; needs OpenROAD).

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
| [Yosys](https://github.com/YosysHQ/yosys) | 0.55+ | RTL synthesis of `ctrl_decode` |
| [OpenROAD](https://github.com/The-OpenROAD-Project/OpenROAD) | v2.0-15391+ (git master) | P&R, CTS, STA; pin `--openroad-path` |
| ASAP7 platform | OpenROAD `platform/asap7` | LEF/lib/GDS/RC; pin `--platform-path` |
| KLayout Python bindings (`pip install klayout`) | 0.29+ | DEF -> merged GDS streaming (`scripts/def_to_gds.py`) |

**Tests (`ctest`):**
| Tool | Tested with | Used by |
|---|---|---|
| Icarus Verilog (`iverilog`/`vvp`) | 13.x | `decode_check` (RTL decode sweep) |
| Yosys | 0.55+ | `equiv_check` (post-synthesis equivalence) |
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
