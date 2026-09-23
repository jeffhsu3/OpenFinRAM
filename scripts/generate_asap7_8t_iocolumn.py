#!/usr/bin/env python3
"""Generate the ASAP7 dual-port 8T column hierarchy: one IO per end of the bitlines.

Each port has its IO column at its own end of one unsplit array::

    iocol A | edge cap | array, N wordlines ... tap | iocol B

Port A's bitlines leave the array on M2 and port B's on M4, so neither port
crosses the other's IO.  (The earlier floorplan put both IO cores mid-bitline
between two half arrays: port B crossed the A core on M4 and port A climbed to
M6 to cross the B core.)

Each port's IO is the parametric column block from ``chipforge_asap7``
(`IoColumnSpec`): a four-leaf bitline mux group, sense amplifier, write
driver and output latch drawn on the 8T row, with a tap.  It is built here at
the bitcell's own bitline heights, mirrored for port A so its entries face
the array, and abutted to it: the block sits half a fin pitch up the row so
its fins fall on the bitcell's grid (the bitcell centres a fin on its row
boundary, the block a fin space), and the array's bitline bars overhang into
its landings.  Its netlist is written beside the GDS
(``tech/spice/sram_8t_iocolumn.sp``) for ``SpiceGenerator``.

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
import tempfile
from pathlib import Path

import gdspy
import gdstk
from chipforge_asap7.devices import (
    BitlineMuxSpec,
    IoColumnSpec,
    block_netlist,
    build_io_column,
    io_column_pins,
)
from generate_asap7_8t_bitcell import topbot_name


BOUNDARY = 100
FIN = 2
PIN_TEXTTYPE = 251
M1, V1, M2, V2, M3, V3, M4, V4, M5, V5, M6, V6, M7 = (
    19, 21, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70
)
MUX_ROWS = 4
#: ASAP7's fin pitch, and where chipforge's cells (and ASAP7's standard cells)
#: centre their first fin above a row boundary.
FIN_PITCH, LEAF_FIN_PHASE = 27.0, 13.5
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


#: The block's pin for each of the IO column's, per port.
BLOCK_PIN = {
    "PRECHN": "blprechn_{p}", "SAE": "sae_{p}", "D": "D{P}", "Q": "Q{P}",
    "WRENA": "wrena_{p}", "WRENAN": "wrenan_{p}", "OE": "oe_out_{p}", "OEB": "oeb_out_{p}",
}


def iocol_pin_name(block_pin: str, port: str) -> str:
    """The IO column's name for a block pin: ``BL[2]`` -> ``BL_A[2]``, ``PRECHN`` -> ``blprechn_A``."""
    match = re.fullmatch(r"(BL|BLN|YSEL|YSELN)\[(\d+)\]", block_pin)
    if match:
        bus = match.group(1)
        bus = bus if bus.startswith("BL") else bus.lower()
        return f"{bus}_{port}[{match.group(2)}]"
    if block_pin in ("VDD", "VSS"):
        return block_pin
    return BLOCK_PIN[block_pin].format(p=port, P=port)


def iocol_pins(port: str) -> list[str]:
    """Pin order of ``iocol_sram_8t_{a,b}``, as SpiceGenerator instantiates it."""
    return [
        *(f"BL_{port}[{i}]" for i in range(MUX_ROWS)),
        *(f"BLN_{port}[{i}]" for i in range(MUX_ROWS)),
        *(f"ysel_{port}[{i}]" for i in range(MUX_ROWS)),
        *(f"yseln_{port}[{i}]" for i in range(MUX_ROWS)),
        f"blprechn_{port}", f"sae_{port}", f"wrena_{port}", f"wrenan_{port}",
        f"oe_out_{port}", f"oeb_out_{port}", f"D{port}", f"Q{port}", "VDD", "VSS",
    ]


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


def port_io_name(port: str) -> str:
    return f"iocol_sram_8t_{port.lower()}"


def bitline_entries(bitcell: gdstk.Cell) -> dict[str, tuple[float, float]]:
    """Where the bitcell puts each port's bitline pair, in nm from the bottom of its boundary.

    The pair's metal is the bar under its label: port A's on M2, port B's on
    M4.  This is what the block's leaves are built to.
    """
    _, y0, _, _ = boundary_box(bitcell)
    entries = {}
    for port, (true, comp, layer) in {"A": ("BLA", "BLAN", M2), "B": ("BLB", "BLBN", M4)}.items():
        ys = []
        for name in (true, comp):
            label = direct_label(bitcell, name)
            x, y = map(float, label.origin)
            bars = [poly for poly in bitcell.polygons
                    if poly.layer == layer and gdstk.inside([(x, y)], [poly])[0]]
            if len(bars) != 1:
                raise RuntimeError(f"{bitcell.name}: {name} does not sit on one {layer} bar")
            (_, by0), (_, by1) = bars[0].bounding_box()
            ys.append(round(((float(by0) + float(by1)) / 2 - y0) * 1000, 1))
        entries[port] = (ys[0], ys[1])
    return entries


