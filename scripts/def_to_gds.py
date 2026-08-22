#!/usr/bin/env python3
"""Stream an OpenROAD DEF to GDS with full ASAP7 standard-cell geometry.

KLayout reads the technology and cell LEFs for DEF semantics, substitutes
matching LEF macros from the standard-cell GDS, and writes a hierarchical GDS.
The resulting top cell contains routed metal, vias, physical pin shapes, labels,
and references to the actual standard-cell layouts.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys


def _load_klayout_db():
    try:
        import klayout.db as kdb

        return kdb
    except ImportError as first_error:
        candidates = [
            Path("/usr/lib/klayout/pymod"),
            Path("/usr/local/lib/klayout/pymod"),
        ]
        candidates.extend(Path("/usr/lib").glob("*/klayout/pymod"))
        for candidate in candidates:
            if not candidate.is_dir():
                continue
            sys.path.insert(0, str(candidate))
            try:
                import klayout.db as kdb

                return kdb
            except ImportError:
                sys.path.pop(0)

        raise RuntimeError(
            "KLayout's Python module is unavailable. Install the KLayout "
            "Python bindings or make klayout.db importable by python3."
        ) from first_error


ASAP7_GDS_LAYERS = {
    "V0": 18,
    "M1": 19,
    "M2": 20,
    "V1": 21,
    "V2": 25,
    "M3": 30,
    "V3": 35,
    "M4": 40,
    "V4": 45,
    "M5": 50,
    "V5": 55,
    "M6": 60,
    "V6": 65,
    "M7": 70,
    "V7": 75,
    "M8": 80,
    "V8": 85,
    "M9": 90,
    "V9": 95,
    "Pad": 95,
}


def _existing_file(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"file does not exist: {path}")
    return path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert an OpenROAD DEF to a merged ASAP7 GDS using KLayout"
    )
    parser.add_argument("--def-file", required=True, type=_existing_file)
    parser.add_argument("--tech-lef", required=True, type=_existing_file)
    parser.add_argument("--cell-lef", required=True, type=_existing_file)
    parser.add_argument("--macro-gds", required=True, type=_existing_file)
    parser.add_argument("--output-gds", required=True, type=Path)
    parser.add_argument("--top-cell", default="ctrl_decode")
    return parser.parse_args()


def stream_def_to_gds(args: argparse.Namespace) -> None:
    kdb = _load_klayout_db()

    layer_map_text = "\n".join(
        f"{name} : {number}/0" for name, number in ASAP7_GDS_LAYERS.items()
    )

    options = kdb.LoadLayoutOptions()
    config = options.lefdef_config
    # Preserve the 0.25 nm grid used by the ASAP7 standard-cell GDS. DEF
    # coordinates are on a coarser 1 nm grid and map exactly onto this grid.
    config.dbu = 0.00025
    config.read_lef_with_def = False
    config.lef_files = [str(args.tech_lef), str(args.cell_lef)]
    config.macro_layout_files = [str(args.macro_gds)]
    config.macro_resolution_mode = 2
    config.layer_map = kdb.LayerMap.from_string(layer_map_text)
    config.create_other_layers = False

    config.routing_datatype = 0
    config.special_routing_datatype = 0
    config.via_geometry_datatype = 0
    config.pins_datatype = 251
    config.labels_datatype = 251

    # Macro geometry comes from the GDS, not LEF abstracts. A top-level DEF
    # outline is not needed; OpenFinRAM creates the final macro boundary.
    config.produce_cell_outlines = False
    config.produce_lef_pins = False
    config.produce_lef_labels = False
    config.produce_obstructions = False

    layout = kdb.Layout()
    layout.read(str(args.def_file), options)

    top = layout.cell(args.top_cell)
    if top is None:
        available = ", ".join(sorted(cell.name for cell in layout.top_cells()))
        raise RuntimeError(
            f"top cell {args.top_cell!r} was not imported; available tops: {available}"
        )

    instance_count = sum(1 for _ in top.each_inst())
    if layout.cells() <= 1 or instance_count == 0:
        raise RuntimeError(
            "DEF import contains no macro instances; standard-cell GDS substitution failed"
        )

    output = args.output_gds.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp.gds")
    layout.write(str(temporary))

    # Reopen the stream before publishing it so a partial or malformed file
    # never replaces the controller used by LayoutGenerator.
    check = kdb.Layout()
    check.read(str(temporary))
    check_top = check.cell(args.top_cell)
    if check_top is None or check.cells() <= 1:
        temporary.unlink(missing_ok=True)
        raise RuntimeError("written GDS failed hierarchy validation")

    os.replace(temporary, output)
    print(
        "DEF-to-GDS stream-out complete: "
        f"top={args.top_cell} cells={layout.cells()} "
        f"instances={instance_count} bbox={top.dbbox()} output={output}"
    )


def main() -> int:
    try:
        stream_def_to_gds(parse_args())
    except Exception as error:  # Keep the C++ caller's failure mode concise.
        print(f"DEF-to-GDS stream-out failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
