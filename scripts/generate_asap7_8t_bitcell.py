#!/usr/bin/env python3
"""Generate and verify OpenFinRAM's ASAP7 8T bitcell and edge-cell family.

The logical topology follows OpenRAM's public dual-port cell:

  https://github.com/VLSIDA/sky130_fd_bd_sram/tree/
      fc63b12883b4bf458ee8c756ba64c37063e1ffb9/cells/openram_dp_cell

Only the topology and port convention are used.  The geometry is native ASAP7:
the published ``sram_cell_6t_122`` core is copied from OpenFinRAM's tracked
ASAP7 SRAM GDS and two 2-fin access NMOS devices are added on the same 27 nm
fin / 54 nm gate grid.  Port A retains the core's M2 bitlines and M3 wordline;
port B is routed on M4/M5 to avoid disturbing the compact 6T-core routing.

Pin mapping from the OpenRAM reference is:

  WL0  BL0  BR0  WL1  BL1  BR1
   |    |    |    |    |    |
  WLA  BLA  BLAN  WLB  BLB  BLBN

The second generated GDS contains the forced-state dummy plus electrically
empty row, column, and corner cap cells used at array boundaries.  IO-column,
replica, and compiler integration remain separate work; the macro layout flow
must continue to fail closed in dual-port mode until those cells exist.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import sys
from collections import defaultdict
from pathlib import Path

import gdstk


CELL_NAME = "sram_cell_8t"
SOURCE_CELL = "sram_cell_6t_122"
EDGE_CELL_NAMES = (
    "dummy_cell_8t",
    "sram_cell_8t_col_cap",
    "sram_cell_8t_row_cap",
    "sram_cell_8t_corner",
)
TAP_CELL_NAME = "tapcell_sram_8t"
FIXED_GDS_TIMESTAMP = dt.datetime(2020, 1, 1, 0, 0, 0)

# ASAP7 GDS drawing layers.
WELL = 1
FIN = 2
GATE = 7
GCUT = 10
ACTIVE = 11
NSELECT = 12
PSELECT = 13
LIG = 16
LISD = 17
V0 = 18
M1 = 19
M2 = 20
V1 = 21
V2 = 25
M3 = 30
V3 = 35
M4 = 40
V4 = 45
M5 = 50
SDT = 88
SRAMDRC = 99
BOUNDARY = 100
SRAMVT = 110

PIN_TEXTTYPE = 251
MARKER_LAYERS = {SRAMDRC, BOUNDARY, SRAMVT}

# Device sizing, as fin counts.  All eight devices share one gate pitch, so L
# is common to every channel and a fin-count ratio *is* a width ratio is a
# drive-strength ratio.  These are the single source of truth: verify_topology
# asserts the extracted GDS against them, so changing a number here without
# changing the layout fails, and vice versa.
#
# The values are inherited from the published sram_cell_6t_122 -- build_cell
# only *adds* two access devices to that core -- rather than chosen for
# dual-port operation.  See CELL_RATIO below for why that matters.
PULLDOWN_NFIN = 2
ACCESS_NFIN = 2
PULLUP_NFIN = 1

# Cell ratio (pull-down / access) sets read stability.  During a read the
# access device lifts the storage-low node while the pull-down holds it down;
# the ratio decides how far it rises before the latch is at risk.
CELL_RATIO = PULLDOWN_NFIN / ACCESS_NFIN

# ...but this is a *dual-port* cell and both ports land on the same storage
# nodes.  With WLA and WLB both asserted -- which the macro contract permits,
# both for same-address read/read and for a row half-selected on both ports --
# two access devices load one pull-down, so the effective ratio halves.
CONCURRENT_CELL_RATIO = PULLDOWN_NFIN / (2 * ACCESS_NFIN)

# Pull-up ratio (pull-up / access) sets write-ability: the access device has to
# overpower the pull-up holding the storage-high node.  Lower is easier to
# write, so unlike CELL_RATIO a small number here is not a concern.
PULLUP_RATIO = PULLUP_NFIN / ACCESS_NFIN

# Textbook guidance for a 6T single-port read is 1.5-2.0.  We are at 1.0, and
# half that with both ports selected.  This is a warning rather than an assert
# because failing here would only stop the cell being generated at all; the
# real gate is measured read SNM.  Promote it to an assertion once
# tests/run_8t_stability_check.sh has numbers to back a bound.
RECOMMENDED_MIN_CELL_RATIO = 1.5

# Coordinates are micrometers.  Port B grows the cell north/south while the
# east/west pitch remains the published 108 nm 6T pitch.  As in the official
# bitcell, FIN, select, ACTIVE, and horizontal rails deliberately overhang the
# BOUNDARY so mirrored neighbors merge at the placement seam.
MARKER = (0.000, -0.162, 0.108, 0.432)

# Well/substrate tie geometry.  The tie diffusions sit between the bitcell's
# two gate columns on its own fin grid, so no gate can cross them and the tap
# stays device-free while keeping the array's FIN and poly pitch.
TAP_ACTIVE_X = (0.046, 0.062)
TAP_CONTACT_X = (0.042, 0.066)
TAP_VIA_X = (0.045, 0.063)
TAP_VIA_HEIGHT = 0.018
TAP_BAND_INSET = 0.0135
GATE_A = (0.017, -0.181, 0.037, 0.439)
GATE_B = (0.071, -0.169, 0.091, 0.451)
WLA_M3 = (0.045, -0.162, 0.063, 0.432)
WLB_M5 = (0.048, -0.162, 0.072, 0.432)


def rect(cell: gdstk.Cell, box: tuple[float, float, float, float], layer: int) -> None:
    """Add an axis-aligned drawing rectangle."""
    x0, y0, x1, y1 = box
    cell.add(gdstk.rectangle((x0, y0), (x1, y1), layer=layer, datatype=0))


def bbox(poly: gdstk.Polygon) -> tuple[float, float, float, float]:
    (x0, y0), (x1, y1) = poly.bounding_box()
    return tuple(round(float(v), 7) for v in (x0, y0, x1, y1))


def is_box(poly: gdstk.Polygon, expected: tuple[float, float, float, float]) -> bool:
    return bbox(poly) == tuple(round(v, 7) for v in expected)


def clone_base(source: gdstk.Cell, target: gdstk.Cell) -> None:
    """Copy the official core, replacing only shapes that must be extended."""
    gate_a = (0.017, -0.019, 0.037, 0.277)
    gate_b = (0.071, -0.007, 0.091, 0.289)
    # Relocate the original WLA gate contacts so that an M1 strap can reach
    # each storage node without crossing the wordline.  These are routing
    # shapes only; all six original device channels remain untouched.
    relocated = {
        (LIG, (0.022, -0.008, 0.054, 0.008)): "lig_a",
        (V0, (0.0305, -0.009, 0.0485, 0.009)): "v0_a",
        (M1, (0.0305, -0.019, 0.0485, 0.019)): "m1_a",
        (M2, (0.0215, -0.009, 0.072, 0.009)): "m2_a",
        (V1, (0.0305, -0.009, 0.0485, 0.009)): "v1_a",
        (V2, (0.045, -0.009, 0.063, 0.009)): "v2_a",
        (M3, (0.045, -0.040, 0.063, 0.310)): "m3_wla",
        (LIG, (0.054, 0.262, 0.086, 0.278)): "lig_b",
        (V0, (0.0595, 0.261, 0.0775, 0.279)): "v0_b",
        (M1, (0.0595, 0.251, 0.0775, 0.289)): "m1_b",
        (M2, (0.036, 0.261, 0.0865, 0.279)): "m2_b",
        (V1, (0.0595, 0.261, 0.0775, 0.279)): "v1_b",
        (V2, (0.045, 0.261, 0.063, 0.279)): "v2_b",
    }
    full_width_m2 = {
        (-0.027, 0.126, 0.135, 0.144),
        (-0.026, 0.0745, 0.136, 0.0925),
        (-0.027, 0.225, 0.135, 0.243),
        (-0.026, 0.1775, 0.136, 0.1955),
        (-0.027, 0.027, 0.135, 0.045),
    }

    replaced = {"gate_a": False, "gate_b": False}
    relocated_found = set()
    for poly in source.polygons:
        if poly.datatype != 0:
            target.add(
                gdstk.Polygon(poly.points.copy(), layer=poly.layer,
                              datatype=poly.datatype)
            )
            continue
        if poly.layer in MARKER_LAYERS:
            continue
        if poly.layer == FIN:
            _x0, y0, _x1, y1 = bbox(poly)
            rect(target, (-0.027, y0, 0.135, y1), FIN)
            continue
        if poly.layer == GATE and is_box(poly, gate_a):
            # Retain the official gate's 19/7 nm boundary overhang while
            # extending it through the taller cell.
            rect(target, GATE_A, GATE)
            replaced["gate_a"] = True
            continue
        if poly.layer == GATE and is_box(poly, gate_b):
            # Retain the complementary 7/19 nm official overhang.
            rect(target, GATE_B, GATE)
            replaced["gate_b"] = True
            continue
        key = (poly.layer, bbox(poly))
        if key in relocated:
            relocated_found.add(relocated[key])
            continue
        if poly.layer == M2 and bbox(poly) in full_width_m2:
            x0, y0, x1, y1 = bbox(poly)
            rect(target, (x0, y0, x1, y1), M2)
            continue
        target.add(
            gdstk.Polygon(poly.points.copy(), layer=poly.layer,
                          datatype=poly.datatype)
        )

    missing = [name for name, found in replaced.items() if not found]
    missing.extend(sorted(set(relocated.values()) - relocated_found))
    if missing:
        raise RuntimeError(f"source 6T geometry changed; missing shapes: {', '.join(missing)}")

    rename = {"WL": "WLA", "BL": "BLA", "BLN": "BLAN"}
    for label in source.labels:
        text = rename.get(label.text, label.text)
        # Place the wordline label on the exact center of its M3 trunk so
        # mirrored array instances retain the 108 nm address pitch.
        x = 0.054 if text == "WLA" else float(label.origin[0])
        target.add(
            gdstk.Label(
                text,
                (x, float(label.origin[1])),
                anchor=label.anchor,
                rotation=label.rotation,
                magnification=label.magnification,
                x_reflection=label.x_reflection,
                layer=label.layer,
                texttype=label.texttype,
            )
        )


def add_via_stack_to_m4(
    cell: gdstk.Cell,
    cx: float,
    cy: float,
    *,
    v0_cy: float | None = None,
) -> None:
    """Connect an LISD terminal to an M4 horizontal track at (cx, cy)."""
    landing_y = cy if v0_cy is None else v0_cy
    rect(
        cell,
        (cx - 0.009, landing_y - 0.009, cx + 0.009, landing_y + 0.009),
        V0,
    )
    rect(cell, (cx - 0.009, cy - 0.014, cx + 0.009, cy + 0.014), M1)
    rect(cell, (cx - 0.009, cy - 0.009, cx + 0.009, cy + 0.009), V1)
    rect(cell, (cx - 0.014, cy - 0.009, cx + 0.014, cy + 0.009), M2)
    rect(cell, (cx - 0.009, cy - 0.009, cx + 0.009, cy + 0.009), V2)
    rect(cell, (cx - 0.009, cy - 0.017, cx + 0.009, cy + 0.017), M3)
    rect(cell, (cx - 0.009, cy - 0.012, cx + 0.009, cy + 0.012), V3)


def add_gate_to_m5(
    cell: gdstk.Cell, gate_via_x: float, bridge_x: float, y: float
) -> None:
    """Connect an LIG wordline contact to the vertical WLB route on M5."""
    rect(cell, (gate_via_x - 0.009, y - 0.009,
                gate_via_x + 0.009, y + 0.009), V0)
    rect(cell, (gate_via_x - 0.009, y - 0.019,
                gate_via_x + 0.009, y + 0.019), M1)
    rect(cell, (gate_via_x - 0.009, y - 0.009,
                gate_via_x + 0.009, y + 0.009), V1)
    rect(cell, (min(gate_via_x, bridge_x) - 0.014, y - 0.009,
                max(gate_via_x, bridge_x) + 0.014, y + 0.009), M2)
    rect(cell, (bridge_x - 0.009, y - 0.009,
                bridge_x + 0.009, y + 0.009), V2)
    rect(cell, (bridge_x - 0.009, y - 0.017,
                bridge_x + 0.009, y + 0.017), M3)
    rect(cell, (bridge_x - 0.009, y - 0.012,
                bridge_x + 0.009, y + 0.012), V3)
    # At least 2,000 nm^2 of M4, on the 48 nm routing grid.
    rect(cell, (0.003, y - 0.012, 0.087, y + 0.012), M4)
    rect(cell, (0.048, y - 0.012, 0.072, y + 0.012), V4)


def add_wla_routes(cell: gdstk.Cell) -> None:
    """Add the two WLA gate contacts and their common M3 route."""
    # Bottom WLA contact: moved left of the Q strap.  The 0.5 nm south shift
    # gives the required 15 nm corner spacing to the LISD above.
    rect(cell, (0.000, -0.0115, 0.037, 0.0045), LIG)
    rect(cell, (0.006, -0.0125, 0.024, 0.0055), V0)
    rect(cell, (0.006, -0.017, 0.024, 0.011), M1)
    rect(cell, (0.006, -0.012, 0.024, 0.006), V1)
    rect(cell, (0.004, -0.012, 0.072, 0.006), M2)
    rect(cell, (0.045, -0.012, 0.063, 0.006), V2)

    # Keep the top WLA landing away from the east/west placement seam.  The
    # upper storage strap jogs around this M1 landing below.
    # The 1 nm north shift gives 15 nm corner spacing to the LISD below.
    rect(cell, (0.071, 0.2655, 0.096, 0.2815), LIG)
    rect(cell, (0.072, 0.2645, 0.090, 0.2825), V0)
    rect(cell, (0.072, 0.2585, 0.090, 0.2865), M1)
    rect(cell, (0.072, 0.2635, 0.090, 0.2815), V1)
    rect(cell, (0.040, 0.2635, 0.092, 0.2815), M2)
    rect(cell, (0.045, 0.2635, 0.063, 0.2815), V2)
    # Both wordline ports are full-height trunks.  Array verification relies
    # on these shapes continuing through every north/south row abutment.
    rect(cell, WLA_M3, M3)


def add_storage_straps(cell: gdstk.Cell) -> None:
    """Strap the added access drains to the published core's Q and QB."""

    # Center every storage V0 and its M1 landing on the 54 nm midpoint between
    # the two GATE columns.  The canonical x=42..66 nm LISD terminals enclose
    # each 18 nm V0 by 3 nm on both sides without approaching GATE.
    # The upper core landing is 1 nm below its LISD center to retain the
    # required diagonal V0 spacing to the adjacent WLA landing.
    for cy in (-0.0675, 0.052, 0.2285, 0.3375):
        rect(cell, (0.045, cy - 0.009, 0.063, cy + 0.009), V0)
    rect(cell, (0.045, -0.0815, 0.063, 0.066), M1)
    # The upper M1 is one polygon with 18 nm-wide landing/bypass sections.  It
    # jogs left only while passing the top WLA landing, preserving the prior
    # 18 nm clearance without displacing either storage V0 toward GATE.
    cell.add(
        gdstk.Polygon(
            (
                (0.045, 0.2155), (0.063, 0.2155),
                (0.063, 0.2435), (0.054, 0.2435),
                (0.054, 0.3235), (0.063, 0.3235),
                (0.063, 0.3515), (0.045, 0.3515),
                (0.045, 0.3415), (0.036, 0.3415),
                (0.036, 0.2255), (0.045, 0.2255),
            ),
            layer=M1,
            datatype=0,
        )
    )