def fin_grid_offset(bitcell: gdstk.Cell) -> float:
    """How far up the bitcell's row a chipforge block sits for its fins to land on the bitcell's grid, nm."""
    _, y0, _, _ = boundary_box(bitcell)
    phases = {round(((bbox(poly)[1] + bbox(poly)[3]) / 2 - y0) * 1000 % FIN_PITCH, 1) % FIN_PITCH
              for poly in bitcell.polygons if poly.layer == FIN}  # fmt: skip
    if len(phases) != 1:
        raise RuntimeError(f"{bitcell.name}: fins are not on one {FIN_PITCH} nm grid: {sorted(phases)}")
    return (LEAF_FIN_PHASE - phases.pop()) % FIN_PITCH


def io_block_specs(bitcell: gdstk.Cell) -> dict[str, IoColumnSpec]:
    """One `IoColumnSpec` per port: the block at the bitcell's bitline heights and on its fin grid, one sense phase."""
    _, y0, _, y1 = boundary_box(bitcell)
    entries = bitline_entries(bitcell)
    offset = fin_grid_offset(bitcell)
    specs = {}
    for port, layer in (("A", "M2"), ("B", "M4")):
        mux = BitlineMuxSpec(rows=2, selects=MUX_ROWS, bitline_entry=entries[port], bitline_layer=layer,
                             grid_offset=offset)  # fmt: skip
        if abs(mux.height / 1000 - (y1 - y0)) > 1e-6:
            raise RuntimeError("the block's two rows do not add up to the bitcell's row")
        specs[port] = IoColumnSpec(mux=mux, one_sense_phase=True)
    return specs


def build_io_blocks(specs: dict[str, IoColumnSpec]) -> tuple[dict[str, gdstk.Cell], str]:
    """Draw both ports' blocks with chipforge (nm), read them back in um; and their netlists.

    Both go into one library so the cells they share (amplifier, driver,
    latch, supports) exist once.
    """
    nm = gdspy.GdsLibrary(unit=1e-9, precision=1e-10)
    gdspy.current_library = nm
    names = {port: build_io_column(spec, lib=nm, draw_pin_labels=False).name for port, spec in specs.items()}
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch) / "io_blocks.gds"
        nm.write_gds(str(path))
        um = gdstk.read_gds(str(path), unit=1e-6)
    cells = {cell.name: cell for cell in um.cells}
    netlists = []
    for port, spec in specs.items():
        block = f"iocol_block_{port.lower()}"
        netlists.append(block_netlist(spec, name=block).replace(".END\n", ""))
        pins = io_column_pins(spec)
        netlists.append(
            f".SUBCKT {port_io_name(port)} {' '.join(iocol_pins(port))}\n"
            f"X_block {' '.join(iocol_pin_name(pin, port) for pin in pins)} {block}\n"
            f".ENDS {port_io_name(port)}\n"
        )
    return {port: cells[name] for port, name in names.items()}, "\n".join(netlists) + ".END\n"


