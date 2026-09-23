#!/usr/bin/env python3
"""Assemble and route the 2RW macro from real ASAP7 hard-cell geometry.

Every disconnected supply component is a separate routing terminal.  Physical
pin connectivity is extracted again after DEF stream-out before publishing GDS.
The controller is the already placed/routed ctrl_decode from this compiler run.

A column is one unsplit array with port A's IO at one end of the bitlines and
port B's at the other.  ``--wordlines`` is still NUM_WL, the rows one row-select
address field covers; the array has twice that, the address bit above the
column select being the top bit of its wordline index.

The data bits are two stacks of abutted column tiles with the controller band
between them.  Each stack's wordlines run through its tiles by abutment and
are driven at its edge by a pair of driver strips (chipforge_asap7's
DriverSliceSpec, four wordlines a slice): port A's strip against the array,
port B's flipped under it sharing a VSS rail and reaching the M5 wordlines
through a via stack and strap.  Those wordline nets are never routed: the
connectivity gate proves them through the abutments instead.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import functools
import json
import math
from pathlib import Path
import re
import subprocess

import gdstk
from asap7_connectivity import MetalGraph, METALS
import generate_asap7_wordline_arrays as arrays
import generate_asap7_8t_iocolumn as columns
import generate_asap7_8t_bitcell as edge_cells
import generate_asap7_8t_wl_slices as wl_slices

REPO = Path(__file__).resolve().parents[1]
LAYER_NAMES = dict(zip(METALS, (f"M{i}" for i in range(1, 10))))
#: A driver slice: four wordlines at the array's pitch, its outputs 54 nm in.
SLICE_WIDTH, WORDLINE_PITCH = 0.432, 0.108
#: The array centres a fin on its edge, the slices a fin space on theirs: this
#: far apart both fin grids are on pitch.
FIN_HALF_PITCH = 0.0135
WORDLINE = re.compile(r"WL_?([AB])\[(\d+)\]")


def supply(text):
    name = text.lower().rstrip("!")
    return name if name in ("vdd", "vss") else None


def probe_point(polygon, offset):
    """Choose an interior point that survives GDS grid conversion."""
    inset = gdstk.offset([polygon], -0.0005, precision=1e-7)
    if not inset:
        raise RuntimeError("conductor too narrow for a physical connectivity probe")
    return tuple(float(v) - o for v, o in zip(inset[0].points[0], offset))


def bbox_origin(cell):
    """Where `abstract` puts the cell: its bounding box's corner, on the nm grid."""
    low, _ = cell.bounding_box()
    return tuple(math.floor(float(v) * 1000) / 1000 for v in low)


def abstract(cell, name, signal_labels, abutted=None):
    """Create an exact metal abstract and a pin for every named component.

    Nets `abutted` names are joined by abutment, never routed: they are
    terminals for the connectivity gate but obstructions to the router.
    """
    graph = MetalGraph(cell)
    roots = {}
    for label in signal_labels:
        root = graph.label_root(label)
        if root in roots and roots[root] != label.text:
            raise RuntimeError(f"{name}: short {roots[root]} / {label.text}")
        roots[root] = label.text
    for label in cell.get_labels():
        net = supply(label.text)
        if net is None or label.layer not in METALS:
            continue
        root = graph.label_root(label)
        if root in roots and roots[root] != net:
            raise RuntimeError(f"{name}: short {roots[root]} / {net}")
        roots[root] = net
    if not {"vdd", "vss"}.issubset(roots.values()):
        raise RuntimeError(f"{name}: missing supply conductors")

    # Retain overhangs inside the abstract boundary; never hide device geometry
    # from the router by using only the smaller SRAM abutment rectangle.
    low, high = cell.bounding_box()
    ox, oy = (math.floor(float(v) * 1000) / 1000 for v in low)
    width, height = (
        math.ceil((float(v) - o) * 1000) / 1000 for v, o in zip(high, (ox, oy))
    )
    master = gdstk.Cell(name)
    master.add(gdstk.Reference(cell, origin=(-ox, -oy)))
    groups = defaultdict(list)
    for i, polygon in enumerate(graph.polygons):
        if polygon.layer in METALS:
            groups[graph.root(i)].append(polygon)
    terminals = {}
    obstacles = [
        p for root, polys in groups.items() if root not in roots for p in polys
    ]
    lef = [
        f"MACRO {name}",
        "  CLASS BLOCK ;",
        "  ORIGIN 0 0 ;",
        f"  SIZE {width:.6f} BY {height:.6f} ;",
        "  SYMMETRY X Y ;",
    ]

    def geometry(polygons):
        out = []
        by_layer = defaultdict(list)
        for poly in polygons:
            by_layer[poly.layer].append(poly)
        for layer, polys in sorted(by_layer.items()):
            out.append(f"    LAYER {LAYER_NAMES[layer]} ;")
            for poly in gdstk.boolean(polys, [], "or", precision=1e-6):
                points = " ".join(f"{x - ox:.6f} {y - oy:.6f}" for x, y in poly.points)
                out.append(f"    POLYGON {points} ;")
        return out

    for root, net in sorted(roots.items(), key=lambda item: (item[1], item[0])):
        pin = f"P{len(terminals)}"
        polys = groups[root]
        if abutted is not None and abutted(net):
            obstacles.extend(polys)
            polygon = max(polys, key=lambda p: p.area())
            terminals[pin] = {
                "net": net,
                "layer": polygon.layer,
                "point": probe_point(polygon, (ox, oy)),
                "abutted": True,
            }
            continue
        if name == "dp_controller" and net not in ("vdd", "vss"):
            labels = [label for label in signal_labels if label.text == net]
            access_layers = {label.layer for label in labels}
            # Include the connected landing wire, not just its pin marker:
            # marking its immediately adjacent same-net extension as OBS
            # creates artificial spacing violations during via access.
            access = [p for p in polys if p.layer in access_layers]
            if not access:
                raise RuntimeError(
                    f"{name}: no explicit physical pin landing for {net}"
                )
            obstacles.extend(p for p in polys if p.layer not in access_layers)
        elif net in ("vdd", "vss"):
            # A supply component is reachable on every layer it has: the
            # router lands on a rail or a bar, not on the top stub of a via
            # stack squeezed between the wordlines.
            access = polys
        else:
            access_layer = max(p.layer for p in polys)
            access = [p for p in polys if p.layer == access_layer]
            obstacles.extend(p for p in polys if p.layer != access_layer)
        # Signal use is intentional: even the supply trees must be physically
        # routed here, not collapsed into unconnected global/special nets.
        direction = (
            "OUTPUT"
            if (
                net.startswith(("QA", "QB"))
                or (
                    name == "dp_controller"
                    and net.startswith(
                        ("sel_", "wrena", "ysel", "sae", "oe_out", "oeb_out", "blprech")
                    )
                )
            )
            else "INPUT"
        )
        if net in ("vdd", "vss"):
            direction = "INOUT"
        lef += [
            f"  PIN {pin}",
            f"    DIRECTION {direction} ;",
            "    USE SIGNAL ;",
            "    PORT",
        ]
        lef += geometry(access)
        lef += ["    END", f"  END {pin}"]
        polygon = max(access, key=lambda p: p.area())
        point = probe_point(polygon, (ox, oy))
        terminals[pin] = {"net": net, "layer": polygon.layer, "point": point}
    lef += ["  OBS"]
    lef += geometry(obstacles)
    lef += ["  END", f"END {name}"]
    # Freeze the partition of unnamed internal interconnect too. A route that
    # shorts into a storage node or a controller internal net must fail even
    # if all exported pins still appear mutually isolated.
    for root, polys in groups.items():
        if root in roots:
            continue
        polygon = max(polys, key=lambda p: p.area())
        point = probe_point(polygon, (ox, oy))
        terminals[f"internal_{root}"] = {
            "net": f"internal_{root}",
            "layer": polygon.layer,
            "point": point,
            "private": True,
        }
    # The GDS boundary is the cell's own placement boundary where it has one,
    # so abutted blocks show one clean outline; the LEF SIZE above stays the
    # bounding box, overhangs included, which is what the router must see.
    own = [
        p.bounding_box() for p in cell.polygons if p.layer == 100 and p.datatype == 0
    ]
    if own:
        (bx0, by0), (bx1, by1) = max(
            own, key=lambda b: (b[1][0] - b[0][0]) * (b[1][1] - b[0][1])
        )
        master.add(
            gdstk.rectangle((bx0 - ox, by0 - oy), (bx1 - ox, by1 - oy), layer=100)
        )
    else:
        master.add(gdstk.rectangle((0, 0), (width, height), layer=100))
    return master, terminals, "\n".join(lef), (width, height)