def add_wlb_routes(cell: gdstk.Cell) -> None:
    """Add the two WLB gate contacts and their common M5 route."""
    rect(cell, (0.017, -0.140, 0.054, -0.124), LIG)
    rect(cell, (0.054, 0.388, 0.086, 0.404), LIG)
    add_gate_to_m5(
        cell,
        gate_via_x=0.0395,
        bridge_x=0.018,
        y=-0.132,
    )
    add_gate_to_m5(
        cell,
        gate_via_x=0.0685,
        bridge_x=0.018,
        y=0.396,
    )
    rect(cell, WLB_M5, M5)


def add_storage_straps_and_wla(cell: gdstk.Cell) -> None:
    add_wla_routes(cell)
    add_storage_straps(cell)


def add_port_b(cell: gdstk.Cell) -> None:
    """Add the two access devices and their independent B-side routing."""
    # Two-fin access devices.  Their storage-side LISD terminals extend the
    # core's Q/QB local-interconnect rails; the outside terminals are BLBN/BLB.
    rect(cell, (-0.008, -0.0945, 0.062, -0.0405), ACTIVE)
    rect(cell, (0.046, 0.3105, 0.116, 0.3645), ACTIVE)
    rect(cell, (-0.027, -0.108, 0.135, -0.027), NSELECT)
    rect(cell, (-0.027, 0.297, 0.135, 0.378), NSELECT)

    # Separate WLB's gate segments from WLA and the cross-coupled core.
    rect(cell, (0.000, -0.0355, 0.054, -0.0185), GCUT)
    rect(cell, (0.054, 0.2885, 0.108, 0.3055), GCUT)

    # Storage and outside source/drain terminals plus diffusion/contact
    # marker shapes.  Storage LISD and SDT retain the canonical 24 nm width;
    # their centered V0 landings connect to the core through the M1 straps.
    rect(cell, (0.042, -0.0945, 0.066, -0.0405), LISD)
    rect(cell, (0.042, 0.3105, 0.066, 0.3645), LISD)
    rect(cell, (-0.012, -0.0885, 0.012, -0.0405), LISD)
    rect(cell, (0.096, 0.3105, 0.120, 0.3585), LISD)
    for box in (
        (0.042, -0.0945, 0.066, -0.0405),
        (-0.012, -0.0885, 0.012, -0.0405),
        (0.042, 0.3105, 0.066, 0.3645),
        (0.096, 0.3105, 0.120, 0.3585),
    ):
        rect(cell, box, SDT)

    # M4 bitlines lie on legal 48 nm tracks and stay isolated where the M5
    # wordline crosses them (there is no V4 at either crossing).
    # Move only the BLBN V0 4.5 nm north so its south edge no longer
    # protrudes beyond LISD; the remainder of the stack stays on its track.
    add_via_stack_to_m4(cell, 0.000, -0.084, v0_cy=-0.0795)
    rect(cell, (-0.027, -0.096, 0.135, -0.072), M4)
    add_via_stack_to_m4(cell, 0.108, 0.348)
    rect(cell, (-0.027, 0.336, 0.135, 0.360), M4)

    # WLB gate stacks occupy the neighboring M4 tracks, leaving exactly the
    # required 24 nm M4 spacing to the port-B bitlines.
    add_wlb_routes(cell)

    cell.add(gdstk.Label("BLBN", (-0.018, -0.084), layer=M4,
                           texttype=PIN_TEXTTYPE))
    cell.add(gdstk.Label("BLB", (0.130, 0.348), layer=M4,
                           texttype=PIN_TEXTTYPE))
    cell.add(gdstk.Label("WLB", (0.060, -0.150), layer=M5,
                           texttype=PIN_TEXTTYPE))
    # The added NMOS bodies use the same p-substrate connection as the core.
    cell.add(gdstk.Label("vss!", (0.048, -0.0675), layer=3,
                           texttype=PIN_TEXTTYPE))
    cell.add(gdstk.Label("vss!", (0.048, 0.3375), layer=3,
                           texttype=PIN_TEXTTYPE))


