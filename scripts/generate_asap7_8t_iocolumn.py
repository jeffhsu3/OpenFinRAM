#!/usr/bin/env python3
"""Generate routed ASAP7 dual-port 8T IO-column hierarchy.

The port-A and port-B hard wrappers cannot be overlaid: both contain a full
copy of the differential IO core.  This generator places them side by side
and joins both to the same pair of 8T arrays.  Port B passes across the A core
on M4 (the A wrapper has no M4); port A rises from M2 to M6 in a dedicated
inter-wrapper gap, crosses the B core, and drops back to M2 in a right gap.

The resulting ``colgrp_x{2N}x4_sram_8t`` cells match the logical
``colgrp_sram_8t`` hierarchy emitted by ``SpiceGenerator``.  N is the number
of wordlines in each half-array and may be any positive value present in the
parameterized wordline-array input GDS.
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


def direct_label(cell: gdstk.Cell, name: str) -> gdstk.Label:
    labels = [label for label in cell.labels if label.text == name]
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


def add_m2_to_m6_stack(
    cell: gdstk.Cell,
    routes: dict[str, list[tuple[int, tuple[float, float, float, float]]]],
    net: str,
    bottom: tuple[float, float],
    x_direction: int,
    y_direction: int,
    x_step: float = 0.080,
) -> tuple[float, float]:
    """Build a direction-legal ASAP7 M2-to-M6 staircase.

    M4/M6 are horizontal-only and M5 is vertical-only in the public rules.
    Staggering the transitions lets V2 use an 18 nm vertical M3 landing while
    V3 uses a 24 nm horizontal M3 landing, and avoids bends on M4--M6.
    """
    if x_direction not in (-1, 1) or y_direction not in (-1, 1):
        raise RuntimeError("via-stack directions must be -1 or 1")

    x2, y2 = bottom
    x3, y3 = x2, y2 + y_direction * 0.050
    x4, y4 = x3 + x_direction * x_step, y3
    x5, y5 = x4, y4 + y_direction * 0.050

    # V2: 18 x 18 nm.  M2 supplies horizontal endcaps and M3 leaves the via
    # on a vertical 18 nm track, as required by V2.M3.AUX.2.
    add_route_shape(cell, routes, net, M2,
                    (x2 - 0.014, y2 - 0.009, x2 + 0.014, y2 + 0.009))
    add_route_shape(cell, routes, net, V2,
                    (x2 - 0.009, y2 - 0.009, x2 + 0.009, y2 + 0.009))
    add_route_shape(
        cell, routes, net, M3,
        (x2 - 0.009, min(y2, y3) - 0.014,
         x2 + 0.009, max(y2, y3) + 0.014),
    )

    # V3: 18 x 24 nm.  The local M3 bar gives 5 nm horizontal endcaps;
    # the straight 24 nm M4 segment gives 11 nm horizontal endcaps.
    add_route_shape(cell, routes, net, M3,
                    (x3 - 0.014, y3 - 0.012, x3 + 0.014, y3 + 0.012))
    add_route_shape(cell, routes, net, V3,
                    (x3 - 0.009, y3 - 0.012, x3 + 0.009, y3 + 0.012))
    add_route_shape(
        cell, routes, net, M4,
        (min(x3 - 0.020, x4 - 0.023), y3 - 0.012,
         max(x3 + 0.020, x4 + 0.023), y3 + 0.012),
    )

    # V4 is 24 x 24 nm between horizontal M4 and vertical M5.  V5 is
    # 24 x 32 nm between vertical M5 and horizontal M6.  The latter follows
    # V5.M6.AUX.2 in the DRM (32 nm perpendicular to horizontal M6); the
    # public ORFS KLayout deck currently checks M6's vertical edges instead.
    add_route_shape(cell, routes, net, V4,
                    (x4 - 0.012, y4 - 0.012, x4 + 0.012, y4 + 0.012))
    add_route_shape(
        cell, routes, net, M5,
        (x4 - 0.012, min(y4 - 0.023, y5 - 0.027),
         x4 + 0.012, max(y4 + 0.023, y5 + 0.027)),
    )
    add_route_shape(cell, routes, net, V5,
                    (x5 - 0.012, y5 - 0.016, x5 + 0.012, y5 + 0.016))
    add_route_shape(cell, routes, net, M6,
                    (x5 - 0.023, y5 - 0.016, x5 + 0.023, y5 + 0.016))
    return x5, y5


def wrapper_control_name(name: str) -> str | None:
    scalar = {
        "WRENA_A": "wrena_A",
        "WRENAN_A": "wrenan_A",
        "WRENA_B": "wrena_B",
        "WRENAN_B": "wrenan_B",
        "D_A": "DA",
        "D_B": "DB",
        "Q_A": "QA",
        "OEB_OUT_A": "oeb_out_A",
        "OE_OUT_A": "oe_out_A",
        "BLPRECHTN_A": "blprechtn_A",
        "BLPRECHBN_A": "blprechbn_A",
        "SAE_A": "sae_A",
        "Q_B": "QB",
        "OEB_OUT_B": "oeb_out_B",
        "OE_OUT_B": "oe_out_B",
        "BLPRECHTN_B": "blprechtn_B",
        "BLPRECHBN_B": "blprechbn_B",
        "SAE_B": "sae_B",
    }
    if name in scalar:
        return scalar[name]
    match = re.fullmatch(r"(YSELTN|YSELT|YSELBN|YSELB)_([AB])\[([0-3])\]", name)
    if match:
        return f"{match.group(1).lower()}_{match.group(2)}[{match.group(3)}]"
    return None


def colgrp_control_name(name: str) -> str:
    replacements = {
        "wrena_A": "wrenaA",
        "wrenan_A": "wrenanA",
        "wrena_B": "wrenaB",
        "wrenan_B": "wrenanB",
        "oeb_out_A": "oeb_outA",
        "oe_out_A": "oe_outA",
        "oeb_out_B": "oeb_outB",
        "oe_out_B": "oe_outB",
        "blprechtn_A": "blprechtnA",
        "blprechbn_A": "blprechbnA",
        "blprechtn_B": "blprechtnB",
        "blprechbn_B": "blprechbnB",
    }
    if name in replacements:
        return replacements[name]
    match = re.fullmatch(r"(yseltn|yselt|yselbn|yselb)_([AB])\[([0-3])\]", name)
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


def build_combined_io(
    library: gdstk.Library,
    wrapper_a: gdstk.Cell,
    wrapper_b: gdstk.Cell,
) -> gdstk.Cell:
    ax0, ay0, ax1, ay1 = boundary_box(wrapper_a)
    bx0, by0, bx1, by1 = boundary_box(wrapper_b)
    if (ax0, ay0, ax1, ay1) != (bx0, by0, bx1, by1):
        raise RuntimeError("port-A and port-B wrapper boundaries differ")
    wrapper_width = ax1 - ax0
    wrapper_height = ay1 - ay0
    if wrapper_height <= 0 or wrapper_width <= 0:
        raise RuntimeError("invalid 8T IO wrapper boundary")
    if any(poly.layer == M4 for poly in wrapper_a.polygons):
        raise RuntimeError("port-A wrapper unexpectedly occupies M4 pass-through layer")
    if any(poly.layer == M5 for wrapper in (wrapper_a, wrapper_b)
           for poly in wrapper.polygons):
        raise RuntimeError("8T wrapper unexpectedly occupies M5 crossover layer")

    a_x = GAP_WIDTH
    center_x0 = a_x + wrapper_width
    b_x = center_x0 + GAP_WIDTH
    right_x0 = b_x + wrapper_width
    total_width = right_x0 + GAP_WIDTH
    lift_x = center_x0 + 0.026
    drop_x = right_x0 + GAP_WIDTH - 0.026

    cell = library.new_cell("iocolgrp_sram_8t")
    cell.add(gdstk.Reference(wrapper_a, origin=(a_x - ax0, -ay0)))
    cell.add(gdstk.Reference(wrapper_b, origin=(b_x - bx0, -by0)))
    routes: dict[str, list[tuple[int, tuple[float, float, float, float]]]] = {}

    # Left array interface: A reaches its adjacent core on M2; B crosses the A
    # core on M4, a layer absent from that wrapper.
    for prefix in ("BLT", "BLTN"):
        for index, label in indexed_labels(wrapper_a, f"{prefix}_A").items():
            y = float(label.origin[1]) - ay0
            net = f"{prefix}_A[{index}]"
            add_route_shape(cell, routes, net, M2,
                            (0.0, y - 0.009, a_x + 0.018, y + 0.009))
            cell.add(clone_label(label, net, (0.006, y), M2))
    for prefix in ("BLT", "BLTN"):
        for index, label in indexed_labels(wrapper_b, f"{prefix}_B").items():
            y = float(label.origin[1]) - by0
            net = f"{prefix}_B[{index}]"
            add_route_shape(cell, routes, net, M4,
                            (0.0, y - 0.012, b_x + 0.018, y + 0.012))
            cell.add(clone_label(label, net, (0.006, y), M4))

    # Right array interface: B reaches its adjacent core on M4.  A lifts to M6
    # in the central gap, crosses the B core, then drops in the right gap.
    for prefix in ("BLB", "BLBN"):
        for index, label in indexed_labels(wrapper_b, f"{prefix}_B").items():
            y = float(label.origin[1]) - by0
            net = f"{prefix}_B[{index}]"
            add_route_shape(cell, routes, net, M4,
                            (right_x0 - 0.018, y - 0.012,
                             total_width, y + 0.012))
            cell.add(clone_label(label, net, (total_width - 0.006, y), M4))
    ordered_a_right_y = sorted(
        float(item.origin[1]) - ay0
        for item_prefix in ("BLB", "BLBN")
        for item in indexed_labels(wrapper_a, f"{item_prefix}_A").values()
    )
    for prefix in ("BLB", "BLBN"):
        for index, label in indexed_labels(wrapper_a, f"{prefix}_A").items():
            y = float(label.origin[1]) - ay0
            net = f"{prefix}_A[{index}]"
            # Close true/complement tracks use alternating vertical M5 lanes.
            # Their 48 nm center separation leaves the required 24 nm gap
            # between 24 nm M5 wires while both staircases rise northward.
            route_rank = ordered_a_right_y.index(y)
            x_step = 0.080 if route_rank % 2 == 0 else 0.032
            add_route_shape(cell, routes, net, M2,
                            (center_x0 - 0.018, y - 0.009,
                             lift_x + 0.014, y + 0.009))
            lift_top = add_m2_to_m6_stack(
                cell, routes, net, (lift_x, y), 1, 1, x_step
            )
            drop_top = add_m2_to_m6_stack(
                cell, routes, net, (drop_x, y), -1, 1, x_step
            )
            add_route_shape(cell, routes, net, M6,
                            (lift_top[0], lift_top[1] - 0.016,
                             drop_top[0], lift_top[1] + 0.016))
            add_route_shape(cell, routes, net, M2,
                            (drop_x - 0.014, y - 0.009,
                             total_width, y + 0.009))
            cell.add(clone_label(label, net, (total_width - 0.006, y), M2))

    # Export the active controls.  SAE and SAPRECHN are intentionally shorted
    # per port, matching the current composite SPICE phase convention.
    for wrapper, x_offset in ((wrapper_a, a_x), (wrapper_b, b_x)):
        port = "A" if wrapper is wrapper_a else "B"
        sae = direct_label(wrapper, f"SAE_{port}")
        saprechn = direct_label(wrapper, f"SAPRECHN_{port}")
        sae_point = (x_offset + float(sae.origin[0]) - ax0,
                     float(sae.origin[1]) - ay0)
        sap_point = (x_offset + float(saprechn.origin[0]) - ax0,
                     float(saprechn.origin[1]) - ay0)
        if abs(sae_point[1] - sap_point[1]) > 1e-9:
            raise RuntimeError(f"port {port} sense controls are not row-aligned")
        add_route_shape(
            cell, routes, f"sae_{port}", M3,
            (min(sae_point[0], sap_point[0]) - 0.008,
             sae_point[1] - 0.009,
             max(sae_point[0], sap_point[0]) + 0.008,
             sae_point[1] + 0.009),
        )

        for label in wrapper.labels:
            renamed = wrapper_control_name(label.text)
            if renamed is None:
                continue
            point = (x_offset + float(label.origin[0]) - ax0,
                     float(label.origin[1]) - ay0)
            cell.add(clone_label(label, renamed, point))

    # Export one direct label per supply from the existing wrapper rails.  The
    # macro-level PG network connects both wrapper instances; no functional IO
    # pin is consumed as a local supply tie in the true 2RW interface.
    vss_source = max(
        (label for label in wrapper_b.labels
         if label.text == "VSS" and label.layer == M1),
        key=lambda label: float(label.origin[0]),
    )
    vdd_source = next(
        label for label in wrapper_b.labels
        if label.text == "VDD" and label.layer == M1
    )
    vss_point = (b_x + float(vss_source.origin[0]) - bx0,
                 float(vss_source.origin[1]) - by0)
    vdd_point = (b_x + float(vdd_source.origin[0]) - bx0,
                 float(vdd_source.origin[1]) - by0)
    cell.add(clone_label(vss_source, "VSS", vss_point, M1))
    cell.add(clone_label(vdd_source, "VDD", vdd_point, M1))

    validate_route_spacing(routes)
    rect(cell, (0.0, 0.0, total_width, wrapper_height), BOUNDARY)
    return cell


def build_cap_array(library: gdstk.Library, cap: gdstk.Cell) -> gdstk.Cell:
    x0, y0, x1, y1 = boundary_box(cap)
    width, height = x1 - x0, y1 - y0
    cell = library.new_cell("col_cap_x4_sram_8t")
    for row in range(MUX_ROWS):
        reflected = row % 2 == 1
        origin = (-x0, (row + 1) * height + y0 if reflected
                  else row * height - y0)
        cell.add(gdstk.Reference(cap, origin=origin, x_reflection=reflected))
    rect(cell, (0.0, 0.0, width, MUX_ROWS * height), BOUNDARY)
    return cell


def colgrp_name(wordlines: int) -> str:
    return f"colgrp_x{2 * wordlines}x4_sram_8t"


def build_colgrp(
    library: gdstk.Library,
    array: gdstk.Cell,
    combined_io: gdstk.Cell,
    cap_array: gdstk.Cell,
    wordlines: int,
) -> gdstk.Cell:
    arx0, ary0, arx1, ary1 = boundary_box(array)
    iox0, ioy0, iox1, ioy1 = boundary_box(combined_io)
    cpx0, cpy0, cpx1, cpy1 = boundary_box(cap_array)
    array_width, array_height = arx1 - arx0, ary1 - ary0
    io_width, io_height = iox1 - iox0, ioy1 - ioy0
    cap_width, cap_height = cpx1 - cpx0, cpy1 - cpy0
    if abs(array_height - io_height) > 1e-6 or abs(cap_height - io_height) > 1e-6:
        raise RuntimeError("8T array, IO, and edge-cap heights do not match")

    left_array_x = cap_width
    io_x = left_array_x + array_width
    right_array_x = io_x + io_width
    right_cap_x = right_array_x + array_width
    total_width = right_cap_x + cap_width
    cell = library.new_cell(colgrp_name(wordlines))
    cell.add(gdstk.Reference(cap_array, origin=(-cpx0, -cpy0)))
    cell.add(gdstk.Reference(array,
                             origin=(left_array_x - arx0, -ary0)))
    cell.add(gdstk.Reference(combined_io,
                             origin=(io_x - iox0, -ioy0)))
    cell.add(gdstk.Reference(
        array,
        origin=(right_array_x + array_width + arx0, -ary0),
        rotation=math.pi,
        x_reflection=True,
    ))
    cell.add(gdstk.Reference(
        cap_array,
        origin=(total_width + cpx0, -cpy0),
        rotation=math.pi,
        x_reflection=True,
    ))

    wordline_map = {
        "WLA": ("WLTA", "WLBA", M3),
        "WLB": ("WLTB", "WLBB", M5),
    }
    for source, (left_name, right_name, layer) in wordline_map.items():
        for index, label in indexed_labels(array, source).items():
            x, y = map(float, label.origin)
            cell.add(clone_label(
                label, f"{left_name}[{index}]",
                (left_array_x + x - arx0, y - ary0), layer,
            ))
            cell.add(clone_label(
                label, f"{right_name}[{index}]",
                (right_array_x + array_width - (x - arx0), y - ary0), layer,
            ))

    bitline_map = {
        "BLA": ("BLT_A", "BLB_A", M2),
        "BLAN": ("BLTN_A", "BLBN_A", M2),
        "BLB": ("BLT_B", "BLB_B", M4),
        "BLBN": ("BLTN_B", "BLBN_B", M4),
    }
    for source, (left_io, right_io, layer) in bitline_map.items():
        array_labels = indexed_labels(array, source)
        left_labels = indexed_labels(combined_io, left_io)
        right_labels = indexed_labels(combined_io, right_io)
        if set(array_labels) != set(left_labels) or set(array_labels) != set(right_labels):
            raise RuntimeError(f"{cell.name}: incomplete {source} interface")
        for index, array_label in array_labels.items():
            y = float(array_label.origin[1]) - ary0
            for io_label in (left_labels[index], right_labels[index]):
                if io_label.layer != layer or abs(float(io_label.origin[1]) - y) > 1e-6:
                    raise RuntimeError(
                        f"{cell.name}: {io_label.text} does not align to {source}[{index}]"
                    )

    bitline_prefixes = (
        "BLT_A", "BLTN_A", "BLB_A", "BLBN_A",
        "BLT_B", "BLTN_B", "BLB_B", "BLBN_B",
    )
    for label in combined_io.labels:
        if label.text.startswith(bitline_prefixes) or label.text in {"VDD", "VSS"}:
            continue
        point = (io_x + float(label.origin[0]) - iox0,
                 float(label.origin[1]) - ioy0)
        cell.add(clone_label(label, colgrp_control_name(label.text), point))
    for supply in ("VDD", "VSS"):
        label = direct_label(combined_io, supply)
        point = (io_x + float(label.origin[0]) - iox0,
                 float(label.origin[1]) - ioy0)
        cell.add(clone_label(label, supply, point))

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
    cap = edges.get("sram_cell_8t_col_cap")
    bitcell = arrays.get("sram_cell_8t")
    if None in (wrapper_a, wrapper_b, cap, bitcell):
        raise RuntimeError("8T source GDS is missing a required hard cell")

    unit, precision = units.pop()
    library = gdstk.Library(
        "openfinram_asap7_8t_iocolumn", unit=unit, precision=precision
    )
    library.add(bitcell, cap, wrapper_a, wrapper_b)
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

    combined_io = build_combined_io(library, wrapper_a, wrapper_b)
    cap_array = build_cap_array(library, cap)
    for count in wordline_counts:
        build_colgrp(
            library, selected_arrays[count], combined_io, cap_array, count
        )
    return library


def assert_close(actual: float, expected: float, message: str) -> None:
    if abs(actual - expected) > 1e-6:
        raise RuntimeError(f"{message}: {actual} != {expected}")


def verify_gds(path: Path, wordline_counts: list[int]) -> dict[str, str]:
    library, cells = load_cells(path)
    expected = {
        "sram_cell_8t", "sram_cell_8t_col_cap",
        "ioprech_sram_8t_a", "ioprech_sram_8t_b",
        "iocolgrp_sram_8t", "col_cap_x4_sram_8t",
    }
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

    io = cells["iocolgrp_sram_8t"]
    if len(io.references) != 2:
        raise RuntimeError("iocolgrp_sram_8t: expected two wrapper references")
    if {reference.cell_name for reference in io.references} != {
        "ioprech_sram_8t_a", "ioprech_sram_8t_b"
    }:
        raise RuntimeError("iocolgrp_sram_8t: wrong wrapper references")
    io_box = boundary_box(io)
    assert_close(io_box[3] - io_box[1], 2.376, "combined IO height")
    for prefix, layer in (
        ("BLT_A", M2), ("BLTN_A", M2), ("BLB_A", M2), ("BLBN_A", M2),
        ("BLT_B", M4), ("BLTN_B", M4), ("BLB_B", M4), ("BLBN_B", M4),
    ):
        labels = indexed_labels(io, prefix)
        if set(labels) != set(range(MUX_ROWS)):
            raise RuntimeError(f"iocolgrp_sram_8t: incomplete {prefix} bus")
        if any(label.layer != layer for label in labels.values()):
            raise RuntimeError(f"iocolgrp_sram_8t: {prefix} is on wrong layer")
    io_names = {label.text for label in io.labels}
    required_io = {
        "wrena_A", "wrenan_A", "wrena_B", "wrenan_B",
        "DA", "QA", "DB", "QB",
        "oeb_out_A", "oe_out_A", "oeb_out_B", "oe_out_B",
        "blprechtn_A", "blprechbn_A", "blprechtn_B", "blprechbn_B",
        "sae_A", "sae_B", "VDD", "VSS",
    }
    required_io.update(
        f"{prefix}_{port}[{index}]"
        for prefix in ("yseltn", "yselt", "yselbn", "yselb")
        for port in ("A", "B") for index in range(MUX_ROWS)
    )
    if not required_io.issubset(io_names):
        raise RuntimeError(
            f"iocolgrp_sram_8t: missing IO pins {sorted(required_io - io_names)}"
        )
    if io_names.intersection({"WRENA_B", "WRENAN_B", "D_B", "SAPRECHN_A", "SAPRECHN_B"}):
        raise RuntimeError("iocolgrp_sram_8t: internal-only wrapper pins leaked")
    for layer in (M6, V5, M5, V4, M4, V3, M3, V2, M2):
        if not any(poly.layer == layer for poly in io.polygons):
            raise RuntimeError(f"iocolgrp_sram_8t: missing port-A crossover layer {layer}")
    if any(poly.layer in (V6, M7) for poly in io.polygons):
        raise RuntimeError("iocolgrp_sram_8t: crossover unexpectedly exceeds M6")
    m1_geometry = io.get_polygons(layer=M1, datatype=0)
    for supply in ("VDD", "VSS"):
        label = direct_label(io, supply)
        if label.layer != M1:
            raise RuntimeError(f"iocolgrp_sram_8t: {supply} is not on a wrapper M1 rail")
        x, y = map(float, label.origin)
        if not gdstk.inside([(x, y)], m1_geometry)[0]:
            raise RuntimeError(f"iocolgrp_sram_8t: {supply} label misses wrapper M1")

    cap_array = cells["col_cap_x4_sram_8t"]
    if len(cap_array.references) != MUX_ROWS:
        raise RuntimeError("col_cap_x4_sram_8t: expected four cap references")
    for count in wordline_counts:
        array = cells[f"array_x{count}x4_sram_8t"]
        colgrp = cells[colgrp_name(count)]
        array_box = boundary_box(array)
        colgrp_box = boundary_box(colgrp)
        cap_box = boundary_box(cap_array)
        expected_width = (
            2 * (array_box[2] - array_box[0])
            + (io_box[2] - io_box[0])
            + 2 * (cap_box[2] - cap_box[0])
        )
        assert_close(colgrp_box[2] - colgrp_box[0], expected_width,
                     f"{colgrp.name}: width")
        assert_close(colgrp_box[3] - colgrp_box[1], 2.376,
                     f"{colgrp.name}: height")
        if len(colgrp.references) != 5:
            raise RuntimeError(f"{colgrp.name}: expected cap/array/IO/array/cap")
        expected_refs = [
            "col_cap_x4_sram_8t", f"array_x{count}x4_sram_8t",
            "iocolgrp_sram_8t", f"array_x{count}x4_sram_8t",
            "col_cap_x4_sram_8t",
        ]
        if [reference.cell_name for reference in colgrp.references] != expected_refs:
            raise RuntimeError(f"{colgrp.name}: wrong placement hierarchy")
        for prefix, layer in (
            ("WLTA", M3), ("WLTB", M5), ("WLBA", M3), ("WLBB", M5),
        ):
            labels = indexed_labels(colgrp, prefix)
            if set(labels) != set(range(count)):
                raise RuntimeError(f"{colgrp.name}: incomplete {prefix} bus")
            if any(label.layer != layer for label in labels.values()):
                raise RuntimeError(f"{colgrp.name}: {prefix} is on wrong layer")
        required_colgrp = {
            "DA", "QA", "DB", "QB",
            "wrenaA", "wrenanA", "wrenaB", "wrenanB",
            "oeb_outA", "oe_outA", "oeb_outB", "oe_outB",
            "blprechtnA", "blprechbnA", "blprechtnB", "blprechbnB",
            "sae_A", "sae_B", "VDD", "VSS",
        }
        required_colgrp.update(
            f"{prefix}{port}[{index}]"
            for prefix in ("yseltn", "yselt", "yselbn", "yselb")
            for port in ("A", "B") for index in range(MUX_ROWS)
        )
        names = {label.text for label in colgrp.labels}
        if not required_colgrp.issubset(names):
            raise RuntimeError(
                f"{colgrp.name}: missing pins {sorted(required_colgrp - names)}"
            )

    digests: dict[str, str] = {}
    for name in sorted(cells):
        if not name.startswith(("iocolgrp_", "colgrp_", "col_cap_")):
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
        help="comma-separated half-array wordline counts (default: 2,32,64,128)",
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