def build_port_io(
    library: gdstk.Library, port: str, block: gdstk.Cell, spec: IoColumnSpec
) -> gdstk.Cell:
    """One port's IO column: its block, abutting the array.

    Port A stands left of the array, so its block is mirrored in x to put the
    bitline entries on its right face; port B stands right of the array with
    the block as drawn.  The block sits `grid_offset` up the column so its
    fins are on the array's; its landings then meet each row's bitline bar,
    which overhangs the array's edge into them.  The cell's boundary is the
    column's, which the block overhangs by that offset at the top.
    """
    width, height = spec.width / 1000, spec.height / 1000
    offset = spec.mux.grid_offset / 1000
    mirrored = port == "A"
    edge_x = width if mirrored else 0.0

    cell = library.new_cell(port_io_name(port))
    if mirrored:
        # A reflection in y then a half turn is a mirror in x.
        cell.add(gdstk.Reference(block, origin=(width, offset), rotation=math.pi, x_reflection=True))
    else:
        cell.add(gdstk.Reference(block, origin=(0.0, offset)))

    def placed(x: float, y: float) -> tuple[float, float]:
        """A block-coordinate point (nm) in the cell (um)."""
        return ((width - x / 1000) if mirrored else x / 1000, offset + y / 1000)

    for pin, (metal, (x, y)) in spec.pin_positions.items():
        base = pin.split(".")[0]
        gds_layer = {"M1": M1, "M2": M2, "M3": M3, "M4": M4}[metal]
        px, py = placed(x, y)
        if base in ("BL", "BLN") or re.fullmatch(r"BLN?\[\d+\]", base):
            cell.add(gdstk.Label(iocol_pin_name(base, port), (edge_x + (-0.006 if mirrored else 0.006), py),
                                 layer=gds_layer, texttype=PIN_TEXTTYPE))
        else:
            cell.add(gdstk.Label(iocol_pin_name(base, port), (px, py), layer=gds_layer, texttype=PIN_TEXTTYPE))

    rect(cell, (0.0, 0.0, width, height), BOUNDARY)
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
        # The IO column's pin sits on the centre of the bitcell's bar; the
        # array's label is wherever the bitcell put its own, within the bar.
        half = 0.009 if layer == M2 else 0.012
        for index, array_label in array_labels.items():
            y = float(array_label.origin[1]) - ary0
            io_label = io_labels[index]
            io_y = float(io_label.origin[1]) - (ay0 if io_cell is io_a else by0)
            if io_label.layer != layer or abs(io_y - y) > half + 1e-6:
                raise RuntimeError(
                    f"{cell.name}: {io_label.text} does not align to {source}[{index}]"
                )
            if io_cell is io_a:
                rect(cell, (cap_x - 0.018, io_y - half, cap_metal_x + 0.018, io_y + half), layer)

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
    edges_gds: Path,
    wordline_counts: list[int],
) -> tuple[gdstk.Library, str]:
    """The column hierarchy for every count, and the IO columns' SPICE."""
    arrays_lib, arrays = load_cells(arrays_gds)
    edges_lib, edges = load_cells(edges_gds)
    units = {(library.unit, library.precision) for library in (arrays_lib, edges_lib)}
    if len(units) != 1:
        raise RuntimeError("8T source GDS units/precision do not match")
    edge_names = {topbot_name(False, my) for my in (False, True)}
    edge_names.update({"FILLER_BLANK_8t", "FILLER_cgedge_8t"})
    bitcell = arrays.get("sram_cell_8t")
    if bitcell is None or not edge_names <= edges.keys():
        raise RuntimeError("8T source GDS is missing a required hard cell")

    unit, precision = units.pop()
    library = gdstk.Library(
        "openfinram_asap7_8t_iocolumn", unit=unit, precision=precision
    )
    library.add(bitcell, *(edges[name] for name in sorted(edge_names)))
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

    specs = io_block_specs(bitcell)
    blocks, netlist = build_io_blocks(specs)
    for block in blocks.values():
        for dep in (block, *block.dependencies(True)):
            if dep not in library.cells:
                library.add(dep)
    io_a = build_port_io(library, "A", blocks["A"], specs["A"])
    io_b = build_port_io(library, "B", blocks["B"], specs["B"])
    cap_array = build_cap_array(library, edges)
    for count in wordline_counts:
        build_colgrp(library, selected_arrays[count], io_a, io_b, cap_array, count)
    return library, netlist


def assert_close(actual: float, expected: float, message: str) -> None:
    if abs(actual - expected) > 1e-6:
        raise RuntimeError(f"{message}: {actual} != {expected}")