def build_cell(source_gds: Path) -> gdstk.Library:
    source_lib = gdstk.read_gds(str(source_gds))
    source = next((c for c in source_lib.cells if c.name == SOURCE_CELL), None)
    if source is None:
        raise RuntimeError(f"{SOURCE_CELL!r} not found in {source_gds}")

    lib = gdstk.Library("openfinram_asap7_8t", unit=source_lib.unit,
                        precision=source_lib.precision)
    cell = lib.new_cell(CELL_NAME)
    clone_base(source, cell)
    add_storage_straps_and_wla(cell)
    add_port_b(cell)

    for layer in sorted(MARKER_LAYERS):
        rect(cell, MARKER, layer)

    # Maintain exact 27 nm fin pitch throughout the expanded SRAM marker.
    existing_centers = {round((bbox(p)[1] + bbox(p)[3]) / 2, 6)
                        for p in cell.polygons if p.layer == FIN and p.datatype == 0}
    for index in range(-6, 17):
        center = round(index * 0.027, 6)
        if center not in existing_centers:
            rect(cell, (-0.027, center - 0.0035, 0.135, center + 0.0035), FIN)

    return lib


def clone_label(
    label: gdstk.Label, text: str | None = None
) -> gdstk.Label:
    return gdstk.Label(
        text if text is not None else label.text,
        tuple(label.origin),
        anchor=label.anchor,
        rotation=label.rotation,
        magnification=label.magnification,
        x_reflection=label.x_reflection,
        layer=label.layer,
        texttype=label.texttype,
    )


def clone_polygons(
    source: gdstk.Cell,
    target: gdstk.Cell,
    predicate,
) -> None:
    for poly in source.polygons:
        if predicate(poly):
            target.add(
                gdstk.Polygon(
                    poly.points.copy(), layer=poly.layer, datatype=poly.datatype
                )
            )


def add_process_frame(source: gdstk.Cell, target: gdstk.Cell) -> None:
    """Copy device-free process continuity used by all three cap cells."""
    frame_layers = {
        WELL,
        FIN,
        GATE,
        GCUT,
        NSELECT,
        PSELECT,
        SRAMDRC,
        BOUNDARY,
        SRAMVT,
    }
    clone_polygons(source, target, lambda poly: poly.layer in frame_layers)


def is_full_m2_rail(poly: gdstk.Polygon) -> bool:
    if poly.layer != M2 or poly.datatype != 0:
        return False
    x0, _y0, x1, _y1 = bbox(poly)
    return x1 - x0 >= 0.160


def is_bitline_m4_rail(poly: gdstk.Polygon) -> bool:
    if poly.layer != M4 or poly.datatype != 0:
        return False
    return bbox(poly) in {
        (-0.027, -0.096, 0.135, -0.072),
        (-0.027, 0.336, 0.135, 0.360),
    }


def add_full_rails(source: gdstk.Cell, target: gdstk.Cell) -> None:
    clone_polygons(
        source,
        target,
        lambda poly: is_full_m2_rail(poly) or is_bitline_m4_rail(poly),
    )


def add_ground_rails(source: gdstk.Cell, target: gdstk.Cell) -> None:
    ground_y = {
        (0.027, 0.045),
        (0.225, 0.243),
    }
    clone_polygons(
        source,
        target,
        lambda poly: is_full_m2_rail(poly)
        and (bbox(poly)[1], bbox(poly)[3]) in ground_y,
    )


def copy_labels(
    source: gdstk.Cell,
    target: gdstk.Cell,
    names: set[str],
    rename: dict[str, str] | None = None,
) -> None:
    rename = rename or {}
    for label in source.labels:
        if label.text in names:
            target.add(clone_label(label, rename.get(label.text, label.text)))


