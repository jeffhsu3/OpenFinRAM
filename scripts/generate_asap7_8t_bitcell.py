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

The second generated GDS contains the forced-state dummy, explicit oriented
edge masters, parameterized dummy rows, and blank fillers at the 8T pitch.
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
CORE_EDGE_CELL_NAMES = (
    "dummy_cell_8t",
    "sram_cell_8t_col_cap",
    "sram_cell_8t_row_cap",
    "sram_cell_8t_corner",
)
ORIENTATIONS = ((False, False), (True, False), (False, True), (True, True))


def oriented_name(base: str, mirror_x: bool = False, mirror_y: bool = False) -> str:
    """_lr reverses X; _v2 reverses Y, both about the placement boundary."""
    return base + ("_v2" if mirror_y else "") + ("_lr" if mirror_x else "")


def topbot_name(mirror_x: bool = False, mirror_y: bool = False) -> str:
    return f"dummy_topbot_8t_v{2 if mirror_y else 1}" + ("_lr" if mirror_x else "")


EDGE_CELL_NAMES = tuple(dict.fromkeys(
    CORE_EDGE_CELL_NAMES
    + tuple(oriented_name(base, mx, my)
            for base in ("dummy_vertical_8t", "sram_cell_8t_corner")
            for mx, my in ORIENTATIONS)
    + tuple(topbot_name(mx, my) for mx, my in ORIENTATIONS)
    + ("FILLER_BLANK_8t", "FILLER_cgedge_8t")
))
TAP_CELL_NAME = "tapcell_sram_8t"
STRAP_CELL_NAME = "strapcell_sram_8t"
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

# Well/substrate tie geometry.  The tap is two bitcell slots wide.  Its tie
# implants are the complement of its neighbours' at every band, and a
# neighbouring bitcell's ACTIVE and select overhang the seam by 8 and 27 nm,
# so the tap's own implants start 27 nm in: on a one-slot tap that left a
# 54 nm implant, under the 108 nm minimum (PSELECT.W.1), and a full-width one
# doubly implanted the neighbour's bitline diffusion into a substrate tie.
# Two slots leave 162 nm.  The tie diffusions sit between the second slot's
# two gate columns on the bitcell's own fin grid, so no gate can cross them
# and the tap stays device-free while keeping the array's FIN and poly pitch.
TAP_SLOTS = 2
TAP_MARKER = (0.000, -0.162, TAP_SLOTS * 0.108, 0.432)
TAP_IMPLANT_INSET = 0.027
TAP_TIE_OFFSET = (TAP_SLOTS - 1) * 0.108 / 2
TAP_ACTIVE_X = (0.046 + TAP_TIE_OFFSET, 0.062 + TAP_TIE_OFFSET)
TAP_CONTACT_X = (0.042 + TAP_TIE_OFFSET, 0.066 + TAP_TIE_OFFSET)
TAP_VIA_X = (0.045 + TAP_TIE_OFFSET, 0.063 + TAP_TIE_OFFSET)
TAP_VIA_HEIGHT = 0.018
TAP_BAND_INSET = 0.0135

# Power-strap geometry.  The bitcell fills M1-M5 (M3 is WLA, M4 carries the
# port-B bitlines, M5 is WLB), so the only place a supply can climb out of the
# M2 rails is the tap column, where M3 and M5 are unused.  M3 still has to
# clear the 9 nm neighbour overhang at both slot edges, which pins the climb to
# the cell centre; M5 is free across the whole slot, so the two spines sit on
# the gate-column centres.
STRAP_CLIMB_X = 0.054 + TAP_TIE_OFFSET
STRAP_SPINE_X = {"vss!": 0.027 + TAP_TIE_OFFSET, "vdd!": 0.081 + TAP_TIE_OFFSET}
STRAP_STEP_Y = 0.050
STRAP_M5_HALF = 0.012
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
    # 6 nm of M2 past V1/V2 each side: the public deck's V1.M2.EN.2 opening
    # drops an end cap of exactly 5 nm.
    rect(cell, (cx - 0.015, cy - 0.009, cx + 0.015, cy + 0.009), M2)
    rect(cell, (cx - 0.009, cy - 0.009, cx + 0.009, cy + 0.009), V2)
    rect(cell, (cx - 0.009, cy - 0.017, cx + 0.009, cy + 0.017), M3)
    rect(cell, (cx - 0.009, cy - 0.012, cx + 0.009, cy + 0.012), V3)


def add_gate_to_m5(
    cell: gdstk.Cell, gate_via_x: float, bridge_x: float, y: float,
    landing_y: float | None = None,
) -> None:
    """Connect an LIG wordline contact to the vertical WLB route on M5.

    The contact climbs to M2, runs to the M3 bridge, and the bridge carries
    it to the M4 landing on `landing_y` (the contact's own track unless
    given), where a V4 reaches the WLB trunk.
    """
    landing_y = y if landing_y is None else landing_y
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
    rect(cell, (bridge_x - 0.009, min(y, landing_y) - 0.017,
                bridge_x + 0.009, max(y, landing_y) + 0.017), M3)
    rect(cell, (bridge_x - 0.009, landing_y - 0.012,
                bridge_x + 0.009, landing_y + 0.012), V3)
    # The M4 landing runs from V3 to the V4 under the trunk, 11 nm past
    # each (V3.M4.EN.2 / V4.M4.EN.1), with M4.W.5's 44 nm minimum
    # horizontal length. The DRM has no 2,000 nm^2 M4 area rule.
    v4 = (0.048, 0.072)
    x0 = min(bridge_x - 0.009, v4[0]) - 0.011
    x1 = max(bridge_x + 0.009, v4[1]) + 0.011
    rect(cell, (x0, landing_y - 0.012, x1, landing_y + 0.012), M4)
    rect(cell, (v4[0], landing_y - 0.012, v4[1], landing_y + 0.012), V4)


