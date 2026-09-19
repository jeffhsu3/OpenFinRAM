# ASAP7 SAE / replica delay cell on chipforge_asap7

`scripts/generate_asap7_sae_delay.py` builds the custom sense-amplifier-enable
delay cell (inverter chain, optional NAND2 enable on the first stage) from the
FinFET primitives in `~/iv4/repos/chipforge_asap7`, and gates it with the
public ASAP7 KLayout DRC runset, chipforge_asap7's KLayout LVS deck and Xyce.

```bash
# chipforge_asap7 is installed editable into .venv (uv pip install -e "...[gds]")
.venv/bin/python scripts/generate_asap7_sae_delay.py --drc --lvs        # GDS/SPICE/LEF + gates
.venv/bin/python scripts/generate_asap7_sae_delay.py --sim-only          # Xyce delay + swing
.venv/bin/python scripts/generate_asap7_sae_delay.py --calibrate         # 24-point surrogate fit
.venv/bin/python scripts/glayout_sae_delay.py                            # gLayout Component bridge
.venv/bin/python -m pytest tests/test_sae_delay_cell.py -q
```

## Floorplan

The cell is a row of standard-cell-like tiles on the 7.5-track 270 nm row
(324 nm when a device has 4 fins), copied from the released `NAND2xp33`,
`BUFx2` and `INVx1`:

| tile   | tracks | columns (N band / P band)                | role                                   |
|--------|--------|------------------------------------------|----------------------------------------|
| `nand` | 4      | `VSS EN n IN Y` / `VDD EN Y IN VDD`      | stage 0 with EN; chipforge_asap7 `NandSpec(fingers=1, abut=False)`, flattened in place, IN on the Y-side device |
| `pair` | 4      | `D Ga S Gb D` in both bands              | two chain stages sharing a source      |
| `inv`  | 3      | `S G D`                                  | trailing odd stage                     |

Each tile owns its two edge dummy gates with a 46 nm ACTIVE inset, so tiles
abut with the 92 nm S/D-to-S/D spacing (ACTIVE.S.2A) released cells keep.
Stage outputs travel on M1 only: 18 nm flags on the drain-via rows, vertical
bars over source columns or dummy gates, and an 18 nm bar on the n/p seam that
carries every gate contact into the next tile.  Pins are on M1 (`IN`, `EN`,
`OUT`, rails), which the LEF exports as real port rectangles plus M1 OBS.

## Pitch: standard-cell row or bitcell-matched

`SaeDelayConfig.pitch` (`--pitch`) picks what the cell is matched to:

| pitch | height | width | LEF | placed where |
|---|---|---|---|---|
| `stdcell` (default) | 270 nm (324 with a 4-fin device) | any number of 54 nm tracks | `CLASS CORE`, `SITE asap7sc7p5t` | the synthesized `ctrl_decode` block, replacing `delay_cell.v` |
| `6t` | 270 nm, one 6T bitcell row | multiple of the 108 nm column pitch | `CLASS BLOCK` | handcrafted column periphery beside a 6T array |
| `8t` | 594 nm, one 8T bitcell row | multiple of the 108 nm column pitch | `CLASS BLOCK` | handcrafted column periphery beside this branch's 8T array |

The pitched cells use `RowStack(band_height=135 or 297)`; a chain ending on a
single inverter pads that tile by one dummy track so the width stays on the
column pitch (the default chain becomes 864 x 594 nm as `8t`).  A 6T cell can
hold at most 3 fins per device.  All three are DRC clean on a terminated row
and LVS matched (`sae_delay_6s_2n2p_en_sram_8t`, `sae_delay_4s_3n3p_sram_6t`,
`sae_delay_8s_4n3p_en_sram_8t`).

Where each belongs follows from the released bank, `asap7_sram_0p0/gds/srambank_32b.gds`:

- The 108 x 270 nm bitcell carries its bitlines as 162 nm wide horizontal M2
  bars and its wordline as a vertical 350 nm M3 stripe.  Bitlines run across
  the bank, wordlines run up and down it.
- A `colgrp_x64x4b` is one 4-row (1080 nm) group: a 32-word array, the I/O
  column group (precharge/ymux, write latch, sense amp, `io_tapcell`) in the
  *middle* of the bitline, and another 32-word array.  Thirty-two groups stack
  above and thirty-two below the control band.
- `ctrl_decode_32` is a horizontal band in the middle of the bank (y 34 to
  44 um of 78) holding the predecoder, two mirrored 5-to-32 decoders with the
  `dec_inv` wordline drivers, and `control_sram_2kB_6t122_v3`, a
  standard-cell block (BUFx2, INVx1/x8, DLLx1 latches, NAND2, taps, fillers).
  The `sdel<4:0>` pins land there on M5, so the sense-enable delay chain is
  inside that standard-cell control block.  There is no replica column and no
  per-group delay element: `SAE` leaves the spine on M3 and reaches every
  group's sense amp (`sae` pin on M3) as a vertical broadcast, the same way a
  wordline does.