def build_edge_library(
    bitcell: gdstk.Cell, unit: float = 1e-6, precision: float = 2.5e-10
) -> gdstk.Library:
    """Build the OpenRAM-style dummy, row/column caps, and corner cap."""
    lib = gdstk.Library(
        "openfinram_asap7_8t_edges", unit=unit, precision=precision
    )

    # Forced-state dummy matching SpiceTemplates::get_dummy_cell_8t():
    # BLA/BLB storage side is VSS; BLAN/BLBN storage side is VDD; both
    # wordlines are held inactive at VSS.
    dummy = lib.new_cell("dummy_cell_8t")
    clone_polygons(bitcell, dummy, lambda _poly: True)
    copy_labels(
        bitcell,
        dummy,
        {label.text for label in bitcell.labels},
        {"WLA": "vss!", "WLB": "vss!"},
    )
    dummy.add(
        gdstk.Label("vdd!", (0.051, 0.052), layer=LISD,
                     texttype=PIN_TEXTTYPE),
        gdstk.Label("vss!", (0.045, 0.2295), layer=LISD,
                     texttype=PIN_TEXTTYPE),
    )

    # Column cap: horizontal bitline and supply continuity, but no devices or
    # wordline contacts.  This mirrors OpenRAM's electrically empty cap_col.
    col_cap = lib.new_cell("sram_cell_8t_col_cap")
    add_process_frame(bitcell, col_cap)
    add_full_rails(bitcell, col_cap)
    copy_labels(
        bitcell,
        col_cap,
        {"BLA", "BLAN", "BLB", "BLBN", "vdd!", "vss!"},
    )

    # Row cap: vertical wordline and ground continuity.  Gates are retained as
    # mask/load continuity, but ACTIVE is deliberately absent, so this cell is
    # electrically empty just like OpenRAM's cap_row.
    row_cap = lib.new_cell("sram_cell_8t_row_cap")
    add_process_frame(bitcell, row_cap)
    add_ground_rails(bitcell, row_cap)
    add_wla_routes(row_cap)
    add_wlb_routes(row_cap)
    row_cap.add(
        gdstk.Label("WLA", (0.054, -0.028), layer=M3,
                     texttype=PIN_TEXTTYPE),
        gdstk.Label("WLB", (0.060, -0.150), layer=M5,
                     texttype=PIN_TEXTTYPE),
    )
    copy_labels(bitcell, row_cap, {"vss!"})

    # Corner cap carries both sets of terminating routes, all grounded, but no
    # ACTIVE.  Mirroring this canonical cell covers all four array corners.
    corner = lib.new_cell("sram_cell_8t_corner")
    add_process_frame(bitcell, corner)
    add_full_rails(bitcell, corner)
    add_wla_routes(corner)
    add_wlb_routes(corner)
    copy_labels(
        bitcell,
        corner,
        {"BLA", "BLAN", "BLB", "BLBN", "vdd!", "vss!"},
        {"BLA": "vss!", "BLAN": "vss!", "BLB": "vss!", "BLBN": "vss!"},
    )
    corner.add(
        gdstk.Label("vss!", (0.054, -0.028), layer=M3,
                     texttype=PIN_TEXTTYPE),
        gdstk.Label("vss!", (0.060, -0.150), layer=M5,
                     texttype=PIN_TEXTTYPE),
    )
    return lib


def supply_rails(bitcell: gdstk.Cell) -> dict[str, list[tuple[float, float]]]:
    """Map vdd!/vss! to the y-extent of the full-width M2 rails they pin."""
    rails = [bbox(poly) for poly in bitcell.polygons if is_full_m2_rail(poly)]
    nets: dict[str, list[tuple[float, float]]] = {}
    for label in bitcell.labels:
        if label.layer != M2 or label.text not in ("vdd!", "vss!"):
            continue
        x, y = float(label.origin[0]), float(label.origin[1])
        hits = [r for r in rails if r[0] <= x <= r[2] and r[1] <= y <= r[3]]
        _assert(len(hits) == 1, f"{label.text} pin is not on exactly one M2 rail")
        nets.setdefault(label.text, []).append((hits[0][1], hits[0][3]))
    _assert(set(nets) == {"vdd!", "vss!"}, "bitcell is missing an M2 supply pin")
    return nets


def tie_bands(bitcell: gdstk.Cell) -> list[tuple[float, float, str]]:
    """Regions the tap must tie, as (y0, y1, net), ordered bottom to top.

    Tie polarity is inverted with respect to the bitcell: the n-well takes an
    n+ tie to VDD, and every substrate band the bitcell implants n+ for its
    NMOS takes a p+ tie to VSS.
    """
    well = bbox(next(p for p in bitcell.polygons if p.layer == WELL))
    bands = [(bbox(p)[1], bbox(p)[3], "vss!")
             for p in bitcell.polygons if p.layer == NSELECT]
    bands.append((well[1], well[3], "vdd!"))
    return sorted(bands)


def group_tie_bands(
    bands: list[tuple[float, float, str]]
) -> list[tuple[str, list[tuple[float, float, str]]]]:
    """Collapse adjacent same-net bands so each takes one shared M1 strap."""
    groups: list[tuple[str, list[tuple[float, float, str]]]] = []
    for band in bands:
        if groups and groups[-1][0] == band[2]:
            groups[-1][1].append(band)
        else:
            groups.append((band[2], [band]))
    return groups


def build_tap_library(
    bitcell: gdstk.Cell, unit: float = 1e-6, precision: float = 2.5e-10
) -> gdstk.Library:
    """Build the array well/substrate tap, following tapcell_sram_6t122."""
    lib = gdstk.Library(
        "openfinram_asap7_8t_tap", unit=unit, precision=precision
    )
    cell = lib.new_cell(TAP_CELL_NAME)
    x0, y0, x1, y1 = bbox(
        next(p for p in bitcell.polygons if p.layer == BOUNDARY)
    )

    # Fin and poly grids run through the tap column unchanged; the dummy gates
    # are cut at the row-abutment line exactly as tapcell_sram_6t122 does.
    clone_polygons(bitcell, cell, lambda poly: poly.layer in (FIN, GATE))
    rect(cell, (x0, y0 - 0.0085, x1, y0 + 0.0085), GCUT)
    for layer in (SRAMDRC, BOUNDARY):
        rect(cell, (x0, y0, x1, y1), layer)

    # Bitlines and supplies pass straight through, so the tap can be inserted
    # anywhere in a row without breaking a bitline.
    add_full_rails(bitcell, cell)

    bands = tie_bands(bitcell)
    rails = supply_rails(bitcell)
    well_y0, well_y1, _ = next(b for b in bands if b[2] == "vdd!")
    rect(cell, (x0, well_y0, x1, well_y1), WELL)
    rect(cell, (x0, well_y0, x1, well_y1), NSELECT)
    for band_y0, band_y1, net in bands:
        if net == "vss!":
            rect(cell, (x0, band_y0, x1, band_y1), PSELECT)

    ax0, ax1 = TAP_ACTIVE_X
    cx0, cx1 = TAP_CONTACT_X
    vx0, vx1 = TAP_VIA_X
    for band_y0, band_y1, _net in bands:
        ty0, ty1 = band_y0 + TAP_BAND_INSET, band_y1 - TAP_BAND_INSET
        rect(cell, (ax0, ty0, ax1, ty1), ACTIVE)
        for layer in (LISD, SDT):
            rect(cell, (cx0, ty0, cx1, ty1), layer)
        centre = (ty0 + ty1) / 2
        rect(cell, (vx0, centre - TAP_VIA_HEIGHT / 2,
                    vx1, centre + TAP_VIA_HEIGHT / 2), V0)

    # One M1 strap per run of same-net bands, each landing on a supply rail of
    # its own net.  The runs are separated by a whole implant band, so the
    # straps cannot merge.
    for net, group in group_tie_bands(bands):
        bottom = group[0][0] + TAP_BAND_INSET
        top = group[-1][1] - TAP_BAND_INSET
        rect(cell, (vx0, bottom, vx1, top), M1)
        landed = [r for r in rails[net] if bottom <= r[0] and r[1] <= top]
        _assert(bool(landed), f"tap {net} strap reaches no {net} rail")
        for rail_y0, rail_y1 in landed:
            rect(cell, (vx0, rail_y0, vx1, rail_y1), V1)

    copy_labels(bitcell, cell, {"vdd!", "vss!"})
    return lib


def polygon_fingerprint(cell: gdstk.Cell) -> str:
    """Stable geometry/label digest, independent of GDS timestamps."""
    records: list[tuple] = []
    for poly in cell.polygons:
        points = tuple((round(float(x), 6), round(float(y), 6))
                       for x, y in poly.points)
        records.append(("P", poly.layer, poly.datatype, points))
    for label in cell.labels:
        records.append(("L", label.text, round(float(label.origin[0]), 6),
                        round(float(label.origin[1]), 6), label.layer,
                        label.texttype))
    digest = hashlib.sha256()
    for record in sorted(records, key=repr):
        digest.update(repr(record).encode("utf-8"))
    return digest.hexdigest()


