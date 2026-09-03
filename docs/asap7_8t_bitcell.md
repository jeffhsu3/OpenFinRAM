# ASAP7 true-dual-port 8T bitcell

OpenFinRAM includes a generated ASAP7 bitcell at `tech/gds/sram_cell_8t.gds`
and its array-edge family at `tech/gds/sram_cell_8t_edges.gds`. Their source is
`scripts/generate_asap7_8t_bitcell.py`; do not hand-edit the GDS.

The matching port-specific IO/precharge wrappers are generated in
`tech/gds/sram_8t_ioprech.gds` by
`scripts/generate_asap7_8t_ioprech.py`. Their reference schematics are in
`tech/spice/sram_8t_ioprech.sp`.

Parameterized wordline rows and mux-height arrays are generated in
`tech/gds/sram_wordline_arrays.gds` by
`scripts/generate_asap7_wordline_arrays.py`. The hierarchy follows the
academic library's `sramcol_xN` -> `array_xNx4` composition, but accepts any
positive wordline count and independently parameterizes the mux-row height.
The same implementation emits 6T variants for non-standard sizes.

## Reference and topology

The logical topology and port convention follow the public OpenRAM
`openram_dp_cell` in VLSIDA's Apache-2.0 Sky130 SRAM cell library, pinned to
commit `fc63b12883b4bf458ee8c756ba64c37063e1ffb9`:

- [OpenRAM dual-port cell directory](https://github.com/VLSIDA/sky130_fd_bd_sram/tree/fc63b12883b4bf458ee8c756ba64c37063e1ffb9/cells/openram_dp_cell)
- [Reference SPICE topology](https://github.com/VLSIDA/sky130_fd_bd_sram/blob/fc63b12883b4bf458ee8c756ba64c37063e1ffb9/cells/openram_dp_cell/sky130_fd_bd_sram__openram_dp_cell.base.spice)

No Sky130 geometry is copied. The generator starts from the published ASAP7
`sram_cell_6t_122` in `tech/gds/srambank_32b_boundary_2.gds` and adds two
2-fin access NMOS devices on the exact ASAP7 27 nm fin and 54 nm gate grids.
The OpenRAM reference's parallel pull-down fingers collapse into the two-fin
ASAP7 pull-down devices already present in the published core.

| OpenRAM | OpenFinRAM | Function |
| --- | --- | --- |
| `WL0` | `WLA` | Port-A wordline |
| `BL0` | `BLA` | Port-A true bitline |
| `BR0` | `BLAN` | Port-A complement bitline |
| `WL1` | `WLB` | Port-B wordline |
| `BL1` | `BLB` | Port-B true bitline |
| `BR1` | `BLBN` | Port-B complement bitline |

The matching schematic is `tech/spice/sram_cell_8t.sp`, which is also the
topology emitted by `SpiceTemplates::get_cell_8t()`.

## Edge-cell family

The edge library follows the roles of OpenRAM's public `openram_dp_cell_dummy`,
`openram_dp_cell_cap_row`, and `openram_dp_cell_cap_col` cells:

| Cell | Electrical contents | Boundary role |
| --- | --- | --- |
| `dummy_cell_8t` | Forced 8T core, both wordlines off | Inactive terminal bitcell |
| `sram_cell_8t_col_cap` | No devices; BLA/BLAN/BLB/BLBN and supply rails | Left/right column cap |
| `sram_cell_8t_row_cap` | No devices; WLA/WLB and ground routes | Top/bottom row cap |
| `sram_cell_8t_corner` | No devices; grounded terminating routes | Array corner cap |

All four cells have the same 0.216 µm × 0.594 µm boundary and the same
FIN/GATE/GCUT/WELL/select frame as the bitcell. The canonical row/column/corner
cells are mirrored at placement time for the opposite sides and other three
corners. The cap-cell SPICE subcircuits are intentionally empty, as in OpenRAM;
their GDS conductors provide physical continuity without adding transistors.
The matching netlists are in `tech/spice/sram_cell_8t_edges.sp`.

## Port-specific IO/precharge wrappers

`ioprech_sram_8t_a` and `ioprech_sram_8t_b` reuse the published ASAP7
`iocolgrp_sram_6t122_v2` differential IO core. Each wrapper retains separate
write-enable, sense-enable, sense-precharge, bitline-precharge, column-select,
and output-enable pins. The two wrappers therefore do not share dynamic
control or sense nodes.

The source IO core uses the compact 6T four-row pitch. Each wrapper adds a
two-sided channel-routed adapter to the 0.594 um 8T row pitch:

| Wrapper | Array bitline layer | Core landing | Function |
| --- | --- | --- | --- |
| `ioprech_sram_8t_a` | M2 | M2 through M3 pitch-shift risers | Port-A read/write IO |
| `ioprech_sram_8t_b` | M4 | M4/V3/M3/V2 to M2 | Port-B IO; write pins retained for standalone verification |

The current dual-port macro interface is 1RW+1R, so the generated composite
SPICE ties the port-B write data low and its write driver inactive. Keeping the
physical B wrapper symmetric avoids an unverified custom read-only derivative
and leaves a path to a future 2RW interface. The wrapper contracts also keep
`SAE` and `SAPRECHN` distinct; the current composite maps both to its existing
per-port `sae_A`/`sae_B` phase signal until the controller exposes a separate
sense-precharge phase.

## Physical interface

- Cell name: `sram_cell_8t`
- SRAM boundary: 0.216 µm × 0.594 µm
- Full drawing bounding box: 0.271 µm × 0.601 µm, including legal overhangs
- Port A: `BLA`/`BLAN` on M2 and `WLA` on M3
- Port B: `BLB`/`BLBN` on M4 and `WLB` on M5
- Supplies: the published core's `vdd!`/`vss!` interface
- Device count: 2 PMOS and 6 NMOS

Port B uses M4/M5 so its routes cross the dense published M2/M3 core without
electrical contact. The boundary is two original 108 nm columns wide, leaving
room for relocated wordline contacts and DRC-clean storage-node straps.

## Rebuild and verification

From the repository root:

```sh
.venv/bin/python scripts/generate_asap7_8t_bitcell.py
.venv/bin/python scripts/generate_asap7_8t_bitcell.py \
  --verify tech/gds/sram_cell_8t.gds
.venv/bin/python scripts/generate_asap7_8t_bitcell.py \
  --verify-edges tech/gds/sram_cell_8t_edges.gds
.venv/bin/python scripts/generate_asap7_8t_ioprech.py
.venv/bin/python scripts/generate_asap7_8t_ioprech.py \
  --verify tech/gds/sram_8t_ioprech.gds
.venv/bin/python scripts/generate_asap7_wordline_arrays.py
.venv/bin/python scripts/generate_asap7_wordline_arrays.py \
  --verify tech/gds/sram_wordline_arrays.gds
ctest --test-dir build -R asap7_8t_bitcell_check --output-on-failure
ctest --test-dir build -R asap7_8t_ioprech_check --output-on-failure
ctest --test-dir build -R asap7_wordline_array_check --output-on-failure
```

The verifier reconstructs the effective gates after GCUT, extracts the eight
device channels and conductor graph, and checks the complete dual-port graph:
two cross-coupled CMOS inverters, two WLA access devices, two WLB access
devices, paired true/complement storage nodes, independent pins, and supplies.
It also checks the exact fin/gate grids and the focused lower-metal rules used
by the added geometry. The test regenerates the GDS and requires a byte-for-byte
match with both checked-in artifacts. For the edge library it additionally
checks the forced dummy state, zero transistor channels in every cap cell,
independent pass-through pins, identical process-frame fingerprints, and
compatible boundary/grid geometry for mirrored abutment.

This verification is not a substitute for foundry signoff. Calibre is not
available in the open-source test environment, and the public ASAP7 Calibre
decks distributed with the PDK are encrypted. Run the official ASAP7 DRC/LVS
deck before treating this cell as tapeout-qualified.

## Current scope

The physical foundation now includes the bitcell, array boundaries,
parameterized active wordline arrays, and both port-specific IO/precharge
wrappers. It does not yet provide the dual-port replica/tap cells or the final
IO-column placement and routing that joins both port wrappers to one array.
The macro layout flow therefore continues to reject dual-port layout
generation rather than silently substituting the 6T array.