def add_wla_routes(cell: gdstk.Cell) -> None:
    """Add the two WLA gate contacts and their common M3 route."""
    # Bottom WLA contact: on gate A, 16 nm clear of the west placement seam,
    # the mirror image of the top contact on gate B.  It used to start on the
    # seam (LIG 0..37 nm, V0 on the overhang at 6..24 nm).  Columns are placed
    # mirrored about that seam, so two such contacts met edge to edge and their
    # LIG was one strap: adjacent WLA wordlines were shorted in pairs in every
    # row, and the last before a tap or corner to the corner's vss! track.
    # The lower storage strap jogs east around this M1 landing below.  The
    # 0.5 nm south shift gives the required 15 nm corner spacing to the LISD
    # above.  With the V0 on the gate the LIG needs only its 1 nm extension
    # past GATE (LIG.GATE.EX.1), which leaves 32 nm between mirrored
    # neighbours, over the 31 nm LIG.S.4-5 asks of two short edges.
    rect(cell, (0.016, -0.0115, 0.037, 0.0045), LIG)
    rect(cell, (0.018, -0.0125, 0.036, 0.0055), V0)
    rect(cell, (0.018, -0.017, 0.036, 0.011), M1)
    rect(cell, (0.018, -0.012, 0.036, 0.006), V1)
    rect(cell, (0.016, -0.012, 0.072, 0.006), M2)
    rect(cell, (0.045, -0.012, 0.063, 0.006), V2)

    # Keep the top WLA landing away from the east/west placement seam.  The
    # upper storage strap jogs around this M1 landing below.
    # The 1 nm north shift gives 15 nm corner spacing to the LISD below.
    rect(cell, (0.071, 0.2655, 0.092, 0.2815), LIG)
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
    # The lower M1 jogs right while passing the bottom WLA landing (x=18..36),
    # as the upper one jogs left past the top landing: 18 nm to the landing,
    # 27 nm to the bitline M1 at x=99, landings unmoved under both storage V0.
    cell.add(
        gdstk.Polygon(
            (
                (0.045, -0.0815), (0.063, -0.0815),
                (0.063, -0.0715), (0.072, -0.0715),
                (0.072, 0.056), (0.063, 0.056),
                (0.063, 0.066), (0.045, 0.066),
                (0.045, 0.038), (0.054, 0.038),
                (0.054, -0.0535), (0.045, -0.0535),
            ),
            layer=M1,
            datatype=0,
        )
    )
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


#: Alternating mirrored slots put the WLB landings on different M4 tracks.
#: Both variants use x=18 nm for the M3 bridge: the x=90 nm corridor remains
#: available to the macro router. The former variant's x=90 bridge stopped
#: x16x16 routing (see commit 51d400c).
#: BLBN's M4 bar moves from y=-84 to -36 nm, with its upper via stack 18 nm
#: into the unmirrored cell. Variant B uses x=-18 so the shared stacks coincide
#: at the mirrored seam, leaving its x=18 WLB bridge a legal 18 nm M3 gap.
#: This bridge can now reach y=12 without crossing the BLBN stack. BLB and
#: the cell boundary, devices and external wordline tracks do not move.
WLB_LANDINGS = {False: (-0.132, 0.396), True: (0.012, 0.300)}
VARIANT_B_NAME = CELL_NAME + "_b"
VARIANT_B_END_NAME = CELL_NAME + "_b_end"


def add_wlb_routes(cell: gdstk.Cell, variant: bool = False) -> None:
    """Add the two WLB gate contacts and their common M5 route."""
    rect(cell, (0.017, -0.140, 0.054, -0.124), LIG)
    rect(cell, (0.054, 0.388, 0.086, 0.404), LIG)
    lower, upper = WLB_LANDINGS[variant]
    add_gate_to_m5(
        cell,
        gate_via_x=0.0395,
        bridge_x=0.018,
        y=-0.132,
        landing_y=lower,
    )
    add_gate_to_m5(
        cell,
        gate_via_x=0.0685,
        bridge_x=0.018,
        y=0.396,
        landing_y=upper,
    )
    rect(cell, WLB_M5, M5)


def add_storage_straps_and_wla(cell: gdstk.Cell) -> None:
    add_wla_routes(cell)
    add_storage_straps(cell)