def _assert(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _layer_polygons(cell: gdstk.Cell, layer: int) -> list[gdstk.Polygon]:
    return [p for p in cell.polygons if p.layer == layer and p.datatype == 0]


def _overlap(a: gdstk.Polygon, b: gdstk.Polygon) -> bool:
    """Return true for positive-area overlap (not mere edge contact)."""
    result = gdstk.boolean(a, b, "and", precision=1e-6)
    return bool(result) and sum(abs(p.area()) for p in result) > 1e-12


class _Dsu:
    def __init__(self, size: int):
        self.parent = list(range(size))

    def find(self, item: int) -> int:
        while self.parent[item] != item:
            self.parent[item] = self.parent[self.parent[item]]
            item = self.parent[item]
        return item

    def union(self, left: int, right: int) -> None:
        left, right = self.find(left), self.find(right)
        if left != right:
            self.parent[right] = left


def extract_connectivity(
    cell: gdstk.Cell, expected_channels: int = 8
) -> tuple[list[dict], dict[str, set[int]]]:
    """Extract enough device/net connectivity to audit the intended 8T graph."""
    gates = gdstk.boolean(_layer_polygons(cell, GATE),
                          _layer_polygons(cell, GCUT), "not", precision=1e-6)
    active = _layer_polygons(cell, ACTIVE)
    channels = gdstk.boolean(active, gates, "and", precision=1e-6)
    diffusions = gdstk.boolean(active, gates, "not", precision=1e-6)
    _assert(
        len(channels) == expected_channels,
        f"expected {expected_channels} transistor channels, found {len(channels)}",
    )

    # Node objects use symbolic layer names where processing changes a GDS
    # layer (effective gate segments and gate-split diffusion).
    objects: list[tuple[str | int, gdstk.Polygon]] = []
    objects.extend(("gate", p) for p in gates)
    objects.extend(("diff", p) for p in diffusions)
    for layer in (LIG, LISD, V0, M1, V1, M2, V2, M3, V3, M4, V4, M5):
        objects.extend((layer, p) for p in _layer_polygons(cell, layer))

    same_layer = {"gate", "diff", LIG, LISD, M1, M2, M3, M4, M5}
    cross_layers = {
        frozenset(("gate", LIG)),
        frozenset(("diff", LISD)),
        frozenset((LIG, LISD)),
        frozenset((LIG, V0)),
        frozenset((LISD, V0)),
        frozenset((V0, M1)),
        frozenset((M1, V1)),
        frozenset((V1, M2)),
        frozenset((M2, V2)),
        frozenset((V2, M3)),
        frozenset((M3, V3)),
        frozenset((V3, M4)),
        frozenset((M4, V4)),
        frozenset((V4, M5)),
    }
    dsu = _Dsu(len(objects))
    for i, (left_layer, left_poly) in enumerate(objects):
        for j in range(i):
            right_layer, right_poly = objects[j]
            connect = (left_layer == right_layer and left_layer in same_layer)
            connect = connect or frozenset((left_layer, right_layer)) in cross_layers
            if connect and _overlap(left_poly, right_poly):
                dsu.union(i, j)

    labels: dict[str, set[int]] = defaultdict(set)
    pin_layers = {LIG, LISD, M1, M2, M3, M4, M5}
    for label in cell.labels:
        if label.texttype != PIN_TEXTTYPE or label.layer not in pin_layers:
            continue
        hits = []
        for index, (layer, poly) in enumerate(objects):
            if layer == label.layer and poly.contain(label.origin):
                hits.append(dsu.find(index))
        _assert(bool(hits), f"pin {label.text} is not on drawing geometry")
        labels[label.text].update(hits)

    # Map every channel to its gate component and the diffusion components on
    # each side.  All ASAP7 devices here have vertical gates.
    devices: list[dict] = []
    wells = _layer_polygons(cell, WELL)
    fins = _layer_polygons(cell, FIN)
    for channel in channels:
        (x0, y0), (x1, y1) = channel.bounding_box()
        gate_hits = [dsu.find(i) for i, (layer, poly) in enumerate(objects)
                     if layer == "gate" and _overlap(channel, poly)]
        _assert(len(set(gate_hits)) == 1, "channel does not have one gate net")

        terminals: list[int] = []
        for side_x in (x0 - 0.0005, x1 + 0.0005):
            probe = gdstk.rectangle((side_x - 0.00025, y0 + 0.0005),
                                    (side_x + 0.00025, y1 - 0.0005))
            hits = [dsu.find(i) for i, (layer, poly) in enumerate(objects)
                    if layer == "diff" and _overlap(probe, poly)]
            _assert(len(set(hits)) == 1, "channel does not have two diffusion terminals")
            terminals.append(hits[0])

        is_pmos = any(_overlap(channel, well) for well in wells)
        # Fin count is the device width in this technology, so it is the only
        # thing binding the netlist's nfin= to the drawn geometry.
        nfin = sum(1 for fin in fins if _overlap(channel, fin))
        _assert(nfin > 0, "channel is not crossed by any fin")
        devices.append({"gate": gate_hits[0], "terminals": tuple(terminals),
                        "pmos": is_pmos, "nfin": nfin,
                        "center": ((x0 + x1) / 2, (y0 + y1) / 2)})
    return devices, labels


def verify_topology(cell: gdstk.Cell) -> None:
    devices, labels = extract_connectivity(cell)
    expected_pins = {"WLA", "WLB", "BLA", "BLAN", "BLB", "BLBN", "vdd!", "vss!"}
    _assert(expected_pins <= set(labels),
            f"missing pins: {sorted(expected_pins - set(labels))}")
    for pin in ("WLA", "WLB", "BLA", "BLAN", "BLB", "BLBN"):
        _assert(len(labels[pin]) == 1, f"{pin} must label exactly one electrical net")
    signal_roots = {next(iter(labels[p])) for p in ("WLA", "WLB", "BLA", "BLAN", "BLB", "BLBN")}
    _assert(len(signal_roots) == 6, "one or more independent signal pins are shorted")
    _assert(not (signal_roots & (labels["vdd!"] | labels["vss!"])),
            "a signal pin is shorted to a supply rail")

    def access_map(wordline: str, bitlines: tuple[str, str]) -> dict[str, int]:
        wl = next(iter(labels[wordline]))
        selected = [device for device in devices if device["gate"] == wl]
        _assert(len(selected) == 2, f"{wordline} must drive exactly two access devices")
        result: dict[str, int] = {}
        for device in selected:
            terms = set(device["terminals"])
            _assert(not device["pmos"], f"{wordline} access device is not NMOS")
            _assert(device["nfin"] == ACCESS_NFIN,
                    f"{wordline} access device is {device['nfin']}-fin, not "
                    f"nfin={ACCESS_NFIN}")
            matches = [pin for pin in bitlines if next(iter(labels[pin])) in terms]
            _assert(len(matches) == 1,
                    f"{wordline} access device is not tied to one {bitlines} bitline")
            bitline_root = next(iter(labels[matches[0]]))
            storage = terms - {bitline_root}
            _assert(len(storage) == 1, "access device has ambiguous storage terminal")
            result[matches[0]] = storage.pop()
        _assert(set(result) == set(bitlines), f"{wordline} does not cover both bitlines")
        return result

    port_a = access_map("WLA", ("BLA", "BLAN"))
    port_b = access_map("WLB", ("BLB", "BLBN"))
    _assert(port_a["BLA"] == port_b["BLB"], "BLA and BLB do not access the same node")
    _assert(port_a["BLAN"] == port_b["BLBN"],
            "BLAN and BLBN do not access the same node")
    _assert(port_a["BLA"] != port_a["BLAN"], "Q and QB are shorted")

    # The remaining four devices must be the two cross-coupled CMOS inverters.
    q, qb = port_a["BLA"], port_a["BLAN"]
    supply_vdd = labels["vdd!"]
    supply_vss = labels["vss!"]
    for gate_root, drain_root in ((q, qb), (qb, q)):
        inverter = [device for device in devices if device["gate"] == gate_root]
        _assert(len(inverter) == 2, "storage node does not drive one CMOS inverter")
        _assert({device["pmos"] for device in inverter} == {False, True},
                "storage inverter is not one PMOS plus one NMOS")
        for device in inverter:
            terms = set(device["terminals"])
            expected_fins = PULLUP_NFIN if device["pmos"] else PULLDOWN_NFIN
            _assert(device["nfin"] == expected_fins,
                    f"inverter {'pull-up' if device['pmos'] else 'pull-down'} is "
                    f"{device['nfin']}-fin, not nfin={expected_fins}")
            _assert(drain_root in terms, "cross-coupled inverter drain is disconnected")
            other = terms - {drain_root}
            expected_supply = supply_vdd if device["pmos"] else supply_vss
            _assert(len(other) == 1 and next(iter(other)) in expected_supply,
                    "inverter source is not connected to its supply")

    _assert(sum(device["pmos"] for device in devices) == 2,
            "expected 2 PMOS devices")
    _assert(sum(not device["pmos"] for device in devices) == 6,
            "expected 6 NMOS devices")


def sizing_report() -> list[str]:
    """The cell's strength ratios, derived from the sizing constants.

    Every device shares one gate pitch, so fin counts alone fix these ratios;
    verify_topology has already asserted the extracted GDS matches the
    constants, so reporting from the constants reports the layout.

    This is deliberately printed on every --verify rather than buried, because
    the numbers are the argument for or against the macro's concurrent-access
    contract and were previously stated nowhere at all.
    """
    lines = [
        f"pull-down nfin={PULLDOWN_NFIN}, access nfin={ACCESS_NFIN}, "
        f"pull-up nfin={PULLUP_NFIN} (shared L, one gate pitch)",
        f"cell ratio (pull-down/access)      = {CELL_RATIO:.2f}",
        f"  with both ports selected         = {CONCURRENT_CELL_RATIO:.2f}",
        f"pull-up ratio (pull-up/access)     = {PULLUP_RATIO:.2f}",
    ]
    if CELL_RATIO < RECOMMENDED_MIN_CELL_RATIO:
        lines.append(
            f"WARNING: cell ratio {CELL_RATIO:.2f} is below the "
            f"{RECOMMENDED_MIN_CELL_RATIO:.2f} usually recommended for a "
            f"single-port read, and both ports selected halves it to "
            f"{CONCURRENT_CELL_RATIO:.2f}.  docs/asap7_8t_bitcell.md declares "
            f"same-address read/read legal, so this is the sizing that "
            f"contract rests on.  Measure it: "
            f"ctest -R asap7_8t_stability_check")
    return lines


def verify_dummy_topology(cell: gdstk.Cell) -> None:
    """Verify the forced-state dummy against get_dummy_cell_8t()."""
    devices, labels = extract_connectivity(cell)
    bitlines = ("BLA", "BLAN", "BLB", "BLBN")
    _assert(set(bitlines) <= set(labels), "dummy is missing bitline pins")
    _assert("WLA" not in labels and "WLB" not in labels,
            "dummy must not expose wordline pins")
    _assert("vdd!" in labels and "vss!" in labels,
            "dummy is missing forced storage/supply labels")

    pin_roots = {pin: next(iter(labels[pin])) for pin in bitlines}
    _assert(len(set(pin_roots.values())) == 4, "dummy bitlines are shorted")
    expected_storage = {
        "BLA": labels["vss!"],
        "BLB": labels["vss!"],
        "BLAN": labels["vdd!"],
        "BLBN": labels["vdd!"],
    }
    for pin, bitline_root in pin_roots.items():
        access = [device for device in devices
                  if bitline_root in device["terminals"]]
        _assert(len(access) == 1 and not access[0]["pmos"],
                f"{pin} is not attached to exactly one access NMOS")
        device = access[0]
        _assert(device["gate"] in labels["vss!"],
                f"{pin} access gate is not forced inactive")
        other = set(device["terminals"]) - {bitline_root}
        _assert(len(other) == 1 and next(iter(other)) in expected_storage[pin],
                f"{pin} is attached to the wrong forced storage node")

    _assert(sum(device["pmos"] for device in devices) == 2,
            "dummy expected 2 PMOS devices")
    _assert(sum(not device["pmos"] for device in devices) == 6,
            "dummy expected 6 NMOS devices")
    for device in devices:
        expected_fins = 1 if device["pmos"] else 2
        _assert(device["nfin"] == expected_fins,
                f"dummy {'PMOS' if device['pmos'] else 'NMOS'} is "
                f"{device['nfin']}-fin, not nfin={expected_fins}")


def process_fingerprint(cell: gdstk.Cell) -> str:
    layers = {WELL, FIN, GATE, GCUT, NSELECT, PSELECT,
              SRAMDRC, BOUNDARY, SRAMVT}
    records = []
    for poly in cell.polygons:
        if poly.layer in layers:
            records.append((poly.layer, poly.datatype,
                            tuple((round(float(x), 6), round(float(y), 6))
                                  for x, y in poly.points)))
    digest = hashlib.sha256()
    for record in sorted(records, key=repr):
        digest.update(repr(record).encode("utf-8"))
    return digest.hexdigest()


def filtered_polygon_fingerprint(cell: gdstk.Cell, predicate) -> str:
    records = []
    for poly in cell.polygons:
        if predicate(poly):
            records.append((poly.layer, poly.datatype,
                            tuple((round(float(x), 6), round(float(y), 6))
                                  for x, y in poly.points)))
    digest = hashlib.sha256()
    for record in sorted(records, key=repr):
        digest.update(repr(record).encode("utf-8"))
    return digest.hexdigest()


def verify_cap_topology(cell: gdstk.Cell, cap_kind: str) -> None:
    devices, labels = extract_connectivity(cell, expected_channels=0)
    _assert(not devices, f"{cell.name} must be electrically device-free")
    _assert(not _layer_polygons(cell, ACTIVE),
            f"{cell.name} must not contain ACTIVE")
    bitlines = {"BLA", "BLAN", "BLB", "BLBN"}

    if cap_kind == "col":
        _assert(bitlines <= set(labels), "column cap is missing bitline pins")
        _assert("WLA" not in labels and "WLB" not in labels,
                "column cap unexpectedly exposes wordlines")
        roots = [next(iter(labels[pin])) for pin in sorted(bitlines)]
        _assert(len(set(roots)) == 4, "column-cap bitlines are shorted")
    elif cap_kind == "row":
        _assert({"WLA", "WLB"} <= set(labels),
                "row cap is missing wordline pins")
        _assert(not (bitlines & set(labels)),
                "row cap unexpectedly exposes bitlines")
        _assert(next(iter(labels["WLA"])) != next(iter(labels["WLB"])),
                "row-cap wordlines are shorted")
    elif cap_kind == "corner":
        _assert(not ({"WLA", "WLB"} | bitlines) & set(labels),
                "corner cap unexpectedly exposes signal pins")
    else:
        raise ValueError(f"unknown cap kind {cap_kind}")

    _assert("vss!" in labels, f"{cell.name} is missing ground continuity")


def _contains(outer: tuple, inner: tuple) -> bool:
    """True when box `inner` sits inside box `outer`."""
    return (outer[0] <= inner[0] and outer[1] <= inner[1]
            and inner[2] <= outer[2] and inner[3] <= outer[3])


def verify_tap_rules(cell: gdstk.Cell) -> None:
    """Grid rules for the tap: the bitcell's frame without its devices."""
    for layer in (SRAMDRC, BOUNDARY):
        _assert([bbox(p) for p in _layer_polygons(cell, layer)] == [MARKER],
                f"tap layer {layer} marker is not {MARKER}")
    _assert(not _layer_polygons(cell, SRAMVT),
            "tap must not carry the SRAM-Vt implant")

    fins = sorted(_layer_polygons(cell, FIN), key=lambda p: bbox(p)[1])
    centres = [round((bbox(p)[1] + bbox(p)[3]) / 2, 6) for p in fins]
    _assert(all(round(bbox(p)[3] - bbox(p)[1], 6) == 0.007 for p in fins),
            "tap FIN width is not exactly 7 nm")
    _assert(all(round(right - left, 6) == 0.027
                for left, right in zip(centres, centres[1:])),
            "tap FIN pitch is not exactly 27 nm")
    _assert(sorted(bbox(p) for p in _layer_polygons(cell, GATE))
            == sorted((GATE_A, GATE_B)),
            "tap does not reuse the bitcell gate columns")


def verify_tap_topology(cell: gdstk.Cell, bitcell: gdstk.Cell) -> None:
    """The tap ties both polarities, holds no device, and passes bitlines."""
    devices, labels = extract_connectivity(cell, expected_channels=0)
    _assert(not devices, "tap cell must be device-free")

    well = [bbox(p) for p in _layer_polygons(cell, WELL)]
    nsel = [bbox(p) for p in _layer_polygons(cell, NSELECT)]
    psel = sorted(bbox(p) for p in _layer_polygons(cell, PSELECT))
    _assert(len(well) == 1, "tap must have exactly one n-well band")
    _assert(nsel == well,
            "tap n+ implant must cover exactly the n-well (the VDD tie)")
    _assert(bool(psel), "tap has no p+ substrate tie")
    for box in psel:
        _assert(box[3] <= well[0][1] or well[0][3] <= box[1],
                "tap p+ implant overlaps the n-well")

    # One tie diffusion per implant band, clear of both gate columns so no
    # channel can form, each climbing LISD -> V0 -> M1.
    implants = nsel + psel
    actives = sorted(bbox(p) for p in _layer_polygons(cell, ACTIVE))
    _assert(len(actives) == len(implants),
            "tap does not place one tie diffusion per implant band")
    gates = [bbox(p) for p in _layer_polygons(cell, GATE)]
    contacts = [bbox(p) for p in _layer_polygons(cell, LISD)]
    vias = [bbox(p) for p in _layer_polygons(cell, V0)]
    straps = [bbox(p) for p in _layer_polygons(cell, M1)]
    for active in actives:
        host = [box for box in implants if _contains(box, active)]
        _assert(len(host) == 1, "tie diffusion is not inside one implant band")
        for gate in gates:
            _assert(active[2] <= gate[0] or gate[2] <= active[0],
                    "tie diffusion crosses a gate column")
        contact = [box for box in contacts if _contains(box, active)]
        _assert(len(contact) == 1, "tie diffusion has no local-interconnect tab")
        via = [box for box in vias if _contains(contact[0], box)]
        _assert(len(via) == 1, "tie contact has no V0")
        _assert(any(_contains(strap, via[0]) for strap in straps),
                "tie V0 is not covered by an M1 strap")

    # Every strap lands a V1 on a supply rail carrying its own net.
    rails = supply_rails(cell)
    v1_boxes = [bbox(p) for p in _layer_polygons(cell, V1)]
    for net, group in group_tie_bands(tie_bands(bitcell)):
        bottom = group[0][0] + TAP_BAND_INSET
        top = group[-1][1] - TAP_BAND_INSET
        strap = [m for m in straps
                 if abs(m[1] - bottom) < 1e-9 and abs(m[3] - top) < 1e-9]
        _assert(len(strap) == 1, f"tap has no single {net} strap")
        landed = [v for v in v1_boxes if _contains(strap[0], v)]
        _assert(bool(landed), f"tap {net} strap has no V1 to a supply rail")
        for via in landed:
            _assert(any(rail[0] <= via[1] and via[3] <= rail[1]
                        for rail in rails[net]),
                    f"tap {net} V1 does not land on a {net} rail")

    _assert(labels["vdd!"].isdisjoint(labels["vss!"]),
            "tap shorts VDD to VSS")

    # A tap sits mid-row, so it must hand every bitline and supply straight
    # through at exactly the bitcell's coordinates.
    rail_predicate = lambda poly: is_full_m2_rail(poly) or is_bitline_m4_rail(poly)
    _assert(
        filtered_polygon_fingerprint(cell, rail_predicate)
        == filtered_polygon_fingerprint(bitcell, rail_predicate),
        "tap rails do not abut the bitcell's bitlines and supplies",
    )


def verify_rules(cell: gdstk.Cell) -> None:
    """Check the exact-grid and focused routing rules used by this generator."""
    markers = {layer: [bbox(p) for p in _layer_polygons(cell, layer)]
               for layer in MARKER_LAYERS}
    for layer, boxes in markers.items():
        _assert(boxes == [MARKER], f"layer {layer} marker is not {MARKER}")

    fins = sorted(_layer_polygons(cell, FIN), key=lambda p: bbox(p)[1])
    centers = [round((bbox(p)[1] + bbox(p)[3]) / 2, 6) for p in fins]
    _assert(all(round(bbox(p)[3] - bbox(p)[1], 6) == 0.007 for p in fins),
            "FIN width is not exactly 7 nm")
    _assert(all(round(right - left, 6) == 0.027
                for left, right in zip(centers, centers[1:])),
            "FIN pitch is not exactly 27 nm")

    gates = _layer_polygons(cell, GATE)
    _assert(len(gates) == 2, "expected the two official gate columns")
    _assert(sorted(bbox(poly) for poly in gates) == sorted((GATE_A, GATE_B)),
            "GATE columns do not span the complete north/south boundary")
    gate_centers = sorted(round((bbox(p)[0] + bbox(p)[2]) / 2, 6) for p in gates)
    _assert(gate_centers == [0.027, 0.081], "gate centers are off the 54 nm pitch")
    _assert(all(round(bbox(p)[2] - bbox(p)[0], 6) == 0.020 for p in gates),
            "GATE width is not exactly 20 nm")

    for layer in (M1, M2, M3):
        for poly in _layer_polygons(cell, layer):
            x0, y0, x1, y1 = bbox(poly)
            _assert(min(x1 - x0, y1 - y0) >= 0.018 - 1e-9,
                    f"layer {layer} has a sub-18 nm shape")

    m4 = _layer_polygons(cell, M4)
    for poly in m4:
        x0, y0, x1, y1 = bbox(poly)
        _assert(round(y1 - y0, 6) == 0.024, "M4 route is not 24 nm wide")
        _assert(abs(y0 / 0.024 - round(y0 / 0.024)) < 1e-6 and
                abs(y1 / 0.024 - round(y1 / 0.024)) < 1e-6,
                "M4 horizontal edges are off the 24 nm grid")
        _assert((x1 - x0) * (y1 - y0) >= 0.002 - 1e-12,
                "M4 area is below 2,000 nm^2")
    m4_y = sorted((bbox(p)[1], bbox(p)[3]) for p in m4)
    _assert(all(next_y0 - y1 >= 0.024 - 1e-9
                for (_, y1), (next_y0, _) in zip(m4_y, m4_y[1:])),
            "M4 tracks violate 24 nm spacing")

    for poly in _layer_polygons(cell, M5):
        x0, _y0, x1, _y1 = bbox(poly)
        _assert(round(x1 - x0, 6) == 0.024, "M5 route is not 24 nm wide")
        _assert(abs(x0 / 0.024 - round(x0 / 0.024)) < 1e-6 and
                abs(x1 / 0.024 - round(x1 / 0.024)) < 1e-6,
                "M5 vertical edges are off the 24 nm grid")

    # All four storage terminals retain the source cell's 24 nm LISD/SDT
    # width.  Their 18 nm V0 landings are centered on the 54 nm inter-gate
    # midpoint with 3 nm LISD enclosure on each side.
    lisd = _layer_polygons(cell, LISD)
    for index, left in enumerate(lisd):
        for right in lisd[:index]:
            _assert(not _overlap(left, right),
                    "LISD contains redundant overlapping polygons")
    if cell.name == CELL_NAME:
        wla_lig = {
            (0.000, -0.0115, 0.037, 0.0045),
            (0.071, 0.2655, 0.096, 0.2815),
        }
        _assert(wla_lig <= {bbox(poly) for poly in _layer_polygons(cell, LIG)},
                "WLA LIG landings do not preserve 15 nm LISD spacing")
        storage_lisd = {
            (0.042, -0.0945, 0.066, -0.0405),
            (0.042, 0.0235, 0.066, 0.118),
            (0.042, 0.152, 0.066, 0.2465),
            (0.042, 0.3105, 0.066, 0.3645),
        }
        _assert(storage_lisd <= {bbox(poly) for poly in lisd},
                "storage LISD terminals are not canonical 24 nm shapes")
        storage_v0 = {
            (0.045, -0.0765, 0.063, -0.0585),
            (0.045, 0.043, 0.063, 0.061),
            (0.045, 0.2195, 0.063, 0.2375),
            (0.045, 0.3285, 0.063, 0.3465),
        }
        _assert(storage_v0 <= {bbox(poly) for poly in _layer_polygons(cell, V0)},
                "storage V0 landings are not centered at x=54 nm")
        _assert(
            (-0.009, -0.0885, 0.009, -0.0705)
            in {bbox(poly) for poly in _layer_polygons(cell, V0)},
            "BLBN V0 landing protrudes beyond its LISD terminal",
        )
    lisd_gate_overlap = gdstk.boolean(lisd, gates, "and", precision=1e-6)
    _assert(not lisd_gate_overlap, "LISD must not overlap either GATE column")
    lisd_union = gdstk.boolean(lisd, [], "or", precision=1e-6)
    uncovered_sdt = gdstk.boolean(
        _layer_polygons(cell, SDT), lisd_union, "not", precision=1e-6
    )
    _assert(not uncovered_sdt, "SDT must be completely contained by LISD")


def verify_gds(path: Path) -> str:
    lib = gdstk.read_gds(str(path))
    cell = next((candidate for candidate in lib.cells if candidate.name == CELL_NAME), None)
    if cell is None:
        raise ValueError(f"{CELL_NAME!r} not found in {path}")
    _assert(len(lib.cells) == 1, f"{path} must contain only {CELL_NAME}")
    verify_rules(cell)
    verify_topology(cell)
    return polygon_fingerprint(cell)


def verify_tap_gds(path: Path, bitcell_path: Path) -> str:
    lib = gdstk.read_gds(str(path))
    cells = {cell.name: cell for cell in lib.cells}
    _assert(set(cells) == {TAP_CELL_NAME},
            f"tap library cells are {sorted(cells)}, expected [{TAP_CELL_NAME}]")
    bit_lib = gdstk.read_gds(str(bitcell_path))
    bitcell = next((c for c in bit_lib.cells if c.name == CELL_NAME), None)
    if bitcell is None:
        raise ValueError(f"{CELL_NAME!r} not found in {bitcell_path}")
    cell = cells[TAP_CELL_NAME]
    verify_tap_rules(cell)
    verify_tap_topology(cell, bitcell)
    return polygon_fingerprint(cell)


def verify_edge_gds(path: Path) -> dict[str, str]:
    lib = gdstk.read_gds(str(path))
    cells = {cell.name: cell for cell in lib.cells}
    _assert(set(cells) == set(EDGE_CELL_NAMES),
            f"edge library cells are {sorted(cells)}, expected {list(EDGE_CELL_NAMES)}")

    for cell in cells.values():
        verify_rules(cell)
    verify_dummy_topology(cells["dummy_cell_8t"])
    verify_cap_topology(cells["sram_cell_8t_col_cap"], "col")
    verify_cap_topology(cells["sram_cell_8t_row_cap"], "row")
    verify_cap_topology(cells["sram_cell_8t_corner"], "corner")

    frames = {name: process_fingerprint(cell) for name, cell in cells.items()}
    _assert(len(set(frames.values())) == 1,
            "dummy/cap process frames do not match at abutment boundaries")

    col_cap = cells["sram_cell_8t_col_cap"]
    row_cap = cells["sram_cell_8t_row_cap"]
    corner = cells["sram_cell_8t_corner"]
    rail_predicate = lambda poly: is_full_m2_rail(poly) or is_bitline_m4_rail(poly)
    _assert(
        filtered_polygon_fingerprint(col_cap, rail_predicate)
        == filtered_polygon_fingerprint(corner, rail_predicate),
        "column and corner cap rails do not abut identically",
    )

    conductor_layers = {LIG, LISD, V0, M1, V1, M2, V2, M3, V3, M4, V4, M5}
    wordline_predicate = lambda poly: (
        poly.layer in conductor_layers and not rail_predicate(poly)
    )
    _assert(
        filtered_polygon_fingerprint(row_cap, wordline_predicate)
        == filtered_polygon_fingerprint(corner, wordline_predicate),
        "row and corner cap wordline routes do not abut identically",
    )

    col_rail_boxes = {bbox(poly) for poly in col_cap.polygons
                      if rail_predicate(poly)}
    expected_rails = {
        (-0.027, 0.027, 0.135, 0.045),
        (-0.026, 0.0745, 0.136, 0.0925),
        (-0.027, 0.126, 0.135, 0.144),
        (-0.026, 0.1775, 0.136, 0.1955),
        (-0.027, 0.225, 0.135, 0.243),
        (-0.027, -0.096, 0.135, -0.072),
        (-0.027, 0.336, 0.135, 0.360),
    }
    _assert(col_rail_boxes == expected_rails,
            "column-cap rails do not span the complete mirrored cell pitch")
    _assert(any(is_box(poly, WLA_M3)
                for poly in _layer_polygons(row_cap, M3)),
            "row cap is missing the full-height WLA M3 trunk")
    _assert(any(is_box(poly, WLB_M5)
                for poly in _layer_polygons(row_cap, M5)),
            "row cap is missing the full-height WLB M5 trunk")
    return {name: polygon_fingerprint(cell) for name, cell in sorted(cells.items())}


def parse_args(argv: list[str]) -> argparse.Namespace:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-gds",
        type=Path,
        default=repo / "tech/gds/srambank_32b_boundary_2.gds",
        help="tracked ASAP7 GDS containing sram_cell_6t_122",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repo / "tech/gds/sram_cell_8t.gds",
        help="generated bitcell-only GDS",
    )
    parser.add_argument(
        "--edge-output",
        type=Path,
        help="generated dummy/row-cap/column-cap/corner GDS; defaults beside --output",
    )
    parser.add_argument(
        "--tap-output",
        type=Path,
        help="generated array well/substrate tap GDS; defaults beside --output",
    )
    verify_group = parser.add_mutually_exclusive_group()
    verify_group.add_argument(
        "--verify",
        type=Path,
        help="verify an existing GDS instead of generating one",
    )
    verify_group.add_argument(
        "--verify-edges",
        type=Path,
        help="verify an existing 8T edge-cell GDS instead of generating",
    )
    verify_group.add_argument(
        "--verify-tap",
        type=Path,
        help="verify an existing 8T tap GDS instead of generating",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.verify:
            digest = verify_gds(args.verify)
            print(f"PASS {args.verify}: {CELL_NAME}, topology=8T, sha256={digest}")
            for line in sizing_report():
                print(f"  {line}")
            return 0

        if args.verify_edges:
            digests = verify_edge_gds(args.verify_edges)
            print(f"PASS {args.verify_edges}: {len(digests)} edge cells")
            for name, digest in digests.items():
                print(f"  {name}: sha256={digest}")
            return 0

        if args.verify_tap:
            bitcell_path = args.output
            digest = verify_tap_gds(args.verify_tap, bitcell_path)
            print(f"PASS {args.verify_tap}: {TAP_CELL_NAME}, sha256={digest}")
            return 0

        lib = build_cell(args.source_gds)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        lib.write_gds(str(args.output), timestamp=FIXED_GDS_TIMESTAMP)
        digest = verify_gds(args.output)
        print(f"wrote {args.output}: {CELL_NAME}, topology=8T, sha256={digest}")

        edge_output = args.edge_output or args.output.with_name("sram_cell_8t_edges.gds")
        edge_output.parent.mkdir(parents=True, exist_ok=True)
        edge_lib = build_edge_library(
            lib.cells[0], unit=lib.unit, precision=lib.precision
        )
        edge_lib.write_gds(str(edge_output), timestamp=FIXED_GDS_TIMESTAMP)
        edge_digests = verify_edge_gds(edge_output)
        print(f"wrote {edge_output}: {len(edge_digests)} edge cells")
        for name, edge_digest in edge_digests.items():
            print(f"  {name}: sha256={edge_digest}")

        tap_output = args.tap_output or args.output.with_name(
            "sram_cell_8t_tap.gds"
        )
        tap_output.parent.mkdir(parents=True, exist_ok=True)
        tap_lib = build_tap_library(
            lib.cells[0], unit=lib.unit, precision=lib.precision
        )
        tap_lib.write_gds(str(tap_output), timestamp=FIXED_GDS_TIMESTAMP)
        tap_digest = verify_tap_gds(tap_output, args.output)
        print(f"wrote {tap_output}: {TAP_CELL_NAME}, sha256={tap_digest}")
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