def half_of(bit, bits):
    """Which stack a data bit is in: `lo` below the controller band, `hi` above."""
    return "lo" if bit < bits // 2 else "hi"


def leaf_net(name, bank, bit, wordlines, bits):
    """Macro net of a column pin; `wordlines` is NUM_WL, half the array's."""
    if name in ("vdd", "vss"):
        return name
    if name in ("DA", "DB", "QA", "QB"):
        return f"{name[0]}_{name[1]}[{bit}]"
    wl = WORDLINE.fullmatch(name)
    if wl:
        return f"wl_{wl[1]}_{half_of(bit, bits)}[{bank * 2 * wordlines + int(wl[2])}]"
    mux = re.fullmatch(r"(yseln|ysel)([AB])\[(\d+)\]", name)
    if mux:
        return f"{mux[1]}_{mux[2]}[{bank * 4 + int(mux[3])}]"
    ctrl = re.fullmatch(r"(wrena|wrenan|oeb_out|oe_out|blprechn)([AB])", name)
    if ctrl:
        return f"{ctrl[1]}_{ctrl[2]}[{bank}]"
    if name in ("sae_A", "sae_B"):
        return f"{name}[{bank}]"
    raise RuntimeError(f"unmapped column pin {name}")


def strip_net(name, bank, half, wordlines):
    """Macro net of a strip pair pin: the controller's predecode lines in, wordlines out."""
    if name in ("vdd", "vss"):
        return name
    sel = re.fullmatch(r"SEL_([AB])\[(\d+)\]", name)
    if sel:
        return f"sel_hi_{sel[1]}[{bank * (2 * wordlines // 4) + int(sel[2])}]"
    low = re.fullmatch(r"B_([AB])\[(\d+)\]", name)
    if low:
        return f"sel_lo_{low[1]}[{low[2]}]"
    wl = WORDLINE.fullmatch(name)
    if wl:
        return f"wl_{wl[1]}_{half}[{bank * 2 * wordlines + int(wl[2])}]"
    raise RuntimeError(f"unmapped strip pin {name}")


def outer_boundary(cell):
    """The cell's boundary at any depth: a ladder wrapper's is its spec cell's."""
    boxes = [
        poly.bounding_box()
        for poly in cell.get_polygons(depth=None)
        if poly.layer == 100 and poly.datatype == 0
    ]
    if not boxes:
        raise RuntimeError(f"{cell.name}: no BOUNDARY polygon")
    (x0, y0), (x1, y1) = max(
        boxes, key=lambda b: (b[1][0] - b[0][0]) * (b[1][1] - b[0][1])
    )
    return tuple(round(float(v), 7) for v in (x0, y0, x1, y1))


def wordline_tracks(cell):
    """x of every wordline of a column tile, by port and index, from its labels."""
    tracks = {}
    for label in cell.labels:
        wl = WORDLINE.fullmatch(label.text)
        if wl:
            tracks[(wl[1], int(wl[2]))] = round(float(label.origin[0]), 4)
    return tracks