def verify_gds(path: Path, wordline_counts: list[int], spice: Path | None = None) -> dict[str, str]:
    library, cells = load_cells(path)
    bitcell = cells.get("sram_cell_8t")
    if bitcell is None:
        raise RuntimeError(f"{path}: missing sram_cell_8t")
    specs = io_block_specs(bitcell)
    expected = {
        "sram_cell_8t", "FILLER_BLANK_8t", "FILLER_cgedge_8t",
        port_io_name("A"), port_io_name("B"), "col_cap_x4_sram_8t",
    }
    expected.update(topbot_name(False, my) for my in (False, True))
    for count in wordline_counts:
        expected.update({
            f"sramcol_x{count}_sram_8t",
            f"array_x{count}x4_sram_8t",
            colgrp_name(count),
        })
    blocks = {spec.cell_name for spec in specs.values()}
    block_cells = {name for name in cells if name.startswith(("iocol_x", "blmux_", "sarow_", "wrdrv_", "outlatch_", "filler_fin", "tap_fin"))}
    if not blocks <= block_cells:
        raise RuntimeError(f"{path}: missing the IO blocks {sorted(blocks - block_cells)}")
    if set(cells) != expected | block_cells:
        raise RuntimeError(
            f"{path}: cell set mismatch; missing={sorted(expected - set(cells))}, "
            f"extra={sorted(set(cells) - expected - block_cells)}"
        )

    io_boxes = {}
    for port, layer in (("A", M2), ("B", M4)):
        io = cells[port_io_name(port)]
        spec = specs[port]
        if [reference.cell_name for reference in io.references] != [spec.cell_name]:
            raise RuntimeError(f"{io.name}: expected exactly its own block, {spec.cell_name}")
        (reference,) = io.references
        mirrored = port == "A"
        if bool(reference.x_reflection) != mirrored or bool(reference.rotation) != mirrored:
            raise RuntimeError(f"{io.name}: port A's block is mirrored in x, port B's as drawn")
        # The block sits half a fin pitch up the column, on the array's fin grid.
        assert_close(float(reference.origin[1]), spec.mux.grid_offset / 1000, f"{io.name}: block fin-grid offset")
        io_boxes[port] = boundary_box(io)
        assert_close(io_boxes[port][3] - io_boxes[port][1], spec.height / 1000, f"{io.name} height")
        assert_close(io_boxes[port][2] - io_boxes[port][0], spec.width / 1000, f"{io.name} width")
        # The bitlines meet the array at the face turned to it, on its layer,
        # where the bitcell puts them.
        face_x = io_boxes[port][2] if mirrored else io_boxes[port][0]
        entry = spec.mux.bitline_entry
        for prefix, y_leaf in (("BL", entry[0]), ("BLN", entry[1])):
            labels = indexed_labels(io, f"{prefix}_{port}")
            if set(labels) != set(range(MUX_ROWS)):
                raise RuntimeError(f"{io.name}: incomplete {prefix}_{port} bus")
            for index, label in labels.items():
                if label.layer != layer:
                    raise RuntimeError(f"{io.name}: {label.text} is on the wrong layer")
                if abs(float(label.origin[0]) - face_x) > 0.0061:
                    raise RuntimeError(f"{io.name}: {label.text} is not at the array face")
                row = spec.mux.height / 1000
                want = (index + 1) * row - y_leaf / 1000 if index % 2 else index * row + y_leaf / 1000
                assert_close(float(label.origin[1]), want, f"{io.name}: {label.text} height")
        # The column draws nothing of its own: the array's bars reach into the block.
        own = {poly.layer for poly in io.polygons} - {BOUNDARY}
        if own:
            raise RuntimeError(f"{io.name}: unexpected routing layers {sorted(own)}")
        names = [label.text for label in io.labels]
        required = set(iocol_pins(port))
        if not required <= set(names):
            raise RuntimeError(f"{io.name}: missing IO pins {sorted(required - set(names))}")
        stale = [n for n in names if re.match(r"(YSEL|BLPRECH|SAPRECHN|WRENA|D_|Q_|yselt|yselb|blprecht|blprechb)", n)]
        if stale:
            raise RuntimeError(f"{io.name}: wrapper-era pins leaked: {stale}")
        if any(n == "VDD" and label.layer != M1 for n, label in zip(names, io.labels)):
            raise RuntimeError(f"{io.name}: a supply label is not on an M1 rail")

    if spice is not None:
        text = spice.read_text()
        for port in ("A", "B"):
            header = f".SUBCKT {port_io_name(port)} {' '.join(iocol_pins(port))}"
            if header not in text:
                raise RuntimeError(f"{spice}: {port_io_name(port)} is missing or its pins are not {header}")
            if f".SUBCKT iocol_block_{port.lower()} " not in text:
                raise RuntimeError(f"{spice}: missing the block netlist of port {port}")

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
        if not name.startswith(("iocol_sram", "colgrp_", "col_cap_")):
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
        "--edges-gds", type=Path,
        default=repo / "tech/gds/sram_cell_8t_edges.gds",
    )
    parser.add_argument(
        "--output", type=Path,
        default=repo / "tech/gds/sram_8t_iocolumn.gds",
    )
    parser.add_argument(
        "--spice-output", type=Path, default=None,
        help="the IO columns' SPICE (default: tech/spice/sram_8t_iocolumn.sp beside the default output)",
    )
    parser.add_argument(
        "--verify", type=Path,
        help="verify an existing GDS instead of generating one",
    )
    parser.add_argument(
        "--verify-spice", type=Path, default=None,
        help="with --verify: the SPICE file that has to go with it",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo = Path(__file__).resolve().parents[1]
    try:
        if args.verify:
            digests = verify_gds(args.verify, args.word_lines, args.verify_spice)
            print(f"PASS {args.verify}: {len(digests)} routed IO-column cells")
        else:
            library, netlist = build_library(args.arrays_gds, args.edges_gds, args.word_lines)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            library.write_gds(str(args.output), timestamp=FIXED_GDS_TIMESTAMP)
            spice = args.spice_output or (
                repo / "tech/spice/sram_8t_iocolumn.sp"
                if args.output == repo / "tech/gds/sram_8t_iocolumn.gds"
                else args.output.with_suffix(".sp")
            )
            spice.parent.mkdir(parents=True, exist_ok=True)
            spice.write_text(netlist)
            digests = verify_gds(args.output, args.word_lines, spice)
            print(f"wrote {args.output} and {spice}: {len(digests)} routed IO-column cells")
        for name, digest in digests.items():
            print(f"  {name}: sha256={digest}")
        return 0
    except (OSError, RuntimeError, ValueError, StopIteration) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
