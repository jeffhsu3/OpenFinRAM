#!/usr/bin/env python3
"""Instance-level LVS audit for an OpenROAD-produced controller GDS.

This is stage 1 of GDS-level decode verification (full transistor-level
extraction needs Calibre or a complete Magic ASAP7 extraction tech; neither
is available -- see "Periphery status" in scripts/README_sram_tools.md).

What this checks, on the final routed/streamed `ctrl_decode.gds`:

  1. Instance multiset: every standard-cell instance in the reference
     gate-level netlist (`netlist_for_lvs.v`) appears in the GDS placement,
     and vice versa -- no dropped, extra, or swapped-in cell types.
  2. Cell integrity: every distinct placed cell's geometry (polygons +
     sub-references, layer/datatype/coordinate-normalized) is identical to
     the same-named master cell in the ASAP7 library GDS -- no corrupted or
     substituted layouts.

Physical-only cells that OpenROAD inserts after synthesis (fillers,
tapcells, decap) never appear in a netlist; they are skipped via an
allowlist of name prefixes.

What this does NOT check: routing connectivity between instances (a
mis-wired route passes this audit). That requires device extraction or
metal-connectivity tracing; combined with the post-synthesis formal
equivalence (tests/run_equiv_check.sh) and OpenROAD's DEF/GDS coming from
the same database, the remaining exposure is limited to routing defects.

Usage:
    python3 scripts/lvs_instance_check.py \
        --macro-gds tmp/openroad_<ts>/ctrl_decode.gds --top ctrl_decode \
        --netlist tmp/openroad_<ts>/netlist_for_lvs.v \
        --library-gds tech/gds/asap7sc7p5t_28_R_220121a.gds

Exit 0 iff both checks pass.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from collections import Counter
from pathlib import Path

import gdstk

# Cells OpenROAD adds physically (tapcell / filler_placement) that have no
# netlist instance. Matched as prefixes.
PHYSICAL_PREFIXES = ("FILLER", "TAPCELL", "DECAP", "ANTENNA", "VIA_")

_VERILOG_INSTANCE = re.compile(
    r"\b([A-Za-z_]\w*)\s+(\\[^\s(]+|[A-Za-z_][\w.$]*)\s*\(\s*\."
)


def count_netlist_cells(path: Path) -> Counter:
    text = path.read_text()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    counts: Counter = Counter()
    for m in _VERILOG_INSTANCE.finditer(text):
        counts[m.group(1)] += 1
    return counts


def is_physical_only(cell_name: str, prefixes: tuple[str, ...]) -> bool:
    return any(cell_name.startswith(p) for p in prefixes)


def cell_fingerprint(cell: gdstk.Cell) -> str:
    """Stable geometry signature: polygons (normalized) + child references."""
    items = []
    for poly in cell.polygons:
        pts = tuple(sorted((round(x, 4), round(y, 4)) for x, y in poly.points))
        items.append(("P", poly.layer, poly.datatype, pts))
    for ref in cell.references:
        name = ref.cell.name if not isinstance(ref.cell, str) else ref.cell
        items.append(
            (
                "R",
                name,
                round(ref.origin[0], 4),
                round(ref.origin[1], 4),
                round(ref.rotation or 0, 4),
                bool(ref.x_reflection),
            )
        )
    for label in cell.labels:
        items.append(("L", label.text, label.layer, label.texttype))
    digest = hashlib.sha256()
    for item in sorted(items, key=repr):
        digest.update(repr(item).encode())
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--macro-gds", type=Path, required=True)
    ap.add_argument("--top", default="ctrl_decode")
    ap.add_argument("--netlist", type=Path, required=True)
    ap.add_argument("--library-gds", type=Path, required=True)
    ap.add_argument(
        "--physical-prefix",
        action="append",
        default=list(PHYSICAL_PREFIXES),
        help="cell-name prefix treated as physical-only (repeatable)",
    )
    args = ap.parse_args(argv)

    lib = gdstk.read_gds(str(args.library_gds))
    lib_cells = {c.name: c for c in lib.cells}
    macro = gdstk.read_gds(str(args.macro_gds))
    top = next((c for c in macro.cells if c.name == args.top), None)
    if top is None:
        print(f"FAIL: top cell {args.top!r} not in {args.macro_gds}")
        return 1

    # ---- check 2: placed-cell geometry matches library masters -------------
    placed_names = {
        r.cell.name if not isinstance(r.cell, str) else r.cell
        for r in top.references
    }
    bad_geo = []
    unknown = []
    fingerprints = {}
    for name in sorted(placed_names):
        if is_physical_only(name, tuple(args.physical_prefix)):
            continue
        mac_cell = next((c for c in macro.cells if c.name == name), None)
        lib_cell = lib_cells.get(name)
        if mac_cell is None or lib_cell is None:
            unknown.append(name)
            continue
        fm, fl = cell_fingerprint(mac_cell), cell_fingerprint(lib_cell)
        fingerprints[name] = fm
        if fm != fl:
            bad_geo.append(name)

    # ---- check 1: instance multiset vs netlist -----------------------------
    net_counts = count_netlist_cells(args.netlist)
    gds_counts = Counter()
    for ref in top.references:
        name = ref.cell.name if not isinstance(ref.cell, str) else ref.cell
        if is_physical_only(name, tuple(args.physical_prefix)):
            continue
        gds_counts[name] += 1

    missing = {c: n for c, n in net_counts.items() if gds_counts[c] < n}
    extra = {c: n for c, n in gds_counts.items() if net_counts[c] < n}

    ok = True
    total_net = sum(net_counts.values())
    total_gds = sum(gds_counts.values())
    print(f"instance audit for {args.top!r} in {args.macro_gds}")
    print(f"  netlist instances (non-physical): {total_net}")
    print(f"  GDS placements (non-physical):    {total_gds}")

    if unknown:
        ok = False
        print(f"  FAIL: {len(unknown)} placed cells absent from library GDS:")
        for n in sorted(unknown):
            print(f"    - {n}")
    if bad_geo:
        ok = False
        print(f"  FAIL: {len(bad_geo)} cells whose geometry differs from "
              f"the library master:")
        for n in sorted(bad_geo):
            print(f"    - {n}")
    if missing:
        ok = False
        print("  FAIL: cell types under-represented in GDS vs netlist:")
        for c, n in sorted(missing.items()):
            print(f"    - {c}: netlist {n}, gds {gds_counts.get(c, 0)}")
    if extra:
        ok = False
        print("  FAIL: cell types over-represented in GDS vs netlist:")
        for c, n in sorted(extra.items()):
            print(f"    - {c}: gds {n}, netlist {net_counts.get(c, 0)}")

    if ok:
        print(
            "PASS: instance multiset matches netlist; all placed cell "
            f"geometries match {len(fingerprints)} library masters"
        )
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
