#!/usr/bin/env python3
"""Generate the ASAP7 dual-port 8T column hierarchy: one IO per end of the bitlines.

Each port has its IO column at its own end of one unsplit array::

    iocol A | edge cap | array, N wordlines ... tap | iocol B

Port A's bitlines leave the array on M2 and port B's on M4, so neither port
crosses the other's IO.  (The earlier floorplan put both IO cores mid-bitline
between two half arrays: port B crossed the A core on M4 and port A climbed to
M6 to cross the B core.)

The port-A and port-B hard wrappers are still the published differential core
behind a pitch adapter, and that core has a bitline interface on both faces.
Only the face turned to the array is used -- A's right face, B's left.  The
other face's column selects and precharge enable are tied off so that it
idles precharged: they carry VSS/VDD labels here, and the macro router
connects every supply-labelled conductor to its rail.

The resulting ``colgrp_x{N}x4_sram_8t`` cells match the logical
``colgrp_sram_8t`` hierarchy emitted by ``SpiceGenerator``.  N is the number
of wordlines of the array and may be any value present in the parameterized
wordline-array input GDS.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import math
import re
import sys
from pathlib import Path

import gdstk
from generate_asap7_8t_bitcell import topbot_name


BOUNDARY = 100
PIN_TEXTTYPE = 251
M1, V1, M2, V2, M3, V3, M4, V4, M5, V5, M6, V6, M7 = (
    19, 21, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70
)
MUX_ROWS = 4
GAP_WIDTH = 0.144
FIXED_GDS_TIMESTAMP = dt.datetime(2020, 1, 1, 0, 0, 0)


def bbox(poly: gdstk.Polygon) -> tuple[float, float, float, float]:
    (x0, y0), (x1, y1) = poly.bounding_box()
    return tuple(round(float(value), 7) for value in (x0, y0, x1, y1))


def boundary_box(cell: gdstk.Cell) -> tuple[float, float, float, float]:
    boxes = [bbox(poly) for poly in cell.polygons
             if poly.layer == BOUNDARY and poly.datatype == 0]
    if len(boxes) != 1:
        raise RuntimeError(f"{cell.name}: expected one direct BOUNDARY polygon")
    return boxes[0]


def direct_label(cell: gdstk.Cell, name: str, layer: int | None = None) -> gdstk.Label:
    labels = [label for label in cell.labels if label.text == name
              and (layer is None or label.layer == layer)]
    if len(labels) != 1:
        raise RuntimeError(
            f"{cell.name}: expected one direct {name!r} label, found {len(labels)}"
        )
    return labels[0]


def indexed_labels(cell: gdstk.Cell, prefix: str) -> dict[int, gdstk.Label]:
    pattern = re.compile(rf"{re.escape(prefix)}\[([0-9]+)\]")
    result: dict[int, gdstk.Label] = {}
    for label in cell.labels:
        match = pattern.fullmatch(label.text)
        if not match:
            continue
        index = int(match.group(1))
        if index in result:
            raise RuntimeError(f"{cell.name}: duplicate {label.text}")
        result[index] = label
    return result


def rect(
    cell: gdstk.Cell,
    box: tuple[float, float, float, float],
    layer: int,
) -> gdstk.Polygon:
    polygon = gdstk.rectangle(box[:2], box[2:], layer=layer, datatype=0)
    cell.add(polygon)
    return polygon


def clone_label(
    label: gdstk.Label,
    text: str,
    origin: tuple[float, float],
    layer: int | None = None,
) -> gdstk.Label:
    return gdstk.Label(
        text,
        origin,
        anchor=label.anchor,
        rotation=label.rotation,
        magnification=label.magnification,
        x_reflection=label.x_reflection,
        layer=label.layer if layer is None else layer,
        texttype=PIN_TEXTTYPE,
    )


def add_route_shape(
    cell: gdstk.Cell,
    routes: dict[str, list[tuple[int, tuple[float, float, float, float]]]],
    net: str,
    layer: int,
    box: tuple[float, float, float, float],
) -> None:
    rect(cell, box, layer)
    routes.setdefault(net, []).append((layer, box))


# The face of each wrapper that meets the array.  The published core names its
# two faces T and B after the half arrays it used to sit between; here they
# are just the left (T) and the right (B) face of the hard cell.
ARRAY_FACE = {"A": "B", "B": "T"}


def port_pin_name(name: str, port: str) -> str | None:
    """Composite name of a wrapper pin, a supply to tie it to, or None to hide it.

    Pins of the face turned away from the array are tied so that it idles
    precharged with every column deselected.
    """
    used, unused = ARRAY_FACE[port], "TB".replace(ARRAY_FACE[port], "")
    scalar = {
        f"WRENA_{port}": f"wrena_{port}",
        f"WRENAN_{port}": f"wrenan_{port}",
        f"D_{port}": f"D{port}",
        f"Q_{port}": f"Q{port}",
        f"OEB_OUT_{port}": f"oeb_out_{port}",
        f"OE_OUT_{port}": f"oe_out_{port}",
        f"SAE_{port}": f"sae_{port}",
        f"BLPRECH{used}N_{port}": f"blprechn_{port}",
        f"BLPRECH{unused}N_{port}": "VSS",
    }
    if name in scalar:
        return scalar[name]
    match = re.fullmatch(rf"YSEL([TB])(N?)_{port}\[([0-3])\]", name)
    if not match:
        return None
    if match.group(1) == used:
        return f"ysel{match.group(2).lower()}_{port}[{match.group(3)}]"
    return "VDD" if match.group(2) else "VSS"


def colgrp_control_name(name: str) -> str:
    match = re.fullmatch(
        r"(wrena|wrenan|oeb_out|oe_out|blprechn)_([AB])", name
    )
    if match:
        return f"{match.group(1)}{match.group(2)}"
    match = re.fullmatch(r"(yseln|ysel)_([AB])\[([0-3])\]", name)
    if match:
        return f"{match.group(1)}{match.group(2)}[{match.group(3)}]"
    return name


def validate_route_spacing(
    routes: dict[str, list[tuple[int, tuple[float, float, float, float]]]]
) -> None:
    spacing = {
        V1: 0.018, M2: 0.018, V2: 0.018, M3: 0.018,
        V3: 0.018, M4: 0.024, V4: 0.033, M5: 0.024,
        V5: 0.033, M6: 0.032, V6: 0.045, M7: 0.032,
    }
    names = list(routes)
    for index, name in enumerate(names):
        for other_name in names[index + 1:]:
            for layer, a in routes[name]:
                for other_layer, b in routes[other_name]:
                    if layer != other_layer:
                        continue
                    ax0, ay0, ax1, ay1 = a
                    bx0, by0, bx1, by1 = b
                    dx = max(ax0 - bx1, bx0 - ax1, 0.0)
                    dy = max(ay0 - by1, by0 - ay1, 0.0)
                    required = spacing.get(layer, 0.018)
                    if dx == 0.0:
                        okay = dy >= required - 1e-9
                    elif dy == 0.0:
                        okay = dx >= required - 1e-9
                    else:
                        okay = math.hypot(dx, dy) >= required - 1e-9
                    if not okay:
                        raise RuntimeError(
                            f"route spacing violation on layer {layer}: "
                            f"{name} {a} vs {other_name} {b}"
                        )


def port_io_name(port: str) -> str:
    return f"iocol_sram_8t_{port.lower()}"


def build_port_io(library: gdstk.Library, wrapper: gdstk.Cell, port: str) -> gdstk.Cell:
    """One port's IO column: its wrapper, and a strapped gap on the array side.

    Port A stands left of the array and port B right of it, so the gap is on
    A's right and on B's left.  The bitlines cross it on the layer the array
    delivers them on, M2 for port A and M4 for port B.
    """
    x0, y0, x1, y1 = boundary_box(wrapper)
    width, height = x1 - x0, y1 - y0
    if height <= 0 or width <= 0:
        raise RuntimeError("invalid 8T IO wrapper boundary")
    if any(poly.layer == M5 for poly in wrapper.polygons):
        raise RuntimeError("8T wrapper unexpectedly occupies the M5 wordline layer")
    layer, half = (M2, 0.009) if port == "A" else (M4, 0.012)
    wrapper_x = 0.0 if port == "A" else GAP_WIDTH
    total_width = width + GAP_WIDTH
    edge_x = total_width if port == "A" else 0.0

    cell = library.new_cell(port_io_name(port))
    cell.add(gdstk.Reference(wrapper, origin=(wrapper_x - x0, -y0)))
    routes: dict[str, list[tuple[int, tuple[float, float, float, float]]]] = {}
    face = ARRAY_FACE[port]
    for source, target in ((f"BL{face}", "BL"), (f"BL{face}N", "BLN")):
        labels = indexed_labels(wrapper, f"{source}_{port}")
        if set(labels) != set(range(MUX_ROWS)):
            raise RuntimeError(f"{wrapper.name}: incomplete {source}_{port} bus")
        for index, label in labels.items():
            if label.layer != layer:
                raise RuntimeError(f"{wrapper.name}: {label.text} is not on layer {layer}")
            y = float(label.origin[1]) - y0
            net = f"{target}_{port}[{index}]"
            span = ((width - 0.018, total_width) if port == "A"
                    else (0.0, GAP_WIDTH + 0.018))
            add_route_shape(cell, routes, net, layer,
                            (span[0], y - half, span[1], y + half))
            cell.add(clone_label(
                label, net, (edge_x + (-0.006 if port == "A" else 0.006), y), layer
            ))

    # SAE and SAPRECHN are intentionally shorted, matching the composite SPICE
    # phase convention: one sense phase per port precharges low, evaluates high.
    sae = direct_label(wrapper, f"SAE_{port}")
    saprechn = direct_label(wrapper, f"SAPRECHN_{port}")
    sae_point = (wrapper_x + float(sae.origin[0]) - x0, float(sae.origin[1]) - y0)
    sap_point = (wrapper_x + float(saprechn.origin[0]) - x0,
                 float(saprechn.origin[1]) - y0)
    if abs(sae_point[1] - sap_point[1]) > 1e-9:
        raise RuntimeError(f"port {port} sense controls are not row-aligned")
    add_route_shape(
        cell, routes, f"sae_{port}", M3,
        (min(sae_point[0], sap_point[0]) - 0.008, sae_point[1] - 0.009,
         max(sae_point[0], sap_point[0]) + 0.008, sae_point[1] + 0.009),
    )

    for label in wrapper.labels:
        renamed = port_pin_name(label.text, port)
        if renamed is None:
            continue
        point = (wrapper_x + float(label.origin[0]) - x0, float(label.origin[1]) - y0)
        cell.add(clone_label(label, renamed, point))

    # One direct label per supply, on the wrapper's own M1 rails.  The tie-offs
    # above carry the same names on M3, which is how to tell them apart.
    vss_source = max(
        (label for label in wrapper.labels if label.text == "VSS" and label.layer == M1),
        key=lambda label: float(label.origin[0]),
    )
    vdd_source = next(
        label for label in wrapper.labels if label.text == "VDD" and label.layer == M1
    )
    for source in (vss_source, vdd_source):
        point = (wrapper_x + float(source.origin[0]) - x0, float(source.origin[1]) - y0)
        cell.add(clone_label(source, source.text, point, M1))

    validate_route_spacing(routes)
    rect(cell, (0.0, 0.0, total_width, height), BOUNDARY)
    return cell


def build_cap_array(library: gdstk.Library, edges: dict[str, gdstk.Cell],
                    mirror_x: bool = False) -> gdstk.Cell:
    cap = edges[topbot_name()]
    filler = edges["FILLER_cgedge_8t"]
    x0, y0, x1, y1 = boundary_box(cap)
    width, height = x1 - x0, y1 - y0
    fx0, fy0, fx1, fy1 = boundary_box(filler)
    filler_width = fx1 - fx0
    if abs(fy1 - fy0 - MUX_ROWS * height) > 1e-7:
        raise RuntimeError("8T column-group filler height does not match caps")
    cell = library.new_cell("col_cap_x4_sram_8t" + ("_lr" if mirror_x else ""))
    cell.add(gdstk.Reference(filler, origin=((width if mirror_x else 0) - fx0, -fy0)))
    for row in range(MUX_ROWS):
        master = edges[topbot_name(mirror_x, bool(row % 2))]
        origin = ((0 if mirror_x else filler_width) - x0, row * height - y0)
        cell.add(gdstk.Reference(master, origin=origin))
    rect(cell, (0.0, 0.0, width + filler_width, MUX_ROWS * height), BOUNDARY)
    return cell


def colgrp_name(wordlines: int) -> str:
    return f"colgrp_x{wordlines}x4_sram_8t"


def build_colgrp(
    library: gdstk.Library,
    array: gdstk.Cell,
    io_a: gdstk.Cell,
    io_b: gdstk.Cell,
    cap_array: gdstk.Cell,
    wordlines: int,
) -> gdstk.Cell:
    """``iocol A | edge cap | array | iocol B``, one array of `wordlines` wordlines.

    The cap terminates the array's first bitcell as it did at the outer end of
    a half array; it hands every bitline through, and the filler beside it has
    no metal, so port A's M2 bitlines are strapped across the filler.  The far
    end needs no cap: a tapped array ends in a tap there, which is what an IO
    face has always met.
    """
    arx0, ary0, arx1, ary1 = boundary_box(array)
    ax0, ay0, ax1, ay1 = boundary_box(io_a)
    bx0, by0, bx1, by1 = boundary_box(io_b)
    cpx0, cpy0, cpx1, cpy1 = boundary_box(cap_array)
    array_width, array_height = arx1 - arx0, ary1 - ary0
    cap_width = cpx1 - cpx0
    for box, what in (((ax0, ay0, ax1, ay1), "port-A IO"), ((bx0, by0, bx1, by1), "port-B IO"),
                      ((cpx0, cpy0, cpx1, cpy1), "edge cap")):
        if abs((box[3] - box[1]) - array_height) > 1e-6:
            raise RuntimeError(f"8T array and {what} heights do not match")

    cap_x = ax1 - ax0
    array_x = cap_x + cap_width
    io_b_x = array_x + array_width
    total_width = io_b_x + (bx1 - bx0)
    cell = library.new_cell(colgrp_name(wordlines))
    cell.add(gdstk.Reference(io_a, origin=(-ax0, -ay0)))
    cell.add(gdstk.Reference(cap_array, origin=(cap_x - cpx0, -cpy0)))
    cell.add(gdstk.Reference(array, origin=(array_x - arx0, -ary0)))
    cell.add(gdstk.Reference(io_b, origin=(io_b_x - bx0, -by0)))

    for source, layer in (("WLA", M3), ("WLB", M5)):
        for index, label in indexed_labels(array, source).items():
            x, y = map(float, label.origin)
            cell.add(clone_label(
                label, f"{source}[{index}]", (array_x + x - arx0, y - ary0), layer,
            ))

    # Each port's bitlines have to arrive at the height and on the layer its IO
    # column takes them.  Port A's cross the metal-free filler to the cap.
    filler = next(ref.cell for ref in cap_array.references
                  if ref.cell_name == "FILLER_cgedge_8t")
    if any(poly.layer in (M2, M4) for poly in filler.polygons):
        raise RuntimeError("edge filler is no longer free of bitline metal")
    fx0, _, fx1, _ = boundary_box(filler)
    cap_metal_x = cap_x + (fx1 - fx0)
    bitline_map = {
        "BLA": ("BL_A", io_a, M2), "BLAN": ("BLN_A", io_a, M2),
        "BLB": ("BL_B", io_b, M4), "BLBN": ("BLN_B", io_b, M4),
    }
    for source, (io_prefix, io_cell, layer) in bitline_map.items():
        array_labels = indexed_labels(array, source)
        io_labels = indexed_labels(io_cell, io_prefix)
        if set(array_labels) != set(io_labels):
            raise RuntimeError(f"{cell.name}: incomplete {source} interface")
        for index, array_label in array_labels.items():
            y = float(array_label.origin[1]) - ary0
            io_label = io_labels[index]
            io_y = float(io_label.origin[1]) - (ay0 if io_cell is io_a else by0)
            if io_label.layer != layer or abs(io_y - y) > 1e-6:
                raise RuntimeError(
                    f"{cell.name}: {io_label.text} does not align to {source}[{index}]"
                )
            if io_cell is io_a:
                rect(cell, (cap_x - 0.018, y - 0.009, cap_metal_x + 0.018, y + 0.009), layer)

    for io_cell, io_x, (ox, oy) in ((io_a, 0.0, (ax0, ay0)), (io_b, io_b_x, (bx0, by0))):
        for label in io_cell.labels:
            if label.text.startswith(("BL_", "BLN_")):
                continue
            if label.text in {"VDD", "VSS"} and label.layer == M1 and io_cell is io_a:
                continue  # one rail label per supply is enough; port B's are kept
            point = (io_x + float(label.origin[0]) - ox, float(label.origin[1]) - oy)
            cell.add(clone_label(label, colgrp_control_name(label.text), point))

    rect(cell, (0.0, 0.0, total_width, array_height), BOUNDARY)
    return cell


def load_cells(path: Path) -> tuple[gdstk.Library, dict[str, gdstk.Cell]]:
    library = gdstk.read_gds(str(path))
    return library, {cell.name: cell for cell in library.cells}


def build_library(
    arrays_gds: Path,
    wrappers_gds: Path,
    edges_gds: Path,
    wordline_counts: list[int],
) -> gdstk.Library:
    arrays_lib, arrays = load_cells(arrays_gds)
    wrappers_lib, wrappers = load_cells(wrappers_gds)
    edges_lib, edges = load_cells(edges_gds)
    units = {(library.unit, library.precision)
             for library in (arrays_lib, wrappers_lib, edges_lib)}
    if len(units) != 1:
        raise RuntimeError("8T source GDS units/precision do not match")
    wrapper_a = wrappers.get("ioprech_sram_8t_a")
    wrapper_b = wrappers.get("ioprech_sram_8t_b")
    edge_names = {topbot_name(False, my) for my in (False, True)}
    edge_names.update({"FILLER_BLANK_8t", "FILLER_cgedge_8t"})
    bitcell = arrays.get("sram_cell_8t")
    if None in (wrapper_a, wrapper_b, bitcell) or not edge_names <= edges.keys():
        raise RuntimeError("8T source GDS is missing a required hard cell")

    unit, precision = units.pop()
    library = gdstk.Library(
        "openfinram_asap7_8t_iocolumn", unit=unit, precision=precision
    )
    library.add(bitcell, wrapper_a, wrapper_b, *(edges[name] for name in sorted(edge_names)))
    selected_arrays: dict[int, gdstk.Cell] = {}
    for count in wordline_counts:
        row_name = f"sramcol_x{count}_sram_8t"
        array_name = f"array_x{count}x4_sram_8t"
        if row_name not in arrays or array_name not in arrays:
            raise RuntimeError(
                f"{arrays_gds}: missing {array_name}; regenerate the wordline "
                f"artifact with --word-lines including {count}"
            )
        library.add(arrays[row_name], arrays[array_name])
        selected_arrays[count] = arrays[array_name]

    io_a = build_port_io(library, wrapper_a, "A")
    io_b = build_port_io(library, wrapper_b, "B")
    cap_array = build_cap_array(library, edges)
    for count in wordline_counts:
        build_colgrp(library, selected_arrays[count], io_a, io_b, cap_array, count)
    return library


def assert_close(actual: float, expected: float, message: str) -> None:
    if abs(actual - expected) > 1e-6:
        raise RuntimeError(f"{message}: {actual} != {expected}")


def verify_gds(path: Path, wordline_counts: list[int]) -> dict[str, str]:
    library, cells = load_cells(path)
    expected = {
        "sram_cell_8t", "FILLER_BLANK_8t", "FILLER_cgedge_8t",
        "ioprech_sram_8t_a", "ioprech_sram_8t_b",
        port_io_name("A"), port_io_name("B"), "col_cap_x4_sram_8t",
    }
    expected.update(topbot_name(False, my) for my in (False, True))
    for count in wordline_counts:
        expected.update({
            f"sramcol_x{count}_sram_8t",
            f"array_x{count}x4_sram_8t",
            colgrp_name(count),
        })
    if set(cells) != expected:
        raise RuntimeError(
            f"{path}: cell set mismatch; missing={sorted(expected - set(cells))}, "
            f"extra={sorted(set(cells) - expected)}"
        )

    io_boxes = {}
    for port, layer, other in (("A", M2, M4), ("B", M4, None)):
        io = cells[port_io_name(port)]
        wrapper = f"ioprech_sram_8t_{port.lower()}"
        if [reference.cell_name for reference in io.references] != [wrapper]:
            raise RuntimeError(f"{io.name}: expected exactly its own wrapper")
        if any(ref.rotation or ref.x_reflection for ref in io.references):
            raise RuntimeError(f"{io.name}: the wrapper must only be translated")
        io_boxes[port] = boundary_box(io)
        assert_close(io_boxes[port][3] - io_boxes[port][1], 2.376, f"{io.name} height")
        wrapper_box = boundary_box(cells[wrapper])
        assert_close(io_boxes[port][2] - io_boxes[port][0],
                     wrapper_box[2] - wrapper_box[0] + GAP_WIDTH, f"{io.name} width")
        # The bitlines meet the array at the face turned to it, on its layer.
        face_x = io_boxes[port][2] if port == "A" else io_boxes[port][0]
        for prefix in ("BL", "BLN"):
            labels = indexed_labels(io, f"{prefix}_{port}")
            if set(labels) != set(range(MUX_ROWS)):
                raise RuntimeError(f"{io.name}: incomplete {prefix}_{port} bus")
            for label in labels.values():
                if label.layer != layer:
                    raise RuntimeError(f"{io.name}: {label.text} is on the wrong layer")
                if abs(float(label.origin[0]) - face_x) > 0.0061:
                    raise RuntimeError(f"{io.name}: {label.text} is not at the array face")
        # One IO per end: nothing crosses a core any more, so nothing of the
        # composite's own rises above the layer its bitlines arrive on.
        own = {poly.layer for poly in io.polygons} - {BOUNDARY}
        if not own <= {layer, M3}:
            raise RuntimeError(f"{io.name}: unexpected routing layers {sorted(own)}")
        names = [label.text for label in io.labels]
        required = {
            f"wrena_{port}", f"wrenan_{port}", f"D{port}", f"Q{port}",
            f"oeb_out_{port}", f"oe_out_{port}", f"blprechn_{port}", f"sae_{port}",
        }
        required.update(f"{prefix}_{port}[{index}]" for prefix in ("ysel", "yseln")
                        for index in range(MUX_ROWS))
        if not required.issubset(names):
            raise RuntimeError(f"{io.name}: missing IO pins {sorted(required - set(names))}")
        leaked = [name for name in names if re.match(r"(YSEL|BLPRECH|SAPRECHN|WRENA|D_|Q_)", name)
                  or re.match(r"(yselt|yselb|blprecht|blprechb)", name)]
        if leaked:
            raise RuntimeError(f"{io.name}: wrapper-only or split-era pins leaked: {leaked}")
        # The idle face: four selects and its precharge enable low, four
        # complement selects high, each on the wrapper's own M3 pin.
        ties = {supply: [label for label in io.labels
                         if label.text == supply and label.layer == M3]
                for supply in ("VDD", "VSS")}
        if (len(ties["VSS"]), len(ties["VDD"])) != (MUX_ROWS + 1, MUX_ROWS):
            raise RuntimeError(f"{io.name}: idle-face tie-offs are incomplete")
        unused = "TB".replace(ARRAY_FACE[port], "")
        wrapper_cell = cells[wrapper]
        wx0, wy0, _, _ = wrapper_box
        shift = 0.0 if port == "A" else GAP_WIDTH
        for supply, pins in (
            ("VSS", [f"BLPRECH{unused}N_{port}"]
                    + [f"YSEL{unused}_{port}[{i}]" for i in range(MUX_ROWS)]),
            ("VDD", [f"YSEL{unused}N_{port}[{i}]" for i in range(MUX_ROWS)]),
        ):
            want = sorted((round(shift + float(direct_label(wrapper_cell, pin).origin[0]) - wx0, 6),
                           round(float(direct_label(wrapper_cell, pin).origin[1]) - wy0, 6))
                          for pin in pins)
            have = sorted((round(float(label.origin[0]), 6), round(float(label.origin[1]), 6))
                          for label in ties[supply])
            if want != have:
                raise RuntimeError(f"{io.name}: {supply} tie-offs are not on the idle face's pins")
        m1_geometry = io.get_polygons(layer=M1, datatype=0)
        for supply in ("VDD", "VSS"):
            label = direct_label(io, supply, M1)
            x, y = map(float, label.origin)
            if not gdstk.inside([(x, y)], m1_geometry)[0]:
                raise RuntimeError(f"{io.name}: {supply} label misses wrapper M1")

    cap_array = cells["col_cap_x4_sram_8t"]
    expected_caps = ["FILLER_cgedge_8t"] + [topbot_name(False, bool(row % 2)) for row in range(MUX_ROWS)]
    if [ref.cell_name for ref in cap_array.references] != expected_caps:
        raise RuntimeError(f"{cap_array.name}: expected filler and four oriented cap masters")
    if any(ref.rotation or ref.x_reflection for ref in cap_array.references):
        raise RuntimeError(f"{cap_array.name}: edge masters must only be translated")
    cap_box = boundary_box(cap_array)
    for count in wordline_counts:
        array = cells[f"array_x{count}x4_sram_8t"]
        colgrp = cells[colgrp_name(count)]
        array_box = boundary_box(array)
        colgrp_box = boundary_box(colgrp)
        expected_width = (
            (array_box[2] - array_box[0]) + (cap_box[2] - cap_box[0])
            + sum(box[2] - box[0] for box in io_boxes.values())
        )
        assert_close(colgrp_box[2] - colgrp_box[0], expected_width, f"{colgrp.name}: width")
        assert_close(colgrp_box[3] - colgrp_box[1], 2.376, f"{colgrp.name}: height")
        expected_refs = [
            port_io_name("A"), "col_cap_x4_sram_8t",
            f"array_x{count}x4_sram_8t", port_io_name("B"),
        ]
        if [reference.cell_name for reference in colgrp.references] != expected_refs:
            raise RuntimeError(f"{colgrp.name}: expected iocol A / cap / array / iocol B")
        if any(ref.rotation or ref.x_reflection for ref in colgrp.references):
            raise RuntimeError(f"{colgrp.name}: one unsplit array, nothing mirrored")
        xs = [float(reference.origin[0]) for reference in colgrp.references]
        if xs != sorted(xs):
            raise RuntimeError(f"{colgrp.name}: port A must be left of the array, port B right")
        for prefix, layer in (("WLA", M3), ("WLB", M5)):
            labels = indexed_labels(colgrp, prefix)
            if set(labels) != set(range(count)):
                raise RuntimeError(f"{colgrp.name}: incomplete {prefix} bus")
            if any(label.layer != layer for label in labels.values()):
                raise RuntimeError(f"{colgrp.name}: {prefix} is on wrong layer")
        # Port A's bitlines are strapped across the metal-free filler.
        straps = [bbox(poly) for poly in colgrp.polygons if poly.layer == M2]
        if len(straps) != 2 * MUX_ROWS:
            raise RuntimeError(f"{colgrp.name}: expected eight port-A bitline straps")
        required_colgrp = {
            "DA", "QA", "DB", "QB",
            "wrenaA", "wrenanA", "wrenaB", "wrenanB",
            "oeb_outA", "oe_outA", "oeb_outB", "oe_outB",
            "blprechnA", "blprechnB", "sae_A", "sae_B", "VDD", "VSS",
        }
        required_colgrp.update(
            f"{prefix}{port}[{index}]"
            for prefix in ("yseln", "ysel")
            for port in ("A", "B") for index in range(MUX_ROWS)
        )
        names = {label.text for label in colgrp.labels}
        if not required_colgrp.issubset(names):
            raise RuntimeError(
                f"{colgrp.name}: missing pins {sorted(required_colgrp - names)}"
            )
        split_era = sorted(name for name in names
                           if re.match(r"(WLT|WLBA|WLBB|yselt|yselb|blprecht|blprechb)", name))
        if split_era:
            raise RuntimeError(f"{colgrp.name}: split-era pins remain: {split_era}")

    digests: dict[str, str] = {}
    for name in sorted(cells):
        if not name.startswith(("iocol_", "colgrp_", "col_cap_")):
            continue
        records: list[str] = []
        cell = cells[name]
        records.extend(
            f"P:{polygon.layer}:{polygon.datatype}:{bbox(polygon)}"
            for polygon in cell.polygons
        )
        records.extend(
            f"L:{label.text}:{label.layer}:{label.texttype}:"
            f"{tuple(round(float(value), 7) for value in label.origin)}"
            for label in cell.labels
        )
        records.extend(
            f"R:{reference.cell_name}:"
            f"{tuple(round(float(value), 7) for value in reference.origin)}:"
            f"{round(float(reference.rotation or 0), 7)}:"
            f"{int(reference.x_reflection)}"
            for reference in cell.references
        )
        digests[name] = hashlib.sha256(
            "\n".join(sorted(records)).encode()
        ).hexdigest()
    return digests


def parse_wordlines(value: str) -> list[int]:
    try:
        counts = sorted({int(item) for item in value.split(",")})
    except ValueError as error:
        raise argparse.ArgumentTypeError("wordline counts must be integers") from error
    if not counts or any(count < 1 for count in counts):
        raise argparse.ArgumentTypeError("wordline counts must be positive")
    return counts


def parse_args(argv: list[str]) -> argparse.Namespace:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--word-lines", type=parse_wordlines,
        default=parse_wordlines("2,32,64,128"),
        help="comma-separated array wordline counts (default: 2,32,64,128)",
    )
    parser.add_argument(
        "--arrays-gds", type=Path,
        default=repo / "tech/gds/sram_wordline_arrays.gds",
    )
    parser.add_argument(
        "--wrappers-gds", type=Path,
        default=repo / "tech/gds/sram_8t_ioprech.gds",
    )
    parser.add_argument(
        "--edges-gds", type=Path,
        default=repo / "tech/gds/sram_cell_8t_edges.gds",
    )
    parser.add_argument(
        "--output", type=Path,
        default=repo / "tech/gds/sram_8t_iocolumn.gds",
    )
    parser.add_argument(
        "--verify", type=Path,
        help="verify an existing GDS instead of generating one",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.verify:
            digests = verify_gds(args.verify, args.word_lines)
            print(f"PASS {args.verify}: {len(digests)} routed IO-column cells")
        else:
            library = build_library(
                args.arrays_gds, args.wrappers_gds,
                args.edges_gds, args.word_lines,
            )
            args.output.parent.mkdir(parents=True, exist_ok=True)
            library.write_gds(str(args.output), timestamp=FIXED_GDS_TIMESTAMP)
            digests = verify_gds(args.output, args.word_lines)
            print(f"wrote {args.output}: {len(digests)} routed IO-column cells")
        for name, digest in digests.items():
            print(f"  {name}: sha256={digest}")
        return 0
    except (OSError, RuntimeError, ValueError, StopIteration) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
