#!/usr/bin/env python3
"""Reproducibly copy named cells (+ their dependency closure) from one GDS into
another. Used to restore push-rule IP cells that were dropped when
`tech/gds/srambank_32b_boundary_2.gds` was pared down from the academic
`asap7_sram_0p0/gds/srambank_32b.gds`.

This keeps the input GDS a *documented, rebuildable* artifact instead of an
opaque binary edit: the manifest below records exactly which cells OpenFinRAM
pulls from the academic source and why. Re-run any time the source updates.

    uv run python scripts/merge_cells_from_gds.py            # apply the manifest
    uv run python scripts/merge_cells_from_gds.py --dry-run  # report only
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import gdstk

REPO = Path(__file__).resolve().parent.parent
ACADEMIC = Path("/home/jeff/iv4/repos/asap7/asap7_sram_0p0/gds/srambank_32b.gds")
TARGET = REPO / "tech/gds/srambank_32b_boundary_2.gds"

# Manifest: cells to guarantee present in TARGET, sourced from ACADEMIC.
# These are the bitcell-pitch dummy-fill cells for the ctrl_decode region
# (well/implant/fin continuity + clean array-boundary abutment) that the
# pared-down boundary_2 GDS omitted.
MANIFEST = [
    "dummy_vertical_6t122",       # full 0.162 x 0.350 push-rule dummy bitcell
    "dummy_corner",               # dummy-row corner cell
    "dummy_corner_v2",            # dummy-row corner cell (variant)
    "tapcell_dummy_6t122",        # well tap in the dummy row
    "dummy_vertical_array_X64",   # pre-tiled 64-wide dummy row (uses the above)
]


def closure(cell: gdstk.Cell) -> list[gdstk.Cell]:
    """cell + all cells it depends on (recursive)."""
    return [cell, *cell.dependencies(True)]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, default=ACADEMIC)
    ap.add_argument("--target", type=Path, default=TARGET)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    src = gdstk.read_gds(str(args.source))
    tgt = gdstk.read_gds(str(args.target))
    src_cells = {c.name: c for c in src.cells}
    tgt_names = {c.name for c in tgt.cells}

    missing_src = [n for n in MANIFEST if n not in src_cells]
    if missing_src:
        print(f"!! manifest cells absent from source {args.source.name}: {missing_src}",
              file=sys.stderr)
        return 2

    # Build the set of cells to add (manifest + deps), skipping names already
    # present in the target (never clobber an existing cell).
    to_add: dict[str, gdstk.Cell] = {}
    for name in MANIFEST:
        for c in closure(src_cells[name]):
            if c.name not in tgt_names and c.name not in to_add:
                to_add[c.name] = c

    print(f"source: {args.source}")
    print(f"target: {args.target}  ({len(tgt_names)} cells)")
    if not to_add:
        print("nothing to add — all manifest cells (and deps) already present.")
        return 0
    print(f"adding {len(to_add)} cell(s):")
    for n in sorted(to_add):
        reason = "(manifest)" if n in MANIFEST else "(dependency)"
        print(f"   + {n} {reason}")

    if args.dry_run:
        print("dry-run: no changes written.")
        return 0

    backup = args.target.with_suffix(".gds.bak")
    if not backup.exists():
        shutil.copy2(args.target, backup)
        print(f"backup: {backup}")
    tgt.add(*to_add.values())
    tgt.write_gds(str(args.target))
    print(f"wrote {args.target} ({len(tgt.cells)} cells)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
