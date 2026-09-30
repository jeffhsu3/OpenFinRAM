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
- The commercial flow remains the default when `--openroad` is omitted; dual-port falls back to it.
- The build hard-fails on missing timing paths, setup/hold, or slew/capacitance/fanout violations.
- STA uses routed global parasitics at TT only — a conservative implementation target, not a characterized frequency claim. Use `scripts/characterize_read.py` for the array datapath.

## Dual-port 8T bitcell (ASAP7)

The dual-port physical foundation is available as the generated native ASAP7
hard cell `tech/gds/sram_cell_8t.gds`, the matching dummy/row-cap/column-cap/
corner family in `tech/gds/sram_cell_8t_edges.gds`, and independent port-A and
port-B IO/precharge wrappers in `tech/gds/sram_8t_ioprech.gds`. Schematics live
under `tech/spice/`. It uses the public OpenRAM dual-port topology; the layout
itself is built from the published ASAP7 6T core and ASAP7-native geometry. See
[docs/asap7_8t_bitcell.md](docs/asap7_8t_bitcell.md) for the pin map,
provenance, rebuild command, and verification scope.

`tech/gds/sram_wordline_arrays.gds` adds academic-style parameterized
`sramcol_xN` and `array_xNxM` hierarchy for both 8T and 6T. The tracked ladder
contains 2/32/64/128 wordlines; `scripts/generate_asap7_wordline_arrays.py`
accepts any positive wordline count for non-standard macro sizes. Wordline
count and column-mux height are separate parameters, and the default four-row
8T arrays are checked directly against both IO-wrapper pin pitches.

`tech/gds/sram_8t_iocolumn.gds` joins both physical IO wrappers to the same
pair of capped 8T half-arrays. The A and B cores are side by side rather than
overlaid: port B crosses the A core on M4, while port A crosses the B core on
M5 through dedicated via-stack gaps. The generated `colgrp_x{2N}x4_sram_8t`
ladder follows the same 2/32/64/128 half-array sizes and accepts non-standard N
when supplied a matching parameterized-array GDS.

The layout compiler still fails closed in dual-port mode until the matching
replica/tap cells and final macro-level placement/power integration exist; the
active-array and routed dual-port IO-column hierarchy itself is now available.
Dual-port preflight loads this library and validates the requested wordline
variant and fixed four-row mux contract before reporting those later blockers.

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