def build_wordline_strips(lib, leaf, wordlines, bits):
    """The strip pair for each side of the controller band, in the tile's x.

    A strip is one slice per four of the tile's port-A wordlines, the slice's
    outputs on them, with a filler wherever a slice has no neighbour (the
    array's tap columns and the row's ends).  The pair puts port A's strip
    against the array and port B's flipped under it on a shared VSS rail;
    port B's outputs climb to the tile's M5 wordlines on a VIA34/VIA45 stack
    and an M5 strap.  Both strips sit half a fin pitch in from the pair's
    edges so their fins and the array's are on one grid, port A's outputs
    bridged to the edge on M3.  Returns ``{"lo": cell, "hi": cell}`` and the
    load class the slices were sized to.
    """
    cells_along = 4 * (bits // 2)
    load = wl_slices.load_class(cells_along)
    ladder = {
        c.name: c
        for c in gdstk.read_gds(str(REPO / "tech/gds/sram_8t_wl_slices.gds")).cells
    }
    slice_cell, filler = (
        ladder[wl_slices.slice_name(load)],
        ladder[wl_slices.slice_name(load) + "_filler"],
    )
    have = {c.name for c in lib.cells}
    for cell in (slice_cell, filler):
        for dep in (cell, *cell.dependencies(True)):
            if dep.name not in have:
                lib.add(dep)
                have.add(dep.name)
    rows = 2 * wordlines
    if rows % 4:
        raise RuntimeError(
            "the array's wordlines must be a multiple of four, one driver slice each"
        )
    tracks = wordline_tracks(leaf)
    if len(tracks) != 2 * rows:
        raise RuntimeError(
            f"expected {2 * rows} wordline labels on the tile, found {len(tracks)}"
        )
    height = outer_boundary(slice_cell)[3]
    pins = {label.text: (float(label.origin[0]), float(label.origin[1]), label.layer)
            for label in slice_cell.labels}  # fmt: skip

    strip = lib.new_cell(f"wl_strip_c{load}_x{rows // 4}")
    starts = []
    for k in range(rows // 4):
        x0 = tracks[("A", 4 * k)] - WORDLINE_PITCH / 2
        for j in range(4):
            want, got = (
                tracks[("A", 4 * k + j)],
                x0 + WORDLINE_PITCH / 2 + WORDLINE_PITCH * j,
            )
            if abs(want - got) > 1e-6:
                raise RuntimeError(
                    f"slice {k}: WL{4 * k + j} is at {want}, the slice's output at {got}"
                )
        strip.add(gdstk.Reference(slice_cell, (x0, 0)))
        starts.append(x0)
        sx, sy, layer = pins["SEL"]
        strip.add(gdstk.Label(f"SEL[{k}]", (x0 + sx, sy), layer=layer, texttype=251))
        for j in range(4):
            x, y, layer = pins[f"WL{j}"]
            strip.add(
                gdstk.Label(f"WL[{4 * k + j}]", (x0 + x, y), layer=layer, texttype=251)
            )
    # Every slice's B<j> is its own metal: the slices' predecode inputs do
    # not join across a slice boundary, so each is a pin and the router ties
    # them to the controller's sel_lo<j>.
    for x0_slice in starts:
        for j in range(4):
            x, y, layer = pins[f"B{j}"]
            strip.add(
                gdstk.Label(f"B[{j}]", (x0_slice + x, y), layer=layer, texttype=251)
            )
    spans = [(x0, x0 + SLICE_WIDTH) for x0 in starts]
    gaps = [
        (spans[0][0] - WORDLINE_PITCH, spans[0][0]),
        (spans[-1][1], spans[-1][1] + WORDLINE_PITCH),
    ]
    gaps += [
        (left[1], right[0])
        for left, right in zip(spans, spans[1:])
        if right[0] - left[1] > 1e-6
    ]
    for g0, g1 in gaps:
        count = round((g1 - g0) / WORDLINE_PITCH)
        if abs((g1 - g0) - count * WORDLINE_PITCH) > 1e-6:
            raise RuntimeError(
                f"a {round((g1 - g0) * 1000)} nm gap between slices is not whole fillers"
            )
        for c in range(count):
            strip.add(gdstk.Reference(filler, (g0 + c * WORDLINE_PITCH, 0)))
    x_left, x_right = spans[0][0] - WORDLINE_PITCH, spans[-1][1] + WORDLINE_PITCH

    # OpenROAD's ASAP7 default via stacks, drawn as cells so the pair is one place to look.
    via34 = lib.new_cell("wl_via34")
    via34.add(gdstk.rectangle((-0.020, -0.012), (0.020, 0.012), layer=40),
              gdstk.rectangle((-0.009, -0.017), (0.009, 0.017), layer=30),
              gdstk.rectangle((-0.009, -0.012), (0.009, 0.012), layer=35))  # fmt: skip
    via45 = lib.new_cell("wl_via45")
    via45.add(gdstk.rectangle((-0.012, -0.023), (0.012, 0.023), layer=50),
              gdstk.rectangle((-0.023, -0.012), (0.023, 0.012), layer=40),
              gdstk.rectangle((-0.012, -0.012), (0.012, 0.012), layer=45))  # fmt: skip

    # The strip's gate tracks (slices and fillers share the array's 54 nm
    # grid).  The array's gate stubs reach 7 or 19 nm past its boundary and
    # the strip's 5 nm past its own, so the fin-grid margin would leave a
    # short gap on a track; bridge each track across the margin, under the
    # slice's gate cut, so the abutment reads as one poly line.
    gate_tracks = sorted(
        {
            round(
                (float(poly.bounding_box()[0][0]) + float(poly.bounding_box()[1][0]))
                / 2,
                4,
            )
            for poly in strip.get_polygons(depth=None)
            if poly.layer == 7
        }
    )
    pairs = {}
    pair_height = 2 * height + 2 * FIN_HALF_PITCH
    seam = height + FIN_HALF_PITCH  # the shared VSS rail
    for half, facing_up in (("lo", False), ("hi", True)):
        pair = lib.new_cell(f"wl_strips_{half}_c{load}_x{rows // 4}")

        def at(y):  # the `hi` pair is drawn; the `lo` pair is its mirror image
            return y if facing_up else pair_height - y

        def rect(x0, y0, x1, y1, layer):
            ys = sorted((at(y0), at(y1)))
            pair.add(gdstk.rectangle((x0, ys[0]), (x1, ys[1]), layer=layer))

        def label(text, x, y, layer):
            pair.add(gdstk.Label(text, (x, at(y)), layer=layer, texttype=251))

        for port, flipped in (("A", False), ("B", True)):
            pair.add(
                gdstk.Reference(
                    strip, (0, at(seam)), x_reflection=flipped != (not facing_up)
                )
            )
            sign = -1 if flipped else 1
            for k in range(rows // 4):
                sx, sy, layer = pins["SEL"]
                label(f"SEL_{port}[{k}]", starts[k] + sx, seam + sign * sy, layer)
            for x0_slice in starts:  # every slice's, as in the strip
                for j in range(4):
                    x, y, layer = pins[f"B{j}"]
                    label(f"B_{port}[{j}]", x0_slice + x, seam + sign * y, layer)
        for i in range(rows):
            x, x5 = tracks[("A", i)], tracks[("B", i)]
            rect(x - 0.009, seam + height, x + 0.009, pair_height, 30)
            label(f"WL_A[{i}]", x, pair_height - 0.005, 30)
            y_via = seam - height + 0.025
            for cell, vx in ((via34, x), (via45, x5)):
                pair.add(gdstk.Reference(cell, (vx, at(y_via))))
            rect(x5 - 0.012, y_via - 0.023, x5 + 0.012, pair_height, 50)
            label(f"WL_B[{i}]", x5, pair_height - 0.005, 50)
        for x in gate_tracks:
            rect(x - 0.010, seam + height, x + 0.010, pair_height, 7)
        pair.add(gdstk.rectangle((x_left, 0), (x_right, pair_height), layer=100))
        pairs[half] = pair
    return pairs, load


def ends_tag(bottom, top):
    return {
        (True, True): "",
        (True, False): "_endb",
        (False, True): "_endt",
        (False, False): "_noend",
    }[(bottom, top)]


def build_leaf(wordlines, tap_pitch, bottom=True, top=True):
    """``iocol A | cap | array of `wordlines` | iocol B``, with a dummy row below and/or above.

    In a stack of abutted tiles only the stack's two ends need a dummy row
    (its outer end and the end facing its driver strip); the tiles between
    abut array row to array row, their IO blocks rail to rail.
    """
    gds = REPO / "tech/gds"
    lib = arrays.build_library(
        gds / "sram_cell_8t.gds",
        gds / "srambank_32b_boundary_2.gds",
        [wordlines],
        4,
        gds / "sram_cell_8t_tap.gds",
        tap_pitch,
    )
    cells = {c.name: c for c in lib.cells}
    edges = {
        c.name: c for c in gdstk.read_gds(str(gds / "sram_cell_8t_edges.gds")).cells
    }
    # Each port's IO is the parametric column block, built to this bitcell.
    specs = columns.io_block_specs(cells["sram_cell_8t"])
    blocks, _ = columns.build_io_blocks(specs)
    for block in blocks.values():
        for dep in (block, *block.dependencies(True)):
            if dep not in lib.cells:
                lib.add(dep)
    io_a = columns.build_port_io(lib, "A", blocks["A"], specs["A"])
    io_b = columns.build_port_io(lib, "B", blocks["B"], specs["B"])
    caps = columns.build_cap_array(lib, edges)
    array = cells[f"array_x{wordlines}x4_tap{tap_pitch}_sram_8t"]
    leaf = columns.build_colgrp(lib, array, io_a, io_b, caps, wordlines)
    capped = lib.new_cell("capped_" + leaf.name + ends_tag(bottom, top))
    capped.add(gdstk.Reference(leaf))
    for label in leaf.labels:
        capped.add(columns.clone_label(label, label.text, tuple(label.origin)))
    width = columns.boundary_box(leaf)[2]
    bx0, by0, bx1, by1 = columns.boundary_box(cells["sram_cell_8t"])
    slot_width, pitch = bx1 - bx0, by1 - by0
    io_a_width = columns.boundary_box(io_a)[2]
    cap_width = columns.boundary_box(caps)[2]
    blank = edges["FILLER_BLANK_8t"]
    fx0, fy0, fx1, fy1 = columns.boundary_box(blank)
    half_width = fx1 - fx0
    ends = lib.new_cell("dp_array_end_rows" + ends_tag(bottom, top))
    # The array has one capped end, port A's; a dummy row runs above and below
    # it from there, with a corner over the cap and blanks over the filler.
    # Port B's end meets its IO on the array's last tap, as an IO face always
    # has, and the dummy rows simply stop there.
    array_x = io_a_width + cap_width
    corner_x = array_x - slot_width
    filler_x = io_a_width
    for bottom in [b for b, wanted in ((False, top), (True, bottom)) if wanted]:
        y = -pitch if bottom else 4 * pitch
        row = edge_cells.build_dummy_vertical_array(
            lib, edges, wordlines, tap_pitch, mirror_x=False, mirror_y=bottom
        )
        ends.add(gdstk.Reference(row, origin=(array_x, y)))
        corner = edges[edge_cells.oriented_name("sram_cell_8t_corner", False, bottom)]
        ends.add(gdstk.Reference(corner, origin=(corner_x - bx0, y - by0)))
        for col in range(2):
            ends.add(
                gdstk.Reference(
                    blank,
                    origin=(
                        filler_x + col * half_width - fx0,
                        y + fy1 if bottom else y - fy0,
                    ),
                    x_reflection=bottom,
                )
            )
    if ends.references:
        capped.add(gdstk.Reference(ends))
        tie_end_row_stubs(capped)
    columns.rect(
        capped, (0, -pitch if bottom else 0, width, (5 if top else 4) * pitch), 100
    )
    return capped


def tie_end_row_stubs(cell):
    """Join each dummy row's stray supply stubs to the row's own supply metal.

    The corner and tap-slot cells of the dummy rows keep bitline and supply
    stubs at the real cells' heights, labelled VSS or VDD but touching
    nothing: twenty 162 nm bars a tile, on M2 and M4, that the router would
    otherwise have to reach one by one between the M5 wordlines, which it
    manages only at some track phases.  Stubs of one net at one height are
    first bridged along the row on M2.  Then a VSS stub on M2 takes a strap
    up the column to the row's VSS bar; an M4 stub, which cannot be strapped
    on M4 (one direction only), takes a V3 onto the cell's own VSS via
    stack, or an M3 jog to it; and the VDD bar, whose nearest VDD is the
    neighbouring real row's M2 bar across the dummy row's VSS bars, takes a
    V2, an M3 jog on a free track and a V2 down.  The tile then presents
    rails.
    """
    HALF, HALF3, CAP3 = 0.009, 0.009, 0.014
    graph, net_of_root, members, m3, m2 = _supply_graph(cell)

    def is_stub(polys):
        """A lone bar on one layer: one track tall, however many cells long."""
        layers = {p.layer for p in polys}
        if len(layers) != 1 or next(iter(layers)) not in (20, 40):
            return False
        y0 = min(p.bounding_box()[0][1] for p in polys)
        y1 = max(p.bounding_box()[1][1] for p in polys)
        return y1 - y0 < 0.03

    # Bridge: same net, M2, same height, nothing on M2 in between.
    stubs = {root: polys for root, polys in members.items() if is_stub(polys)}
    groups = defaultdict(list)
    for root, polys in stubs.items():
        x0 = min(p.bounding_box()[0][0] for p in polys)
        x1 = max(p.bounding_box()[1][0] for p in polys)
        (_, y0), (_, y1) = polys[0].bounding_box()
        if polys[0].layer == 20:
            groups[(net_of_root[root], round((y0 + y1) / 2, 3))].append(
                (x0, x1, y0, y1)
            )
    for (net, _), bars in groups.items():
        bars.sort()
        for (_, ax1, y0, y1), (bx0, _, _, _) in zip(bars, bars[1:]):
            if any(
                bb[0][0] < bx0
                and bb[1][0] > ax1
                and bb[0][1] < y1 + 0.018
                and bb[1][1] > y0 - 0.018
                for bb, root in m2
                if net_of_root.get(root) != net
            ):
                continue
            cell.add(gdstk.rectangle((ax1, y0), (bx0, y1), layer=20))

    # Tie: everything single-layer and short is a stub, before or after bridging.
    graph, net_of_root, members, m3, m2 = _supply_graph(cell)

    def m3_is_free(x, y0, y1, own):
        # M3 keeps 18 nm from a long edge and 25 nm from a short one (M3.S.1, .2).
        for ((ax, ay), (bx, by)), root in m3:
            if root in own:
                continue
            clear = 0.018 if by - ay > 0.036 else 0.025
            if (
                ax < x + HALF3 + clear
                and bx > x - HALF3 - clear
                and ay < y1 + clear
                and by > y0 - clear
            ):
                return False
        return True

    def m2_is_free(x0, y0, x1, y1, own):
        return not any(
            bb[0][0] < x1 + 0.018
            and bb[1][0] > x0 - 0.018
            and bb[0][1] < y1 + 0.018
            and bb[1][1] > y0 - 0.018
            for bb, root in m2
            if root not in own
        )

    def nearest(box, net, layer, reach, exclude, min_overlap):
        """Closest same-net polygon on `layer` sharing `min_overlap` of x: (gap, root, bbox of its run)."""
        (sx0, sy0), (sx1, sy1) = box
        best = None
        for root, polys in members.items():
            if net_of_root[root] != net or root == exclude:
                continue
            for polygon in polys:
                if polygon.layer != layer:
                    continue
                (ox0, oy0), (ox1, oy1) = polygon.bounding_box()
                gap = max(oy0 - sy1, sy0 - oy1)
                if min(sx1, ox1) - max(sx0, ox0) >= min_overlap - 1e-6 and gap < reach:
                    if best is None or gap < best[0]:
                        best = (gap, root, ((ox0, oy0), (ox1, oy1)))
        if best is None:
            return None
        # A row's bar is a chain of overlapping cell-wide pieces: take the
        # whole run at that height, so the jog can go anywhere along it.
        gap, root, ((ox0, oy0), (ox1, oy1)) = best
        for polygon in members[root]:
            (px0, py0), (px1, py1) = polygon.bounding_box()
            if (
                polygon.layer == layer
                and abs(py0 - oy0) < 0.003
                and abs(py1 - oy1) < 0.003
            ):
                ox0, ox1 = min(ox0, px0), max(ox1, px1)
        return gap, root, ((ox0, oy0), (ox1, oy1))

    def m3_jog(x, y_a, y_b, own):
        lo, hi = sorted((y_a, y_b))
        if not m3_is_free(x, lo - CAP3, hi + CAP3, own):
            return False
        cell.add(
            gdstk.rectangle((x - HALF3, lo - CAP3), (x + HALF3, hi + CAP3), layer=30)
        )
        m3.append((((x - HALF3, lo - CAP3), (x + HALF3, hi + CAP3)), own[0]))
        return True

    for root, polys in members.items():
        layers = {p.layer for p in polys}
        if len(layers) != 1 or next(iter(layers)) not in (20, 40):
            continue
        net, layer = net_of_root[root], next(iter(layers))
        sx0 = min(p.bounding_box()[0][0] for p in polys)
        sx1 = max(p.bounding_box()[1][0] for p in polys)
        sy0 = min(p.bounding_box()[0][1] for p in polys)
        sy1 = max(p.bounding_box()[1][1] for p in polys)
        if sx1 - sx0 > 0.2 and sy1 - sy0 > 0.03:
            continue  # not a stub nor a bridged run: a bar the router can reach
        y_stub = (sy0 + sy1) / 2
        box = ((sx0, sy0), (sx1, sy1))
        if layer == 20:
            found = nearest(box, net, 20, 0.7, root, 0.036)
            if found is None:
                raise RuntimeError(
                    f"no {net} M2 near the stub at ({sx0:.3f}, {sy0:.3f})"
                )
            gap, target, ((tx0, ty0), (tx1, ty1)) = found
            x0, x1 = max(sx0, tx0), min(sx1, tx1)
            if gap < 0.2:  # the row's own bar, a strap away on M2
                lo, hi = min(sy0, ty0), max(sy1, ty1)
                for x in (
                    x0 + 0.018 + k * 0.036 for k in range(int((x1 - x0) / 0.036))
                ):
                    if m2_is_free(x - HALF, lo, x + HALF, hi, (root, target)):
                        cell.add(
                            gdstk.rectangle((x - HALF, lo), (x + HALF, hi), layer=20)
                        )
                        break
                else:
                    raise RuntimeError(
                        f"no room for an M2 strap from the {net} stub at ({sx0:.3f}, {sy0:.3f})"
                    )
                continue
            # A V2 at each end and an M3 jog on a track (the wordlines sit at
            # 18 mod 36 nm) inside both bars and clear of every other M3.
            y_bar = (ty0 + ty1) / 2
            first = math.ceil((x0 + 2 * HALF - 0.018) / 0.036) * 0.036 + 0.018
            for x in (first + k * 0.036 for k in range(40)):
                if x + 2 * HALF > x1:
                    raise RuntimeError(
                        f"no free M3 track over the {net} bar at ({x0:.3f}..{x1:.3f}, {y_stub:.3f})"
                    )
                if m3_jog(x, y_stub, y_bar, (root, target)):
                    break
            for y in (y_stub, y_bar):
                cell.add(
                    gdstk.rectangle(
                        (x - HALF, y - HALF), (x + HALF, y + HALF), layer=25
                    )
                )
        else:
            found = nearest(box, net, 30, 0.2, root, 2 * HALF3)
            if found is None:
                raise RuntimeError(
                    f"no {net} M3 near the M4 stub at ({sx0:.3f}, {sy0:.3f})"
                )
            _, target, ((tx0, ty0), (tx1, ty1)) = found
            x = (tx0 + tx1) / 2  # the stack's own M3 column
            if not sx0 + 0.020 <= x <= sx1 - 0.020:  # M4 past V3 by 11 nm a side
                raise RuntimeError(
                    f"the {net} M3 at x={x:.3f} is not under the M4 stub ({sx0:.3f}..{sx1:.3f})"
                )
            if (
                not ty0 <= y_stub <= ty1
            ):  # the stack's M3 stops short of the stub: jog to it
                y_m3 = ty0 if ty0 > y_stub else ty1
                if not m3_jog(x, y_stub, y_m3, (root, target)):
                    raise RuntimeError(
                        f"the M3 jog to the {net} M4 stub at ({x:.3f}, {y_stub:.3f}) is blocked"
                    )
            cell.add(
                gdstk.rectangle(
                    (x - HALF3, y_stub - 0.012), (x + HALF3, y_stub + 0.012), layer=35
                )
            )


def _supply_graph(cell):
    """The cell's metal graph, its supply-labelled roots, their polygons, and all M3/M2 boxes by root."""
    graph = MetalGraph(cell)
    net_of_root = {}
    for label in cell.get_labels(depth=None):
        net = supply(label.text)
        if net is not None and label.layer in METALS:
            net_of_root[graph.label_root(label)] = net
    members = defaultdict(list)
    m3, m2 = [], []
    for i, polygon in enumerate(graph.polygons):
        root = graph.root(i)
        if polygon.layer == 30:
            m3.append((polygon.bounding_box(), root))
        if polygon.layer == 20:
            m2.append((polygon.bounding_box(), root))
        if polygon.layer in METALS and root in net_of_root:
            members[root].append(polygon)
    return graph, net_of_root, members, m3, m2


def run(args):
    if args.wordlines < 2 or args.wordlines % 2 or args.bits < 2 or args.bits % 2:
        raise RuntimeError("wordlines and bits must be positive even values >= 2")
    if args.banks < 1 or args.banks & (args.banks - 1):
        raise RuntimeError("banks must be a power of two")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    array_wordlines = 2 * args.wordlines
    tap_pitch = math.gcd(array_wordlines, 16)

    def is_wordline(net):
        return WORDLINE.fullmatch(net) is not None

    # Only a stack's two ends carry a dummy row: its first tile the bottom
    # one, its last the top one, the tiles between none, so they abut array
    # row to array row and IO block to IO block.
    half_bits = args.bits // 2

    def ends_of(index):
        return (index == 0, index == half_bits - 1)

    variants = {}
    for index in range(half_bits):
        bottom, top = ends_of(index)
        if (bottom, top) in variants:
            continue
        cell = build_leaf(array_wordlines, tap_pitch, bottom=bottom, top=top)
        labels = [label for label in cell.labels if not supply(label.text)]
        variants[(bottom, top)] = (cell, *abstract(
            cell, "dp_column" + ends_tag(bottom, top), labels, abutted=is_wordline
        ))  # fmt: skip
    leaf = next(iter(variants.values()))[0]
    size = next(iter(variants.values()))[4]
    lef = "\n".join(v[3] for v in variants.values())
    strip_lib = gdstk.Library(unit=1e-6, precision=1e-10)
    strip_pairs, slice_load = build_wordline_strips(
        strip_lib, leaf, args.wordlines, args.bits
    )
    pair_masters = {}
    for half, pair in strip_pairs.items():
        pair_labels = [label for label in pair.labels if not supply(label.text)]
        pair_masters[half] = abstract(
            pair, f"dp_wl_strips_{half}", pair_labels, abutted=is_wordline
        )
    ctrl_lib = gdstk.read_gds(str(args.controller))
    ctrl = next(c for c in ctrl_lib.cells if c.name == "ctrl_decode")
    add_pin_conductors(ctrl)
    ctrl_labels = [
        label
        for label in ctrl.labels
        if label.texttype == 251 and not supply(label.text)
    ]
    controller, ctrl_pins, ctrl_lef, ctrl_size = abstract(
        ctrl, "dp_controller", ctrl_labels
    )
    masters = {v[1].name: (v[1], v[2]) for v in variants.values()}
    masters["dp_controller"] = (controller, ctrl_pins)
    masters.update({name: (cell, cell_pins) for name, (cell, cell_pins, _, _) in
                    ((m[0].name, m) for m in pair_masters.values())})  # fmt: skip
    (work / "blocks.lef").write_text(
        'VERSION 5.8 ;\nBUSBITCHARS "[]" ;\nDIVIDERCHAR "/" ;\n'
        + lef
        + "\n"
        + ctrl_lef
        + "\n"
        + "\n".join(m[2] for m in pair_masters.values())
        + "\nEND LIBRARY\n"
    )
    library = gdstk.Library(unit=1e-6, precision=1e-10)
    for c in (
        *(v[1] for v in variants.values()),
        controller,
        *(m[0] for m in pair_masters.values()),
    ):
        for dep in [c, *c.dependencies(True)]:
            if dep.name not in {p.name for p in library.cells}:
                library.add(dep)
    library.write_gds(str(work / "blocks.gds"), timestamp=columns.FIXED_GDS_TIMESTAMP)

    top_name = f"sram_x{args.wordlines * 2}x{args.bits}x{args.banks}"
    # Three kinds of empty space, each its own option: the margin between the
    # blocks and the macro edge, where the macro's pins land (M8/M9) and turn;
    # the channel on each side of the controller, where its outputs fan out;
    # and the gap between banks.  Every one is routed on the top layers over
    # the blocks as well, so none needs to hold whole buses.
    margin, channel, bank_gap = args.margin, args.channel_width, args.bank_gap
    margin_y = args.margin_y if args.margin_y is not None else margin
    for name, value in (
        ("margin", margin),
        ("margin_y", margin_y),
        ("channel", channel),
        ("bank gap", bank_gap),
    ):
        if value < 0.2:
            raise RuntimeError(f"the {name} must be at least 0.2 um, got {value}")
    # Preserve the validated pin-access phase in each bank. 2.88 um is the
    # least common multiple of all M1-M9 routing pitches; an arbitrary bank
    # stride can strand the small cap/tap ground terminals between tracks.
    bank_stride = math.ceil((size[0] + bank_gap) / 2.88) * 2.88
    width = max(
        (args.banks - 1) * bank_stride + size[0] + 2 * margin, ctrl_size[0] + 2 * margin
    )

    # Bottom to top: the lower stack of tiles, abutted at the tile's boundary
    # pitch; its strip pair facing down into it; a channel; the controller; a
    # channel; the upper stack's pair facing up; the upper stack.  A tile's
    # IO columns are shorter than its array, so the stacks keep a channel per
    # tile on each IO side for the controls and data.
    def tile_height(index):
        _, y0, _, y1 = columns.boundary_box(variants[ends_of(index)][0])
        return y1 - y0

    offsets = [sum(tile_height(k) for k in range(index)) for index in range(half_bits)]
    pair_height = columns.boundary_box(strip_pairs["hi"])[3]
    stack = offsets[-1] + tile_height(half_bits - 1)
    y_lo_tiles = margin_y
    y_lo_strips = y_lo_tiles + stack
    y_ctrl = y_lo_strips + pair_height + channel
    y_hi_strips = y_ctrl + ctrl_size[1] + channel
    y_hi_tiles = y_hi_strips + pair_height
    height = y_hi_tiles + stack + margin_y
    # (instance, master, origin, pin net -> macro net)
    instances = [("CTRL", "dp_controller", (margin, y_ctrl), lambda net: net)]
    nets = defaultdict(list)
    probes = defaultdict(list)
    for pin, info in ctrl_pins.items():
        if info.get("private"):
            continue
        nets[info["net"]].append(("CTRL", pin))
    for bank in range(args.banks):
        x_bank = margin + bank * bank_stride
        for bit in range(args.bits):
            inst = f"COL_{bank}_{bit}"
            index = bit % half_bits
            tile, hard, pins = variants[ends_of(index)][:3]
            bx0, by0, _, _ = columns.boundary_box(tile)
            tile_ox, tile_oy = bbox_origin(tile)
            y_tile = (y_lo_tiles if bit < half_bits else y_hi_tiles) + offsets[index]
            origin = (x_bank - (bx0 - tile_ox), y_tile - (by0 - tile_oy))
            resolve = functools.partial(
                leaf_net, bank=bank, bit=bit, wordlines=args.wordlines, bits=args.bits
            )
            instances.append((inst, hard.name, origin, resolve))
            for pin, info in pins.items():
                if info.get("private") or info.get("abutted"):
                    continue
                nets[resolve(info["net"])].append((inst, pin))
        for half, y_pair in (("lo", y_lo_strips), ("hi", y_hi_strips)):
            cell, cell_pins, _, _ = pair_masters[half]
            inst = f"WL_{half.upper()}_{bank}"
            pox, poy = bbox_origin(strip_pairs[half])
            origin = (x_bank + pox, y_pair + poy)  # the pair is drawn in the tile's x
            resolve = functools.partial(
                strip_net, bank=bank, half=half, wordlines=args.wordlines
            )
            instances.append((inst, cell.name, origin, resolve))
            for pin, info in cell_pins.items():
                if info.get("private") or info.get("abutted"):
                    continue
                nets[resolve(info["net"])].append((inst, pin))

    external = {"clk", "rst_n", "vdd", "vss"}
    external.update(f"{p}_n_{port}" for p in ("ce", "we", "oe") for port in "AB")
    address_bits = (2 * args.wordlines * 4 * args.banks - 1).bit_length()
    external.update(f"A_{port}[{bit}]" for port in "AB" for bit in range(address_bits))
    external.update(
        f"{p}_{port}[{bit}]" for p in "DQ" for port in "AB" for bit in range(args.bits)
    )
    for net in nets:
        if net not in external and not any(inst == "CTRL" for inst, pin in nets[net]):
            raise RuntimeError(f"controller has no driver for {net}")
    if external - nets.keys():
        raise RuntimeError(f"missing external nets {sorted(external - nets.keys())}")

    def dbu(value):
        return round(value * 1000)

    d = [
        "VERSION 5.8 ;",
        'DIVIDERCHAR "/" ;',
        'BUSBITCHARS "[]" ;',
        f"DESIGN {top_name} ;",
        "UNITS DISTANCE MICRONS 1000 ;",
        f"DIEAREA ( 0 0 ) ( {dbu(width)} {dbu(height)} ) ;",
        f"COMPONENTS {len(instances)} ;",
    ]
    for inst, master, (x, y), resolve in instances:
        d.append(f"- {inst} {master} + FIXED ( {dbu(x)} {dbu(y)} ) N ;")
        for pin, info in masters[master][1].items():
            if info.get("private"):
                net = f"private:{inst}:{pin}"
            else:
                net = resolve(info["net"])
            probes[net].append(
                {
                    "instance": inst,
                    "pin": pin,
                    "layer": info["layer"],
                    "point": [x + info["point"][0], y + info["point"][1]],
                }
            )
    d += ["END COMPONENTS", f"PINS {len(external)} ;"]
    for net in sorted(external):
        direction = (
            "INOUT"
            if net in ("vdd", "vss")
            else ("OUTPUT" if net.startswith("Q_") else "INPUT")
        )
        d.append(f"- {net} + NET {net} + DIRECTION {direction} + USE SIGNAL ;")
        nets[net].append(("PIN", net))
    d += ["END PINS", f"NETS {len(nets)} ;"]
    for net, ends in sorted(nets.items()):
        d.append(
            f"- {net} " + " ".join(f"( {inst} {pin} )" for inst, pin in ends) + " ;"
        )
    d += ["END NETS", "END DESIGN"]
    (work / "placed.def").write_text("\n".join(d) + "\n")
    manifest = {
        "cell": top_name,
        "size": [width, height],
        "tap_pitch": tap_pitch,
        "nets": probes,
        "external": sorted(external),
    }
    (work / "connectivity.json").write_text(json.dumps(manifest, indent=2) + "\n")
    tcl = [
        f"read_lef {{{REPO}/tech/lef/asap7_tech.lef}}",
        "read_lef blocks.lef",
        "read_def placed.def",
        "set_thread_count 2",
    ]
    for i, pitch in enumerate(
        (0.036, 0.036, 0.036, 0.048, 0.048, 0.064, 0.064, 0.080, 0.080), 1
    ):
        tcl.append(
            f"make_tracks M{i} -x_offset 0 -y_offset 0 -x_pitch {pitch} -y_pitch {pitch}"
        )
    tcl += [
        # The macro routes and pins on M1 up to --top-layer: odd, so the pins
        # take its horizontal neighbour below for the side edges and itself
        # for the top and bottom.  M7 by default: M8/M9 are the chip's, and
        # the public deck sizes them by length, which the tech LEF does not.
        f"place_pins -hor_layers M{args.top_layer - 1} -ver_layers M{args.top_layer}",
        f"set_routing_layers -signal M1-M{args.top_layer}",
        "global_route",
        f"detailed_route -droute_end_iter {args.route_iterations} -output_drc macro_drc.rpt",
        "write_def routed.def",
        "write_db routed.odb",
        "exit",
    ]
    (work / "route.tcl").write_text("\n".join(tcl) + "\n")
    with (work / "route.log").open("w") as log:
        result = subprocess.run(
            [args.openroad, "-exit", "route.tcl"],
            cwd=work,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=args.route_timeout,
        )
    if result.returncode:
        raise RoutingFailed(f"macro routing failed; see {work}/route.log")
    # Distribution KLayout bindings may target system Python, while gdstk is
    # installed in a virtual environment with a different Python ABI.
    subprocess.run(
        [
            "python3",
            str(REPO / "scripts/def_to_gds.py"),
            "--def-file",
            str(work / "routed.def"),
            "--tech-lef",
            str(REPO / "tech/lef/asap7_tech.lef"),
            "--cell-lef",
            str(work / "blocks.lef"),
            "--macro-gds",
            str(work / "blocks.gds"),
            "--output-gds",
            str(work / "routed.gds"),
            "--top-cell",
            top_name,
        ],
        check=True,
    )
    lib = gdstk.read_gds(str(work / "routed.gds"))
    top = next(c for c in lib.cells if c.name == top_name)
    # KLayout writes DEF BPin rectangles on purpose 251. They are real
    # conductor landings as well as LEF pin markers, not label-only geometry.
    add_pin_conductors(top)
    lib.write_gds(str(work / "routed.gds"), timestamp=columns.FIXED_GDS_TIMESTAMP)
    # No final artifact is published until every physical terminal is joined
    # to its net and every distinct named net remains isolated.
    verify(work / "routed.gds", manifest)
    check_route_drc(work / "macro_drc.rpt")
    top.add(gdstk.rectangle((0, 0), (width, height), layer=100))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    lib.write_gds(str(args.output), timestamp=columns.FIXED_GDS_TIMESTAMP)
    report = {
        "cell": top_name,
        "size_um": [width, height],
        "banks": args.banks,
        "bits": args.bits,
        "margin_um": args.margin,
        "floorplan": "controller band between two stacks of abutted column tiles "
        "(port A IO | array | port B IO), a wordline driver strip pair on each side",
        "wordlines_per_half": args.wordlines,
        "array_wordlines": array_wordlines,
        "tap_pitch": tap_pitch,
        "column_tiles": args.banks * args.bits,
        "wordline_slice": wl_slices.slice_name(slice_load),
        "cells_along_wordline": 4 * (args.bits // 2),
        "wordline_strip_pairs": 2 * args.banks,
        "checked_net_partitions": len(probes),
        "routing_drc_violations": 0,
        "physical_connectivity": "PASS",
        "signoff_lvs": "not_run",
        "device_drc": "not_run",
        "characterization": "not_measured",
        "reports": str(work),
    }
    args.output.with_suffix(".physical.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(
        f"PASS: {top_name}: {args.banks * args.bits} column tiles, {2 * args.banks} strip pairs, "
        f"{len(probes)} connected nets"
    )


def add_pin_conductors(cell):
    """DEF pin rectangles are conductors as well as pin-purpose markers."""
    for polygon in list(cell.polygons):
        if polygon.layer in METALS and polygon.datatype == 251:
            drawing = polygon.copy()
            drawing.datatype = 0
            cell.add(drawing)


def check_route_drc(report):
    if not report.is_file():
        raise RuntimeError(f"missing routing DRC report: {report}")
    if report.read_text().strip():
        raise RuntimeError(f"macro routing DRC violations remain: {report}")


def verify(path, manifest):
    lib = gdstk.read_gds(str(path))
    top = next(c for c in lib.cells if c.name == manifest["cell"])
    graph = MetalGraph(top)
    roots = {}
    for net, probes in manifest["nets"].items():
        components = set()
        for probe in probes:
            found = graph.at(probe["layer"], probe["point"])
            if len(found) != 1:
                raise RuntimeError(f"{net}: missing/ambiguous terminal {probe}")
            components.update(found)
        if len(components) != 1:
            raise RuntimeError(
                f"{net}: {len(components)} disconnected physical components"
            )
        root = components.pop()
        if root in roots:
            raise RuntimeError(f"short: {net} / {roots[root]}")
        roots[root] = net
    labels = {label.text: label for label in top.labels}
    for net in manifest["external"]:
        if net not in labels or roots.get(graph.label_root(labels[net])) != net:
            raise RuntimeError(f"external pin {net} missing or disconnected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wordlines", type=int, required=True)
    parser.add_argument("--bits", type=int, required=True)
    parser.add_argument("--banks", type=int, default=1)
    parser.add_argument("--controller", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--openroad", default="openroad")
    parser.add_argument("--route-iterations", type=int, default=64)
    parser.add_argument("--route-timeout", type=int, default=900)
    parser.add_argument("--channel-width", type=float, default=0.3,
                        help="um between the controller and each strip pair")  # fmt: skip
    parser.add_argument("--margin", type=float, default=0.3,
                        help="um from the blocks to the macro edge, where the pins land")  # fmt: skip
    parser.add_argument("--bank-gap", type=float, default=0.3, help="um between banks")
    parser.add_argument(
        "--margin-y",
        type=float,
        default=None,
        help="top/bottom margin, if not --margin",
    )
    parser.add_argument(
        "--top-layer",
        type=int,
        default=7,
        choices=(5, 7, 9),
        help="highest metal the macro routes and pins on",
    )
    parser.add_argument(
        "--max-margin",
        type=float,
        default=1.2,
        help="widen the margin (doubling) up to this if routing fails",
    )
    args = parser.parse_args()
    # The margin is where the macro pins land and the controller's outputs
    # turn; how much the router needs depends on where it put the pins,
    # which is not known before routing.  Start tight, widen on failure.
    while True:
        try:
            run(args)
            return
        except RoutingFailed as error:
            wider = round(args.margin * 2, 3)
            if wider > args.max_margin:
                raise RuntimeError(
                    f"{error}; margin {args.margin} um is the widest tried"
                ) from error
            print(
                f"routing failed at a {args.margin} um margin; retrying at {wider} um"
            )
            args.margin = wider


class RoutingFailed(RuntimeError):
    """OpenROAD could not route the assembled macro."""


if __name__ == "__main__":
    main()