def add_port_b(cell: gdstk.Cell, variant: bool = False, terminal: bool = False) -> None:
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

    # M4 bitlines stay on the 48 nm track grid. BLBN's V0 remains within
    # the original LISD contact; M1 reaches a V1 at y=-57, M2 steps sideways,
    # and M3 reaches the bitline at y=-36. The 58 nm M3 sides use the 18 nm
    # long-edge spacing rule beside the WLB bridge, rather than the 25 nm
    # rule for a short edge. Both variants share this stack at their seam.
    # Move the shared BLBN upper stack off the mirrored seam, into A.
    # An untapped terminal B cannot put this stack outside the array, where
    # it would approach the IO metal by only 5 nm. Use the inward x=90 track
    # there; tapped macro interiors retain the router corridor on that track.
    bx = 0.090 if terminal else (-0.018 if variant else 0.018)
    rect(cell, (-0.009, -0.0885, 0.009, -0.0705), V0)
    rect(cell, (-0.009, -0.0935, 0.009, -0.043), M1)
    rect(cell, (-0.009, -0.066, 0.009, -0.048), V1)
    rect(cell, (min(0, bx) - 0.014, -0.066,
                max(0, bx) + 0.014, -0.048), M2)
    rect(cell, (bx - 0.009, -0.066, bx + 0.009, -0.048), V2)
    rect(cell, (bx - 0.009, -0.077, bx + 0.009, -0.019), M3)
    rect(cell, (bx - 0.009, -0.048, bx + 0.009, -0.024), V3)
    rect(cell, (-0.038, -0.048, 0.135, -0.024), M4)
    add_via_stack_to_m4(cell, 0.108, 0.348)
    rect(cell, (-0.027, 0.336, 0.135, 0.360), M4)

    # WLB gate stacks occupy the neighboring M4 tracks, leaving exactly the
    # required 24 nm M4 spacing to the port-B bitlines.
    add_wlb_routes(cell, variant)

    cell.add(gdstk.Label("BLBN", (-0.018, -0.036), layer=M4,
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
    # The cell, and variant B for the mirrored slots of a column (WLB_LANDINGS).
    for name, variant, terminal in ((CELL_NAME, False, False),
                                    (VARIANT_B_NAME, True, False),
                                    (VARIANT_B_END_NAME, True, True)):
        cell = lib.new_cell(name)
        clone_base(source, cell)
        add_storage_straps_and_wla(cell)
        add_port_b(cell, variant, terminal)

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
    dx: float = 0.0,
) -> None:
    for poly in source.polygons:
        if predicate(poly):
            clone = gdstk.Polygon(
                poly.points.copy(), layer=poly.layer, datatype=poly.datatype
            )
            if dx:
                clone.translate(dx, 0.0)
            target.add(clone)


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
    # The bitcell's two port-B bitlines, or a tap's stretch of them.
    x0, y0, x1, y1 = bbox(poly)
    return x0 in (-0.027, -0.038) and x1 - x0 >= 0.160 and (y0, y1) in {(-0.048, -0.024), (0.336, 0.360)}


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
    bitcell: gdstk.Cell, unit: float = 1e-6, precision: float = 2.5e-10,
    row_counts: tuple[int, ...] = (64,),
) -> gdstk.Library:
    """Build edge masters and dummy arrays at the bitcell's placement pitch."""
    if not row_counts or any(type(count) is not int or count < 1 for count in row_counts):
        raise ValueError("edge row counts must be positive integers")
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
    # The masters mirrored in x stand in a row where the bitcell would be
    # mirrored, so they take variant B's WLB landings (WLB_LANDINGS).
    row_cap_b = gdstk.Cell("sram_cell_8t_row_cap_b")
    for cap, variant in ((row_cap, False), (row_cap_b, True)):
        add_process_frame(bitcell, cap)
        add_ground_rails(bitcell, cap)
        add_wla_routes(cap)
        add_wlb_routes(cap, variant)
        cap.add(
            gdstk.Label("WLA", (0.054, -0.028), layer=M3,
                         texttype=PIN_TEXTTYPE),
            gdstk.Label("WLB", (0.060, -0.150), layer=M5,
                         texttype=PIN_TEXTTYPE),
        )
        copy_labels(bitcell, cap, {"vss!"})

    # Corner cap carries both sets of terminating routes, all grounded, but no
    # ACTIVE. Bake each orientation into a separate master: placement must
    # select the matching process-band orientation, not mirror a reference.
    corner = lib.new_cell("sram_cell_8t_corner")
    corner_b = gdstk.Cell("sram_cell_8t_corner_b")
    for cap, variant in ((corner, False), (corner_b, True)):
        add_process_frame(bitcell, cap)
        add_full_rails(bitcell, cap)
        add_wla_routes(cap)
        add_wlb_routes(cap, variant)
        copy_labels(
            bitcell,
            cap,
            {"BLA", "BLAN", "BLB", "BLBN", "vdd!", "vss!"},
            {"BLA": "vss!", "BLAN": "vss!", "BLB": "vss!", "BLBN": "vss!"},
        )
        cap.add(
            gdstk.Label("vss!", (0.054, -0.028), layer=M3,
                         texttype=PIN_TEXTTYPE),
            gdstk.Label("vss!", (0.060, -0.150), layer=M5,
                         texttype=PIN_TEXTTYPE),
        )
    for mx, my in ORIENTATIONS:
        if mx or my:
            lib.add(oriented_cell(corner_b if mx else corner, oriented_name(corner.name, mx, my), mx, my))
        lib.add(oriented_cell(row_cap_b if mx else row_cap, oriented_name("dummy_vertical_8t", mx, my), mx, my))
        lib.add(oriented_cell(col_cap, topbot_name(mx, my), mx, my))

    # Like FILLER_BLANK_6t122, the blank is a half-width FIN/poly tile with
    # no well, select, ACTIVE, or conductors. Use the extended 8T grids.
    x0, y0, x1, y1 = edge_boundary(bitcell)
    half = (x1 - x0) / 2
    blank = lib.new_cell("FILLER_BLANK_8t")
    for poly in bitcell.polygons:
        if poly.layer == FIN:
            _, fy0, _, fy1 = bbox(poly)
            rect(blank, (0, fy0, half, fy1), FIN)
        elif poly.layer == GATE and bbox(poly)[2] <= x0 + half:
            blank.add(poly.copy().translate(-x0, 0))
    rect(blank, (0, y0, half, y1), BOUNDARY)
    filler = lib.new_cell("FILLER_cgedge_8t")
    for row in range(4):
        for col in range(2):
            filler.add(gdstk.Reference(
                blank, origin=(col * half, (row + 1) * (y1 - y0) + y0
                               if row % 2 else row * (y1 - y0) - y0),
                x_reflection=bool(row % 2),
            ))
    rect(filler, (0, 0, 2 * half, 4 * (y1 - y0)), BOUNDARY)
    cells = {cell.name: cell for cell in lib.cells}
    for count in sorted(set(row_counts)):
        for mx, my in ORIENTATIONS:
            build_dummy_vertical_array(lib, cells, count, mirror_x=mx, mirror_y=my)
    return lib


