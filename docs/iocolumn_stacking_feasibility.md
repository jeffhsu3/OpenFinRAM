# Stacking the two IO cores in the 8T mux-group slot: feasibility

`iocolgrp_sram_8t` puts the port A and port B wrappers side by side. Each is
the released `iocolgrp_sram_6t122_v2` core (2376 x 1080 nm) centred in a
3744 x 2376 nm slot with a 684 nm pitch adapter at each end; its devices fill
y = 675..1701, 43 % of the height. The block is 7920 nm long, 52 % of the
64-wordline column group (15264 nm), and the generator adds 146 routing shapes
up to M6 to fan the 594 nm rows into the 270 nm core and to carry each port
across the other. Two cores are 2160 nm tall. Do they fit one above the other?

Checked on 2026-09-21 with the core lifted from
`tech/gds/srambank_32b_boundary_2.gds`, KLayout DRC and flat extraction.

| Question | Result |
| --- | --- |
| Can the cores abut, as the ASU bank tiles them (32 at a 1080 nm step)? | **No.** 27 nets join: both supplies and all 24 controls (`SAE`, `SAPRECHN`, `BLPRECHTN/BN`, `en_out/enb_out`, `wrena/wrenan`, 16 column selects). They are drawn as vertical feed-throughs, which is right for 32 bits of one port and wrong for two ports. |
| With a gap? | 108 nm between the cores and 108 nm at the slot boundary: **no net is shared**, supplies included. That is every nanometre of the slot's spare height (2376 - 2160 = 216). There is no slack. |
| DRC, three slots tiled | Same 9 markers as today plus **`GATE.S.1` x160**. The cores' gates overhang their boundary by 5 nm, so the tips are 98 nm apart, and the deck implements "exact 54 nm pitch" as any gate spacing under 100 nm that is not 34 nm wide. Two nanometres inside the window; not a pitch error. |
| Poly run through the gap with a GCUT across it? | Removes `GATE.S.1`, keeps the cores apart, and adds ~800 markers (`LIG.GATE.A.3/A.4`, `SRAM.LIG.GATE.OV.2` x264 each): the deck counts any gate line touched by a cut as cut, so every LIG contact on it loses its overlap. Worse; leave the gaps open. |
| Routing room | M3 is full inside the core (25 full-height tracks), so adapters stay outside it; M4 and M5 are empty in the core. Each side needs 16 risers instead of 8. The landings are in row order (`blt<0>` at y = 89/191 ... `blt<3>` at 896/1000), so each port stays planar. |

What it would buy, and cost:

* Port B already arrives on M4. With its risers on M5 over port A's M3 risers
  the adapter stays 684 nm: the block is 3744 nm instead of 7920, -53 %, and
  the 64-wordline column group 11.1 um instead of 15.3, -27 %. With all 16
  risers on M3 at the present 72 nm pitch it is about 4.9 um, -38 %.
* Each core then touches both arrays directly, so the M6 crossing of port A
  over port B, and port B's M4 crossing, go away.
* The far rows' risers grow from about 0.6 um to about 2.2 um: some 370 ohm and
  0.35 fF in series with those bitlines at 168 ohm/um (70 ohm at the STA
  table's value), matched within each pair. Not simulated.
* Power has to be brought to both cores separately, and the 160 `GATE.S.1`
  per three slots have to go on the device-DRC baseline.

Not checked: that the adapter routing actually closes in
`generate_asap7_8t_ioprech.py`/`generate_asap7_8t_iocolumn.py`, control-pin
access for OpenROAD, and LVS of the result.

Verdict: feasible, with no margin. It is a rewrite of both generators' routing
around a hard core that fits to the nanometre and needs a DRC artefact waived,
and all of it is discarded when the IO is redrawn at the 8T row pitch. Worth it
only if the area is needed before a parameterized IO exists.

## What was built instead (2026-09-21)

The parameterized route this note recommended has its first cell:
`chipforge_asap7.devices.BitlineMuxSpec`, precharge plus a transmission-gate
column select per bitline pair, drawn on the 8T row instead of the 6T one.

* One port per end of the bitlines. Two ports' leaves cannot share a strip
  (their M3 feed-through tracks sit at the same x), so port A goes on the left
  of the array, mirrored in x, and port B on the right.
* `rows=2` makes the leaf 594 nm, one bitcell row per pair, with the six
  transistors drawn twice in parallel: 756 x 594 nm at 4:1. That is the
  precharge and mux only, so it does not compare with the 7920 nm the two
  reused cores take, which hold the sense amplifier, write driver and latch
  as well.
* `bitline_entry` and `bitline_layer` take the bitlines where `sram_cell_8t`
  delivers them, measured from the bottom of an upright row: port A on M2 at
  348.5 (BLA) and 245.5 (BLAN), port B on M4 at 510 (BLB) and 78 (BLBN). No
  pitch adapter.

`tmp/bitline_io/make_io_mockup.py` puts `array_x2x4_sram_8t` between a port-A
and a port-B group of four and `check_io_mockup.py` extracts it flat: all
sixteen bitlines reach the pass gates of their own select and their own
precharge, no two share a net and none touches a rail. The groups stand one
cell width off the array with a strap per bitline across the gap, as an edge
cell would: abutted outright, the group's implant covers the diffusion sliver
the array overhangs its boundary with, and every BLAN extracts as a tap, which
is the same mechanism as the open tap-cell finding in `macro_verification.md`.
(Since 2026-09-22 the block in the macro does abut, half a fin pitch up the
row so its fins are on the array's grid, against the cap's filler on port A's
side and the tap on port B's rather than a bare bitcell edge; see
`asap7_8t_bitcell.md`, "The block sits on the array's fin grid".)

The floorplan change is done (2026-09-21), with the reused wrappers for now:
a column is `iocol A | edge cap | one array of 2*NUM_WL wordlines | iocol B`.
Both crossovers are gone, the top/bottom split is out of the RTL, the netlist
and the pin map, and each wrapper idles the face turned away from the array.
That answers this note's question a different way: the two cores no longer
share a strip at all, and the IO length per column went from 7920 nm in one
block to 3888 nm at each end. The cost is bitlines of 2*NUM_WL cells instead of
NUM_WL. See `docs/asap7_8t_bitcell.md` (the IO paragraph), and the closing
sections of `docs/macro_verification.md` and `docs/macro_simulation.md` for
what LVS, DRC and the read/write simulation say about it.

Done, 2026-09-22: the sense amplifier, write driver and output latch are drawn
on the same row, `IoColumnSpec` places and routes the four for one column,
and that block is what `iocol_sram_8t_a/b` now contain. The reused 6T cores
are out of the macro.