So in the reference design, and in OpenFinRAM's open flow where `ctrl_decode`
is synthesized and placed by OpenROAD, the delay element lives in the central
standard-cell spine and the `stdcell` pitch is the drop-in shape.  The `6t`
and `8t` shapes are for moving the timing element out to the I/O column
group, e.g. a replica-timed SAE generated per group, where the released I/O
cells tile at the 108 nm column pitch inside a 4-row-tall group.

## Measured (default `SaeDelayConfig`: 6 stages, 2 fins, NAND enable, SRAM-VT)

| check | result |
|---|---|
| size | 810 x 270 nm, 15 CPP |
| Xyce TT, 8 fF load | 80.4 ps IN->OUT, full swing |
| public runset, `filler cell filler tap filler` | clean |
| KLayout LVS vs unit-fin reference | matched |

The same DRC + LVS matrix passes for a plain 4-stage 3-fin chain, a 2-stage
1-fin RVT cell, a 4-fin/2-fin 324 nm cell and an 8-stage LVT chain.

## What chipforge_asap7 supplies

`LAYERS` and the grid constants, `FinFETSpec` (fin arithmetic and the BSIM-CMG
line), `RowStack`/`RowBand` (bands, rails, seam, fin grid), `NandSpec`/
`build_nand` (the entire first stage, including its own row, contacts and
output bar; the script adds only the seam bar that hands its output on),
`build_device_band` (ACTIVE/SDT/LISD per column of the inverter tiles),
`RowSupportSpec`/`build_row_support` (the filler and tap for this exact stack)
and `verification.run_lvs`.

## Gaps this cell exposed in chipforge_asap7

Ordered by how much of the script they would absorb.

1. **RowStack cannot pin the band height** (fixed upstream 2026-09-17).
   Bands were sized from fin count alone, so a 2-fin row was 216 nm and did
   not sit on the 270 nm row, and `RowStack.bands()` derived each row's upper
   rail from `default_height_per_row`, which put a pinned row's pFET rail
   54 nm short and left the tap's well tie inside the band.  `RowStack` now
   takes `band_height`, carries it in its `code`, and sizes rails from the
   drawn band height; this script uses `RowStack(..., band_height=135)`.
2. **No logic-cell builder on a RowStack** (NAND2 added upstream 2026-09-19
   as `NandSpec`, which this cell now uses for its first stage; the `pair` and
   `inv` tiles are still local).  Everything between "bands" and a
   finished cell is hand-drawn: tiles with per-column roles (source, drain,
   uncontacted series node), rail ties (source LISD to the rail plus the rail
   V0), drain vias on `contact_y`, edge dummies with the 46 nm inset, merged
   seam GCUT over dummy tracks.  `build_device_band` also assumes even columns
   are sources (`source_lisd_y`), which is wrong for `D S D` and NAND tiles; a
   column-role list would serve all of them.
3. **No intra-cell M1 vocabulary.**  Flags on via rows (V0.M1.AUX.3 wants
   exactly 18 nm), vertical bars, seam bars, the two gate-contact styles
   (`from_column` with V0 at `GATE_V0_DX`, or a pad on the gate) and the pin
   flag rows one track inside the drain rows.  `inverter.py` has these inline.
4. **Tile abutment helper.**  Island widths in tracks, shared-dummy vs.
   two-dummy breaks (38 vs. 92 nm ACTIVE gaps, only the latter clears
   ACTIVE.S.2A), and `_merge_spans` for gate cuts across tile boundaries.
5. **Netlist dialects.**  `FinFETSpec.netlist` emits HSPICE `nf=`/`m=`; Xyce's
   BSIM-CMG rejects both.  A `dialect` argument, plus the level 72->107 model
   card translation OpenFinRAM keeps re-implementing, belong in the package.
6. **Generic topology object for SPICE + LVS.**  `SenseAmpTransistor` is the
   pattern; a package-level `Device`/`Topology` with `netlist()` and
   `lvs_schematic()` would replace the per-cell renderers.  The unit-fin
   reference also needs a rule for uncontacted series nodes: the extractor
   sees one net per fin there (released NAND2 has no contact either).
7. **DRC as an API.**  Deck discovery, the KLayout invocation and lyrdb
   category parsing live only in `tests/conftest.py`; expose them next to
   `run_lvs` and `drc_verifiable`.
8. **Pin shapes and LEF.**  Cells publish `pin_positions` (points) but not
   the rectangles a LEF port, an OBS block or a gLayout port needs.
9. **gLayout bridge.**  Nothing in chipforge_asap7 speaks gLayout: no
   Component/Port adapter (nm gdspy -> um gdstk), no MappedPDK-style ASAP7
   glayer map with FIN/GCUT/LIG/LISD, no grules for M1/M2/LIG/LISD, no
   `via_stack`/`straight_route`/`L_route` on the 36 nm track grid, no row
   placement.  `scripts/glayout_sae_delay.py` hand-rolls the adapter.
10. **Library hygiene.**  `lib.new_cell` also registers in gdspy's global
    `current_library`, so two libraries built in one process collide on the
    deterministic cell names; a context helper (and a gdstk/um round-trip
    helper for gdstk-based consumers) would remove the boilerplate.