def edge_boundary(cell: gdstk.Cell) -> tuple[float, float, float, float]:
    boxes = [bbox(p) for p in cell.polygons if p.layer == BOUNDARY and p.datatype == 0]
    if len(boxes) != 1:
        raise ValueError(f"{cell.name}: expected one direct BOUNDARY")
    return boxes[0]


def oriented_cell(source: gdstk.Cell, name: str, mirror_x: bool = False,
                  mirror_y: bool = False) -> gdstk.Cell:
    """Materialize all polygons and pins, retaining the canonical BOUNDARY.

    Reflect around the boundary center, never the geometry bounding box:
    process overhangs and select/well bands need not be symmetric.
    """
    x0, y0, x1, y1 = edge_boundary(source)
    cell = gdstk.Cell(name)
    for poly in source.get_polygons():
        points = poly.points.copy()
        if mirror_x:
            points[:, 0] = x0 + x1 - points[:, 0]
        if mirror_y:
            points[:, 1] = y0 + y1 - points[:, 1]
        cell.add(gdstk.Polygon(points, layer=poly.layer, datatype=poly.datatype))
    for label in source.get_labels():
        copied = clone_label(label)
        x, y = map(float, label.origin)
        copied.origin = (x0 + x1 - x if mirror_x else x,
                         y0 + y1 - y if mirror_y else y)
        cell.add(copied)
    return cell


def dummy_array_name(rows: int, tap_pitch: int = 0, mirror_x: bool = False,
                     mirror_y: bool = False) -> str:
    tap = f"_tap{tap_pitch}" if tap_pitch else ""
    return oriented_name(f"dummy_vertical_array_X{rows}{tap}_8t", mirror_x, mirror_y)


