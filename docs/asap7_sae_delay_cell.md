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
| `nand` | 4      | `VSS EN n IN Y` / `VDD EN Y IN VDD`      | stage 0 with EN; IN drives the Y-side device |
| `pair` | 4      | `D Ga S Gb D` in both bands              | two chain stages sharing a source      |
| `inv`  | 3      | `S G D`                                  | trailing odd stage                     |

Each tile owns its two edge dummy gates with a 46 nm ACTIVE inset, so tiles
abut with the 92 nm S/D-to-S/D spacing (ACTIVE.S.2A) released cells keep.
Stage outputs travel on M1 only: 18 nm flags on the drain-via rows, vertical
bars over source columns or dummy gates, and an 18 nm bar on the n/p seam that
carries every gate contact into the next tile.  Pins are on M1 (`IN`, `EN`,
`OUT`, rails), which the LEF exports as real port rectangles plus M1 OBS.

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
line), `RowStack`/`RowBand` (bands, rails, seam, fin grid), `build_device_band`
(ACTIVE/SDT/LISD per column), `RowSupportSpec`/`build_row_support` (the filler
and tap for this exact stack) and `verification.run_lvs`.

## Gaps this cell exposed in chipforge_asap7

Ordered by how much of the script they would absorb.

1. **RowStack cannot pin the band height** (fixed upstream 2026-09-17).
   Bands were sized from fin count alone, so a 2-fin row was 216 nm and did
   not sit on the 270 nm row, and `RowStack.bands()` derived each row's upper
   rail from `default_height_per_row`, which put a pinned row's pFET rail
   54 nm short and left the tap's well tie inside the band.  `RowStack` now
   takes `band_height`, carries it in its `code`, and sizes rails from the
   drawn band height; this script uses `RowStack(..., band_height=135)`.
2. **No logic-cell builder on a RowStack.**  Everything between "bands" and a
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
