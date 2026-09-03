#!/usr/bin/env python3
"""Generate parameterized ASAP7 SRAM wordline rows and mux-height arrays.

The published academic 6T library composes its fixed bitcell into
``sramcol_x32/x64/x128`` rows and then stacks four copies into ``array_xNx4``.
This generator preserves that hierarchy for the OpenFinRAM 8T cell and also
provides the same construction for arbitrary 6T wordline counts.

The wordline count and the column-mux height are deliberately independent:
``--word-lines`` controls the number of addressable cells across a row, while
``--mux-rows`` controls the number of bitline pairs presented to each IO mux.
The checked-in 8T IO wrappers have a four-row interface, so their pitch is
cross-checked whenever ``--mux-rows=4``.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import gdstk


BOUNDARY = 100
PIN_TEXTTYPE = 251
FIXED_GDS_TIMESTAMP = dt.datetime(2020, 1, 1, 0, 0, 0)


@dataclass(frozen=True)
class CellContract:
    key: str
    bitcell_name: str
    suffix: str
    wordlines: tuple[str, ...]
    bitlines: tuple[str, ...]
    pin_layers: dict[str, int]


CONTRACTS = (
    CellContract(
        key="8t",
        bitcell_name="sram_cell_8t",
        suffix="sram_8t",
        wordlines=("WLA", "WLB"),
        bitlines=("BLA", "BLAN", "BLB", "BLBN"),
        pin_layers={"WLA": 30, "WLB": 50, "BLA": 20, "BLAN": 20,
                    "BLB": 40, "BLBN": 40},
    ),
    CellContract(
        key="6t",
        bitcell_name="sram_cell_6t_122",
        suffix="sram_6t122",
        wordlines=("WL",),
        bitlines=("BL", "BLN"),
        pin_layers={"WL": 30, "BL": 20, "BLN": 20},
    ),
)


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


def supply_label(cell: gdstk.Cell, prefix: str) -> gdstk.Label:
    labels = [label for label in cell.labels
              if label.text.lower().startswith(prefix.lower())]
    if not labels:
        raise RuntimeError(f"{cell.name}: missing {prefix} supply label")
    return labels[0]


def clone_label(label: gdstk.Label, text: str,
                origin: tuple[float, float]) -> gdstk.Label:
    return gdstk.Label(
        text,
        origin,
        anchor=label.anchor,
        rotation=label.rotation,
        magnification=label.magnification,
        x_reflection=label.x_reflection,
        layer=label.layer,
        texttype=PIN_TEXTTYPE,
    )


def transformed_point(
    point: tuple[float, float],
    origin: tuple[float, float],
    mirror_x: bool = False,
    mirror_y: bool = False,
) -> tuple[float, float]:
    x, y = point
    ox, oy = origin
    return (
        round(ox - x if mirror_x else ox + x, 7),
        round(oy - y if mirror_y else oy + y, 7),
    )


def row_name(contract: CellContract, wordlines: int) -> str:
    return f"sramcol_x{wordlines}_{contract.suffix}"


def array_name(contract: CellContract, wordlines: int, mux_rows: int) -> str:
    return f"array_x{wordlines}x{mux_rows}_{contract.suffix}"


def build_row(
    library: gdstk.Library,
    bitcell: gdstk.Cell,
    contract: CellContract,
    wordlines: int,
) -> gdstk.Cell:
    x0, y0, x1, y1 = boundary_box(bitcell)
    width = x1 - x0
    height = y1 - y0
    row = library.new_cell(row_name(contract, wordlines))

    origins: list[tuple[float, float]] = []
    mirrors: list[bool] = []
    for index in range(wordlines):
        mirror_x = index % 2 == 1
        origin = (
            (index + 1) * width + x0 if mirror_x else index * width - x0,
            -y0,
        )
        row.add(gdstk.Reference(
            bitcell,
            origin=origin,
            rotation=math.pi if mirror_x else 0,
            x_reflection=mirror_x,
        ))
        origins.append(origin)
        mirrors.append(mirror_x)

        for pin in contract.wordlines:
            label = direct_label(bitcell, pin)
            point = transformed_point(
                tuple(label.origin), origin, mirror_x=mirror_x
            )
            row.add(clone_label(label, f"{pin}[{index}]", point))

    # As in the academic sramcol hierarchy, expose each shared bitline at the
    # terminal bitcell.  Mirroring the row later moves these pins to the
    # opposite IO-facing edge without changing the logical net name.
    terminal = wordlines - 1
    for pin in contract.bitlines:
        label = direct_label(bitcell, pin)
        point = transformed_point(
            tuple(label.origin), origins[terminal], mirror_x=mirrors[terminal]
        )
        row.add(clone_label(label, pin, point))

    for source_name, output_name in (("vdd", "VDD"), ("vss", "VSS")):
        label = supply_label(bitcell, source_name)
        point = transformed_point(tuple(label.origin), origins[terminal],
                                  mirror_x=mirrors[terminal])
        row.add(clone_label(label, output_name, point))

    row.add(gdstk.rectangle((0, 0), (wordlines * width, height),
                            layer=BOUNDARY, datatype=0))
    return row


def build_array(
    library: gdstk.Library,
    row: gdstk.Cell,
    contract: CellContract,
    wordlines: int,
    mux_rows: int,
) -> gdstk.Cell:
    x0, y0, x1, y1 = boundary_box(row)
    width = x1 - x0
    height = y1 - y0
    array = library.new_cell(array_name(contract, wordlines, mux_rows))

    row_origins: list[tuple[float, float]] = []
    row_mirrors: list[bool] = []
    for index in range(mux_rows):
        mirror_y = index % 2 == 1
        origin = (
            -x0,
            (index + 1) * height + y0 if mirror_y else index * height - y0,
        )
        array.add(gdstk.Reference(row, origin=origin, x_reflection=mirror_y))
        row_origins.append(origin)
        row_mirrors.append(mirror_y)

    # Wordlines run through every mux row.  Export one label per address and
    # port from the first row; the abutted conductors continue through the
    # remaining row references.
    for pin in contract.wordlines:
        for index in range(wordlines):
            label = direct_label(row, f"{pin}[{index}]")
            point = transformed_point(tuple(label.origin), row_origins[0])
            array.add(clone_label(label, f"{pin}[{index}]", point))

    # Each stacked row has an independent bitline pair selected by its YSEL.
    for row_index in range(mux_rows):
        for pin in contract.bitlines:
            label = direct_label(row, pin)
            point = transformed_point(
                tuple(label.origin),
                row_origins[row_index],
                mirror_y=row_mirrors[row_index],
            )
            array.add(clone_label(label, f"{pin}[{row_index}]", point))

    for source_name in ("VDD", "VSS"):
        label = direct_label(row, source_name)
        for row_index in range(mux_rows):
            point = transformed_point(
                tuple(label.origin),
                row_origins[row_index],
                mirror_y=row_mirrors[row_index],
            )
            array.add(clone_label(label, source_name, point))

    array.add(gdstk.rectangle((0, 0), (width, mux_rows * height),
                              layer=BOUNDARY, datatype=0))
    return array


def load_cell(path: Path, name: str) -> tuple[gdstk.Library, gdstk.Cell]:
    library = gdstk.read_gds(str(path))
    cells = {cell.name: cell for cell in library.cells}
    if name not in cells:
        raise RuntimeError(f"{path}: missing {name}")
    return library, cells[name]


def build_library(
    source_8t: Path,
    source_6t: Path,
    wordline_counts: list[int],
    mux_rows: int,
) -> gdstk.Library:
    source_lib_8t, bitcell_8t = load_cell(source_8t, "sram_cell_8t")
    source_lib_6t, bitcell_6t = load_cell(source_6t, "sram_cell_6t_122")
    if (source_lib_8t.unit, source_lib_8t.precision) != (
        source_lib_6t.unit, source_lib_6t.precision
    ):
        raise RuntimeError("6T and 8T GDS units/precision do not match")

    library = gdstk.Library(
        "openfinram_asap7_wordline_arrays",
        unit=source_lib_8t.unit,
        precision=source_lib_8t.precision,
    )
    library.add(bitcell_8t, bitcell_6t)
    bitcells = {"8t": bitcell_8t, "6t": bitcell_6t}
    for contract in CONTRACTS:
        for count in wordline_counts:
            row = build_row(library, bitcells[contract.key], contract, count)
            build_array(library, row, contract, count, mux_rows)
    return library


def indexed_labels(cell: gdstk.Cell, pin: str) -> dict[int, gdstk.Label]:
    pattern = re.compile(rf"{re.escape(pin)}\[([0-9]+)\]")
    result: dict[int, gdstk.Label] = {}
    for label in cell.labels:
        match = pattern.fullmatch(label.text)
        if match:
            index = int(match.group(1))
            if index in result:
                raise RuntimeError(f"{cell.name}: duplicate {label.text}")
            result[index] = label
    return result


def assert_close(actual: float, expected: float, message: str) -> None:
    if abs(actual - expected) > 1e-6:
        raise RuntimeError(f"{message}: {actual} != {expected}")


def verify_contract(
    cells: dict[str, gdstk.Cell],
    contract: CellContract,
    wordlines: int,
    mux_rows: int,
) -> None:
    bitcell = cells[contract.bitcell_name]
    bx0, by0, bx1, by1 = boundary_box(bitcell)
    pitch_x, pitch_y = bx1 - bx0, by1 - by0
    row = cells[row_name(contract, wordlines)]
    array = cells[array_name(contract, wordlines, mux_rows)]

    rx0, ry0, rx1, ry1 = boundary_box(row)
    ax0, ay0, ax1, ay1 = boundary_box(array)
    for actual, expected, description in (
        (rx0, 0, "row x0"), (ry0, 0, "row y0"),
        (rx1, wordlines * pitch_x, "row width"),
        (ry1, pitch_y, "row height"),
        (ax0, 0, "array x0"), (ay0, 0, "array y0"),
        (ax1, wordlines * pitch_x, "array width"),
        (ay1, mux_rows * pitch_y, "array height"),
    ):
        assert_close(actual, expected, f"{array.name}: {description}")

    if len(row.references) != wordlines:
        raise RuntimeError(f"{row.name}: expected {wordlines} bitcell references")
    if any(reference.cell_name != contract.bitcell_name
           for reference in row.references):
        raise RuntimeError(f"{row.name}: contains a non-bitcell reference")
    if len(array.references) != mux_rows:
        raise RuntimeError(f"{array.name}: expected {mux_rows} row references")
    if any(reference.cell_name != row.name for reference in array.references):
        raise RuntimeError(f"{array.name}: contains a non-row reference")

    for pin in contract.wordlines:
        labels = indexed_labels(row, pin)
        if set(labels) != set(range(wordlines)):
            raise RuntimeError(f"{row.name}: incomplete {pin} wordline bus")
        array_labels = indexed_labels(array, pin)
        if set(array_labels) != set(range(wordlines)):
            raise RuntimeError(f"{array.name}: incomplete {pin} wordline bus")
        if any(label.layer != contract.pin_layers[pin]
               for label in (*labels.values(), *array_labels.values())):
            raise RuntimeError(f"{array.name}: {pin} is on the wrong layer")

    for pin in contract.bitlines:
        direct_label(row, pin)
        labels = indexed_labels(array, pin)
        if set(labels) != set(range(mux_rows)):
            raise RuntimeError(f"{array.name}: incomplete {pin} bitline bus")
        if any(label.layer != contract.pin_layers[pin]
               for label in labels.values()):
            raise RuntimeError(f"{array.name}: {pin} is on the wrong layer")


def verify_io_pitch(cells: dict[str, gdstk.Cell], io_gds: Path,
                    wordlines: int, mux_rows: int) -> None:
    if mux_rows != 4:
        return
    io_lib = gdstk.read_gds(str(io_gds))
    io_cells = {cell.name: cell for cell in io_lib.cells}
    array = cells[array_name(CONTRACTS[0], wordlines, mux_rows)]
    mappings = {
        "ioprech_sram_8t_a": {
            "BLA": "BLT_A", "BLAN": "BLTN_A",
        },
        "ioprech_sram_8t_b": {
            "BLB": "BLT_B", "BLBN": "BLTN_B",
        },
    }
    for io_name, pins in mappings.items():
        if io_name not in io_cells:
            raise RuntimeError(f"{io_gds}: missing {io_name}")
        io = io_cells[io_name]
        assert_close(boundary_box(io)[3], boundary_box(array)[3],
                     f"{io_name}: array/IO height")
        for array_pin, io_pin in pins.items():
            array_labels = indexed_labels(array, array_pin)
            io_labels = indexed_labels(io, io_pin)
            if set(array_labels) != set(io_labels):
                raise RuntimeError(f"{io_name}: incomplete {io_pin} interface")
            for index in array_labels:
                assert_close(float(array_labels[index].origin[1]),
                             float(io_labels[index].origin[1]),
                             f"{io_name}: {io_pin}[{index}] pitch")
                if array_labels[index].layer != io_labels[index].layer:
                    raise RuntimeError(f"{io_name}: {io_pin}[{index}] layer mismatch")


def cell_digest(cell: gdstk.Cell) -> str:
    records: list[str] = []
    for polygon in cell.polygons:
        records.append(f"P:{polygon.layer}:{polygon.datatype}:{bbox(polygon)}")
    for label in cell.labels:
        records.append(
            f"L:{label.text}:{label.layer}:{label.texttype}:"
            f"{tuple(round(float(v), 7) for v in label.origin)}"
        )
    for reference in cell.references:
        records.append(
            f"R:{reference.cell_name}:"
            f"{tuple(round(float(v), 7) for v in reference.origin)}:"
            f"{round(float(reference.rotation or 0), 7)}:"
            f"{int(reference.x_reflection)}"
        )
    return hashlib.sha256("\n".join(sorted(records)).encode()).hexdigest()


def verify_gds(path: Path, wordline_counts: list[int], mux_rows: int,
               io_gds: Path) -> dict[str, str]:
    library = gdstk.read_gds(str(path))
    cells = {cell.name: cell for cell in library.cells}
    expected = {contract.bitcell_name for contract in CONTRACTS}
    for contract in CONTRACTS:
        for count in wordline_counts:
            expected.add(row_name(contract, count))
            expected.add(array_name(contract, count, mux_rows))
    if set(cells) != expected:
        missing = sorted(expected - set(cells))
        extra = sorted(set(cells) - expected)
        raise RuntimeError(f"{path}: cell set mismatch; missing={missing}, extra={extra}")

    for contract in CONTRACTS:
        for count in wordline_counts:
            verify_contract(cells, contract, count, mux_rows)
            if contract.key == "8t":
                verify_io_pitch(cells, io_gds, count, mux_rows)
    return {name: cell_digest(cells[name]) for name in sorted(cells)
            if name.startswith(("sramcol_", "array_"))}


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
        "--word-lines",
        type=parse_wordlines,
        default=parse_wordlines("2,32,64,128"),
        help="comma-separated wordline counts (default: 2,32,64,128)",
    )
    parser.add_argument(
        "--mux-rows", type=int, default=4,
        help="number of vertically stacked rows per IO mux (default: 4)",
    )
    parser.add_argument(
        "--8t-gds", dest="gds_8t", type=Path,
        default=repo / "tech/gds/sram_cell_8t.gds",
    )
    parser.add_argument(
        "--6t-gds", dest="gds_6t", type=Path,
        default=repo / "tech/gds/srambank_32b_boundary_2.gds",
    )
    parser.add_argument(
        "--io-gds", type=Path,
        default=repo / "tech/gds/sram_8t_ioprech.gds",
        help="8T IO wrappers used for four-row pitch verification",
    )
    parser.add_argument(
        "--output", type=Path,
        default=repo / "tech/gds/sram_wordline_arrays.gds",
    )
    parser.add_argument(
        "--verify", type=Path,
        help="verify an existing GDS instead of generating one",
    )
    args = parser.parse_args(argv)
    if args.mux_rows < 1:
        parser.error("--mux-rows must be positive")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.verify:
            digests = verify_gds(
                args.verify, args.word_lines, args.mux_rows, args.io_gds
            )
            print(f"PASS {args.verify}: {len(digests)} parameterized cells")
        else:
            library = build_library(
                args.gds_8t, args.gds_6t, args.word_lines, args.mux_rows
            )
            args.output.parent.mkdir(parents=True, exist_ok=True)
            library.write_gds(str(args.output), timestamp=FIXED_GDS_TIMESTAMP)
            digests = verify_gds(
                args.output, args.word_lines, args.mux_rows, args.io_gds
            )
            print(f"wrote {args.output}: {len(digests)} parameterized cells")
        for name, digest in digests.items():
            print(f"  {name}: sha256={digest}")
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