def build_dummy_vertical_array(
    library: gdstk.Library, cells: dict[str, gdstk.Cell], rows: int,
    tap_pitch: int = 0, *, mirror_x: bool = False, mirror_y: bool = False,
) -> gdstk.Cell:
    """Tile one end row, like dummy_vertical_array_X64, for any address count.

    Addresses run along X in the academic hierarchy. Taps occupy extra slots
    and take grounded corners; they do not advance the bitcell mirror parity.
    All references translate explicit masters, including the opposite ends.
    """
    if type(rows) is not int or rows < 1:
        raise ValueError("edge row count must be a positive integer")
    if type(tap_pitch) is not int or tap_pitch < 0 or (tap_pitch and rows % tap_pitch):
        raise ValueError("edge tap pitch must be zero or a positive divisor of rows")
    x0, y0, x1, y1 = edge_boundary(cells["dummy_vertical_8t"])
    width, height = x1 - x0, y1 - y0
    slots = rows + (TAP_SLOTS * (rows // tap_pitch) if tap_pitch else 0)
    cell = library.new_cell(dummy_array_name(rows, tap_pitch, mirror_x, mirror_y))
    slot = 0

    def place(base: str, flipped: bool) -> None:
        name = oriented_name(base, flipped != mirror_x, mirror_y)
        physical_slot = slots - 1 - slot if mirror_x else slot
        cell.add(gdstk.Reference(cells[name], origin=(physical_slot * width - x0, -y0)))

    for index in range(rows):
        place("dummy_vertical_8t", bool(index % 2))
        slot += 1
        if tap_pitch and (index + 1) % tap_pitch == 0:
            # A corner cell over each slot of the tap, the second mirrored as
            # the bitcell after it would be.  (Unmirrored, the gate stubs
            # break the 54 nm pitch; mirrored, the row's last one leaves one
            # LIG.S.4-5 against the port-B IO block, which overhangs the
            # column by half a fin pitch.)
            for k in range(TAP_SLOTS):
                place("sram_cell_8t_corner", bool(k % 2))
                slot += 1
    rect(cell, (0, 0, slots * width, height), BOUNDARY)
    return cell


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


def add_supply_climb(
    cell: gdstk.Cell, rail: tuple[float, float], spine_x: float,
    y_direction: int, step_y: float = STRAP_STEP_Y,
) -> None:
    """Stagger an M2->M3->M4->M5 climb, as the IO column's staircase does.

    M4 is horizontal-only and M5 vertical-only in the ASAP7 public rules, so
    the transitions cannot share a track: V2 leaves the supply rail on a
    vertical M3, V3 turns onto a horizontal M4 run, and V4 lands on the
    vertical M5 spine.  The rail itself is full width, so it already provides
    V2's horizontal endcaps and no extra M2 landing is needed.
    """
    x2 = STRAP_CLIMB_X
    y2 = (rail[0] + rail[1]) / 2
    y3 = y2 + y_direction * step_y

    rect(cell, (x2 - 0.009, y2 - 0.009, x2 + 0.009, y2 + 0.009), V2)
    rect(cell, (x2 - 0.009, min(y2, y3) - 0.014,
                x2 + 0.009, max(y2, y3) + 0.014), M3)

    rect(cell, (x2 - 0.014, y3 - 0.012, x2 + 0.014, y3 + 0.012), M3)
    rect(cell, (x2 - 0.009, y3 - 0.012, x2 + 0.009, y3 + 0.012), V3)
    rect(cell, (min(x2 - 0.020, spine_x - 0.023), y3 - 0.012,
                max(x2 + 0.020, spine_x + 0.023), y3 + 0.012), M4)

    rect(cell, (spine_x - 0.012, y3 - 0.012,
                spine_x + 0.012, y3 + 0.012), V4)


def build_tap_library(
    bitcell: gdstk.Cell, unit: float = 1e-6, precision: float = 2.5e-10
) -> gdstk.Library:
    """Build the array well/substrate tap, following tapcell_sram_6t122."""
    lib = gdstk.Library(
        "openfinram_asap7_8t_tap", unit=unit, precision=precision
    )
    for name in (TAP_CELL_NAME, STRAP_CELL_NAME):
        build_tap_cell(lib, name, bitcell, strap=name == STRAP_CELL_NAME)
    return lib


def build_tap_cell(
    lib: gdstk.Library, name: str, bitcell: gdstk.Cell, strap: bool
) -> gdstk.Cell:
    """Build the array tap; `strap` adds the M2-to-M5 supply climb."""
    cell = lib.new_cell(name)
    x0, y0, bx1, y1 = bbox(
        next(p for p in bitcell.polygons if p.layer == BOUNDARY)
    )
    slot = bx1 - x0
    x1 = x0 + TAP_SLOTS * slot

    # Fin and poly grids run through the tap column unchanged, one bitcell
    # frame per slot; the dummy gates are cut at the row-abutment line
    # exactly as tapcell_sram_6t122 does.  Bitlines and supplies pass
    # straight through, so the tap can be inserted anywhere in a row without
    # breaking a bitline.
    for k in range(TAP_SLOTS):
        clone_polygons(bitcell, cell, lambda poly: poly.layer == GATE, dx=k * slot)
    for poly in bitcell.polygons:  # fins and rails run the whole width, overhang and all
        if poly.layer == FIN or is_full_m2_rail(poly) or is_bitline_m4_rail(poly):
            px0, py0, px1, py1 = bbox(poly)
            rect(cell, (px0, py0, px1 + (TAP_SLOTS - 1) * slot, py1), poly.layer)
    rect(cell, (x0, y0 - 0.0085, x1, y0 + 0.0085), GCUT)
    for layer in (SRAMDRC, BOUNDARY):
        rect(cell, (x0, y0, x1, y1), layer)

    # The well runs through; the tie implants stop short of both seams so
    # they meet the neighbours' overhanging selects edge to edge.
    bands = tie_bands(bitcell)
    rails = supply_rails(bitcell)
    well_y0, well_y1, _ = next(b for b in bands if b[2] == "vdd!")
    rect(cell, (x0, well_y0, x1, well_y1), WELL)
    ix0, ix1 = x0 + TAP_IMPLANT_INSET, x1 - TAP_IMPLANT_INSET
    rect(cell, (ix0, well_y0, ix1, well_y1), NSELECT)
    for band_y0, band_y1, net in bands:
        if net == "vss!":
            rect(cell, (ix0, band_y0, ix1, band_y1), PSELECT)

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

    if strap:
        # One full-height M5 spine per supply.  Rows abut exactly at the cell
        # boundary, as the WLA/WLB trunks already do, so the spines join into a
        # continuous vertical rail down the tap column -- the direction the M2
        # rails cannot carry.  Horizontal M6 tying the columns together crosses
        # bitcells and therefore belongs to the array assembler, not this cell.
        for net, spine_x in STRAP_SPINE_X.items():
            # VSS has two rails at equal distance from the well, so pick the
            # lower one explicitly. Both climbs go north, keeping the VSS
            # landing clear of BLBN's relocated y=-36 nm M4 rail.
            rail = min(rails[net], key=lambda r: r[0])
            add_supply_climb(cell, rail, spine_x,
                             1, step_y=0.040 if net == "vss!" else STRAP_STEP_Y)
            rect(cell, (spine_x - STRAP_M5_HALF, y0,
                        spine_x + STRAP_M5_HALF, y1), M5)
            cell.add(gdstk.Label(net, (spine_x, (rail[0] + rail[1]) / 2),
                                 layer=M5, texttype=PIN_TEXTTYPE))
    return cell


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
        _assert([bbox(p) for p in _layer_polygons(cell, layer)] == [TAP_MARKER],
                f"tap layer {layer} marker is not {TAP_MARKER}")
    _assert(not _layer_polygons(cell, SRAMVT),
            "tap must not carry the SRAM-Vt implant")

    fins = sorted(_layer_polygons(cell, FIN), key=lambda p: bbox(p)[1])
    centres = [round((bbox(p)[1] + bbox(p)[3]) / 2, 6) for p in fins]
    _assert(all(round(bbox(p)[3] - bbox(p)[1], 6) == 0.007 for p in fins),
            "tap FIN width is not exactly 7 nm")
    _assert(all(round(right - left, 6) == 0.027
                for left, right in zip(centres, centres[1:])),
            "tap FIN pitch is not exactly 27 nm")
    wanted = sorted(
        (g[0] + k * 0.108, g[1], g[2] + k * 0.108, g[3])
        for k in range(TAP_SLOTS) for g in (GATE_A, GATE_B)
    )
    _assert(sorted(bbox(p) for p in _layer_polygons(cell, GATE)) == wanted,
            "tap does not reuse the bitcell gate columns in every slot")


def verify_tap_topology(cell: gdstk.Cell, bitcell: gdstk.Cell) -> None:
    """The tap ties both polarities, holds no device, and passes bitlines."""
    devices, labels = extract_connectivity(cell, expected_channels=0)
    _assert(not devices, "tap cell must be device-free")

    well = [bbox(p) for p in _layer_polygons(cell, WELL)]
    nsel = [bbox(p) for p in _layer_polygons(cell, NSELECT)]
    psel = sorted(bbox(p) for p in _layer_polygons(cell, PSELECT))
    _assert(len(well) == 1, "tap must have exactly one n-well band")
    _assert(nsel == [(well[0][0] + TAP_IMPLANT_INSET, well[0][1],
                      well[0][2] - TAP_IMPLANT_INSET, well[0][3])],
            "tap n+ implant must span the n-well band, 27 nm in from each seam")
    for box in psel:
        _assert(abs(box[0] - (TAP_MARKER[0] + TAP_IMPLANT_INSET)) < 1e-9
                and abs(box[2] - (TAP_MARKER[2] - TAP_IMPLANT_INSET)) < 1e-9,
                "tap p+ implant must stop 27 nm short of each seam")
        _assert(box[2] - box[0] >= 0.108 - 1e-9, "tap p+ implant is under the 108 nm minimum width")
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
    def rail_predicate(poly):
        return is_full_m2_rail(poly) or is_bitline_m4_rail(poly)
    stretch = (TAP_SLOTS - 1) * (MARKER[2] - MARKER[0])
    expected_rails = sorted(
        (p.layer, round(b[0], 6), round(b[1], 6), round(b[2] + stretch, 6), round(b[3], 6))
        for p in bitcell.polygons if rail_predicate(p) for b in [bbox(p)]
    )
    drawn_rails = sorted(
        (p.layer, round(b[0], 6), round(b[1], 6), round(b[2], 6), round(b[3], 6))
        for p in cell.polygons if rail_predicate(p) for b in [bbox(p)]
    )
    _assert(
        drawn_rails == expected_rails,
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
        _assert(x1 - x0 >= 0.044 - 1e-9,
                "M4 horizontal length is below 44 nm (M4.W.5)")
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
            (0.016, -0.0115, 0.037, 0.0045),
            (0.071, 0.2655, 0.092, 0.2815),
        }
        _assert(wla_lig <= {bbox(poly) for poly in _layer_polygons(cell, LIG)},
                "WLA LIG landings do not preserve 15 nm LISD spacing")
        # Columns are placed mirrored about both placement seams, so a gate
        # contact that reached one would meet its neighbour's and short two
        # wordlines.  Both WLA landings keep 16 nm: 32 nm between neighbours,
        # which also clears LIG.S.4-5 (31 nm between two short LIG edges).
        x_lo, _, x_hi, _ = edge_boundary(cell)
        for x0, _y0, x1, _y1 in wla_lig:
            _assert(x0 - x_lo >= 0.016 - 1e-9 and x_hi - x1 >= 0.016 - 1e-9,
                    "a WLA LIG landing is within 16 nm of a placement seam")
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


def wlb_landing_tracks(cell: gdstk.Cell) -> set[float]:
    """y of every M4 shape that is not a full-width bitline: the WLB landings."""
    return {round((bbox(p)[1] + bbox(p)[3]) / 2, 4) for p in _layer_polygons(cell, M4)
            if not is_bitline_m4_rail(p)}


def verify_gds(path: Path) -> str:
    lib = gdstk.read_gds(str(path))
    cells = {candidate.name: candidate for candidate in lib.cells}
    _assert(set(cells) == {CELL_NAME, VARIANT_B_NAME, VARIANT_B_END_NAME},
            f"{path} must contain all three bitcell masters, found {sorted(cells)}")
    for cell in cells.values():
        verify_rules(cell)
        verify_topology(cell)
    # A column alternates the two, mirrored: their WLB landings must not share
    # a track, or they meet face to face at every other seam.
    tracks = {name: wlb_landing_tracks(cell) for name, cell in cells.items()}
    _assert(tracks[CELL_NAME] == set(WLB_LANDINGS[False]) and tracks[VARIANT_B_NAME] == tracks[VARIANT_B_END_NAME] == set(WLB_LANDINGS[True]),
            f"WLB landings are not on their tracks: {tracks}")
    _assert(not tracks[CELL_NAME] & tracks[VARIANT_B_NAME], "the variants' WLB landings share a track")
    return hashlib.sha256("".join(polygon_fingerprint(cells[n]) for n in sorted(cells)).encode()).hexdigest()


def verify_strap_topology(strap: gdstk.Cell, tap: gdstk.Cell) -> None:
    """The strap is the tap plus a working M2-to-M5 climb per supply."""
    # The strap must keep every tie, rail and process shape the tap has.
    tap_shapes = {(p.layer, p.datatype, bbox(p)) for p in tap.polygons}
    strap_shapes = {(p.layer, p.datatype, bbox(p)) for p in strap.polygons}
    missing = tap_shapes - strap_shapes
    _assert(not missing,
            f"strap drops {len(missing)} tap shapes, e.g. {sorted(missing)[:2]}")

    # One full-height M5 spine per supply, inside the slot and clear of each
    # other.  Rows abut at the boundary, so a spine short of it breaks the
    # vertical rail the strap exists to provide.
    x0, y0, x1, y1 = TAP_MARKER
    spines = sorted(bbox(p) for p in _layer_polygons(strap, M5))
    _assert(len(spines) == len(STRAP_SPINE_X),
            f"expected {len(STRAP_SPINE_X)} M5 spines, found {len(spines)}")
    for spine, (net, centre) in zip(spines, sorted(STRAP_SPINE_X.items(),
                                                   key=lambda kv: kv[1])):
        _assert(abs(spine[0] - (centre - STRAP_M5_HALF)) < 1e-9
                and abs(spine[2] - (centre + STRAP_M5_HALF)) < 1e-9,
                f"{net} M5 spine is not centred on {centre}")
        _assert(abs(spine[1] - y0) < 1e-9 and abs(spine[3] - y1) < 1e-9,
                f"{net} M5 spine does not span the full row pitch")
        _assert(x0 <= spine[0] and spine[2] <= x1,
                f"{net} M5 spine leaves the tap slot")
    _assert(spines[1][0] - spines[0][2] >= 0.018 - 1e-9,
            "the two M5 spines are closer than 18 nm")

    # M3 has to clear the 9 nm neighbour overhang at both slot edges.
    for poly in _layer_polygons(strap, M3):
        box = bbox(poly)
        _assert(box[0] >= TAP_MARKER[0] + 0.027 - 1e-9 and box[2] <= TAP_MARKER[2] - 0.027 + 1e-9,
                f"strap M3 at {box} intrudes on the neighbour overhang")

    # The climb's M4 must not land on a port-B bitline rail.
    bitline_y = [(bbox(p)[1], bbox(p)[3]) for p in _layer_polygons(strap, M4)
                 if is_bitline_m4_rail(p)]
    _assert(len(bitline_y) == 2, "strap lost a port-B bitline M4 rail")
    for poly in _layer_polygons(strap, M4):
        if is_bitline_m4_rail(poly):
            continue
        box = bbox(poly)
        for low, high in bitline_y:
            _assert(box[3] <= low or high <= box[1],
                    f"strap M4 climb at {box} overlaps a bitline rail")

    # Connectivity: collapsing the supply labels onto one component per climb
    # is what proves the staircase actually reaches the spine.  VSS keeps two
    # components because the tap never joined its two rails through metal.
    _devices, labels = extract_connectivity(strap, expected_channels=0)
    _assert(len(labels["vdd!"]) == 1,
            "strap VDD rail and M5 spine are not connected")
    _assert(len(labels["vss!"]) == 2,
            "strap VSS rail and M5 spine are not connected")
    _assert(labels["vdd!"].isdisjoint(labels["vss!"]),
            "strap shorts VDD to VSS")


def verify_tap_gds(path: Path, bitcell_path: Path) -> dict[str, str]:
    lib = gdstk.read_gds(str(path))
    cells = {cell.name: cell for cell in lib.cells}
    expected = {TAP_CELL_NAME, STRAP_CELL_NAME}
    _assert(set(cells) == expected,
            f"tap library cells are {sorted(cells)}, expected {sorted(expected)}")
    bit_lib = gdstk.read_gds(str(bitcell_path))
    bitcell = next((c for c in bit_lib.cells if c.name == CELL_NAME), None)
    if bitcell is None:
        raise ValueError(f"{CELL_NAME!r} not found in {bitcell_path}")
    for name in sorted(expected):
        verify_tap_rules(cells[name])
        verify_tap_topology(cells[name], bitcell)
    verify_strap_topology(cells[STRAP_CELL_NAME], cells[TAP_CELL_NAME])
    return {name: polygon_fingerprint(cells[name]) for name in sorted(expected)}


def verify_edge_gds(path: Path, row_counts: tuple[int, ...] = (64,)) -> dict[str, str]:
    lib = gdstk.read_gds(str(path))
    cells = {cell.name: cell for cell in lib.cells}
    expected = set(EDGE_CELL_NAMES) | {
        dummy_array_name(count, mirror_x=mx, mirror_y=my)
        for count in row_counts for mx, my in ORIENTATIONS
    }
    _assert(set(cells) == expected,
            f"edge library cells are {sorted(cells)}, expected {sorted(expected)}")

    for name in CORE_EDGE_CELL_NAMES:
        verify_rules(cells[name])
    verify_dummy_topology(cells["dummy_cell_8t"])
    verify_cap_topology(cells["sram_cell_8t_col_cap"], "col")
    verify_cap_topology(cells["sram_cell_8t_row_cap"], "row")
    verify_cap_topology(cells["sram_cell_8t_corner"], "corner")

    frames = {name: process_fingerprint(cells[name]) for name in CORE_EDGE_CELL_NAMES}
    _assert(len(set(frames.values())) == 1,
            "dummy/cap process frames do not match at abutment boundaries")

    col_cap = cells["sram_cell_8t_col_cap"]
    row_cap = cells["sram_cell_8t_row_cap"]
    corner = cells["sram_cell_8t_corner"]
    def rail_predicate(poly):
        return is_full_m2_rail(poly) or is_bitline_m4_rail(poly)
    _assert(
        filtered_polygon_fingerprint(col_cap, rail_predicate)
        == filtered_polygon_fingerprint(corner, rail_predicate),
        "column and corner cap rails do not abut identically",
    )

    conductor_layers = {LIG, LISD, V0, M1, V1, M2, V2, M3, V3, M4, V4, M5}
    def wordline_predicate(poly):
        return poly.layer in conductor_layers and not rail_predicate(poly)
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
        (-0.038, -0.048, 0.135, -0.024),
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

    # The masters mirrored in x are built from variant B (WLB_LANDINGS): their
    # canonical is the x-mirrored master restored, which must keep the row
    # cap's and corner's routes identical as the plain ones do, and take
    # the variant's landings.
    row_cap_b = oriented_cell(cells[oriented_name("dummy_vertical_8t", True, False)], row_cap.name, True, False)
    corner_b = oriented_cell(cells[oriented_name("sram_cell_8t_corner", True, False)], corner.name, True, False)
    _assert(
        filtered_polygon_fingerprint(row_cap_b, wordline_predicate)
        == filtered_polygon_fingerprint(corner_b, wordline_predicate),
        "variant row and corner cap wordline routes do not abut identically",
    )
    _assert(wlb_landing_tracks(row_cap) == set(WLB_LANDINGS[False])
            and wlb_landing_tracks(row_cap_b) == set(WLB_LANDINGS[True]),
            "the x-mirrored edge masters do not take variant B's WLB landings")
    for mx, my in ORIENTATIONS:
        for name, kind, canonical in (
            (oriented_name("dummy_vertical_8t", mx, my), "row", row_cap_b if mx else row_cap),
            (oriented_name("sram_cell_8t_corner", mx, my), "corner", corner_b if mx else corner),
            (topbot_name(mx, my), "col", col_cap),
        ):
            cell = cells[name]
            _assert(not cell.references, f"{name}: oriented master must be materialized")
            restored = oriented_cell(cell, canonical.name, mx, my)
            _assert(polygon_fingerprint(restored) == polygon_fingerprint(canonical),
                    f"{name}: geometry/pins do not match the canonical orientation")
            verify_rules(restored)
            verify_cap_topology(cell, kind)

    blank = cells["FILLER_BLANK_8t"]
    width, height = MARKER[2] - MARKER[0], MARKER[3] - MARKER[1]
    _assert(edge_boundary(blank) == (0, MARKER[1], width / 2, MARKER[3]),
            "blank filler is not a half-width 8T tile")
    _assert({p.layer for p in blank.polygons} == {FIN, GATE, BOUNDARY}
            and not blank.labels and not blank.references,
            "blank filler contains unexpected devices, conductors, or hierarchy")
    _assert(len(_layer_polygons(blank, GATE)) == 1, "blank filler needs one gate column")
    _assert([bbox(p)[1::2] for p in _layer_polygons(blank, FIN)]
            == [bbox(p)[1::2] for p in _layer_polygons(col_cap, FIN)],
            "blank filler FIN grid does not match 8T")
    filler = cells["FILLER_cgedge_8t"]
    _assert(edge_boundary(filler) == (0, 0, width, round(4 * height, 7)),
            "column-group filler is not four 8T rows high")
    _assert(len(filler.references) == 8, "column-group filler needs eight half-width tiles")
    for index, ref in enumerate(filler.references):
        row, col = divmod(index, 2)
        expected_y = (row + 1) * height + MARKER[1] if row % 2 else row * height - MARKER[1]
        _assert(ref.cell_name == blank.name and not ref.rotation
                and bool(ref.x_reflection) == bool(row % 2)
                and all(abs(a - b) < 1e-7 for a, b in zip(ref.origin, (col * width / 2, expected_y))),
                "column-group filler has a misplaced blank tile")
    for count in row_counts:
        for mx, my in ORIENTATIONS:
            row = cells[dummy_array_name(count, mirror_x=mx, mirror_y=my)]
            _assert(edge_boundary(row) == (0, 0, round(count * width, 7), round(height, 7)),
                    f"{row.name}: wrong row-count boundary")
            _assert(len(row.references) == count, f"{row.name}: wrong dummy count")
            for index, ref in enumerate(row.references):
                slot = count - 1 - index if mx else index
                _assert(ref.cell_name == oriented_name("dummy_vertical_8t", bool(index % 2) != mx, my)
                        and not ref.rotation and not ref.x_reflection
                        and all(abs(a - b) < 1e-7 for a, b in zip(ref.origin, (slot * width - MARKER[0], -MARKER[1]))),
                        f"{row.name}: wrong dummy slot/orientation")
    return {name: polygon_fingerprint(cell.copy(name + "_flat").flatten())
            for name, cell in sorted(cells.items())}


def parse_row_counts(value: str) -> tuple[int, ...]:
    try:
        counts = tuple(sorted({int(item) for item in value.split(",")}))
    except ValueError as error:
        raise argparse.ArgumentTypeError("edge row counts must be integers") from error
    if not counts or any(count < 1 for count in counts):
        raise argparse.ArgumentTypeError("edge row counts must be positive")
    return counts


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
        "--edge-rows", type=parse_row_counts, default=(64,),
        help="comma-separated dummy-array address row counts (default: 64)",
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
            digests = verify_edge_gds(args.verify_edges, args.edge_rows)
            print(f"PASS {args.verify_edges}: {len(digests)} edge cells")
            for name, digest in digests.items():
                print(f"  {name}: sha256={digest}")
            return 0

        if args.verify_tap:
            bitcell_path = args.output
            digests = verify_tap_gds(args.verify_tap, bitcell_path)
            print(f"PASS {args.verify_tap}: {len(digests)} tap cells")
            for name, digest in digests.items():
                print(f"  {name}: sha256={digest}")
            return 0

        lib = build_cell(args.source_gds)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        lib.write_gds(str(args.output), timestamp=FIXED_GDS_TIMESTAMP)
        digest = verify_gds(args.output)
        print(f"wrote {args.output}: {CELL_NAME}, topology=8T, sha256={digest}")

        edge_output = args.edge_output or args.output.with_name("sram_cell_8t_edges.gds")
        edge_output.parent.mkdir(parents=True, exist_ok=True)
        edge_lib = build_edge_library(
            lib.cells[0], unit=lib.unit, precision=lib.precision, row_counts=args.edge_rows
        )
        edge_lib.write_gds(str(edge_output), timestamp=FIXED_GDS_TIMESTAMP)
        edge_digests = verify_edge_gds(edge_output, args.edge_rows)
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
        tap_digests = verify_tap_gds(tap_output, args.output)
        print(f"wrote {tap_output}: {len(tap_digests)} tap cells")
        for name, digest in tap_digests.items():
            print(f"  {name}: sha256={digest}")
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
