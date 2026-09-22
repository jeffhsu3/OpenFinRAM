#!/usr/bin/env python3
"""Transistor-level LVS and device DRC of an assembled macro, with open tools.

    .venv/bin/python scripts/verify_macro.py results/sram_x4x2x1_<stamp>

The compiler's own gate (`compile_asap7_2rw.py verify()`) re-extracts the metal
graph and checks that every routed terminal reaches its net.  It treats the
columns and the controller as abstracts, so it cannot see inside them, and it
says so: ``"signoff_lvs": "not_run", "device_drc": "not_run"``.  This runs both.

LVS is KLayout with chipforge_asap7's ASAP7 deck, against the ``.sp`` the
compiler wrote beside the GDS:

* devices are compared fin by fin, source/drain taken the way released CDL
  assumes (the whole ACTIVE outside the gate);
* bitcells are flattened on both sides, since a bitcell's transistors are
  completed by its neighbours and the layout cell is not the schematic's;
* released standard cells whose GDS and CDL order a series stack differently
  (``AO21x1``, ``AOI211xp5``, ...) are proven equivalent as series/parallel
  networks and then compared by pin name.  They are listed; nothing else is
  excused.

When the strict run fails it is repeated with ``double_implant_is_tap=False``.
ACTIVE under both implants acts as a well tap, which ties whatever diffusion it
belongs to to the substrate; one such error grounds a whole class of nets and
hides everything else.  The second run shows what is left without it.

DRC is the public ASAP7 KLayout runset over the whole macro.  It is not a
sign-off deck, and the released hard cells and OpenROAD's routing both trip it,
so the result is a count per rule to hold a line with, not a pass.

``--baseline`` compares against known findings, the way
`tests/run_8t_device_drc_check.sh` holds cell DRC: exit 0 if nothing is new,
1 if something is, 3 if something known has gone (tighten the baseline).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
BITCELLS = ("sram_cell_*",)


def _lvs_findings(result) -> dict:
    """The parts of a hierarchical LVS result worth keeping, JSON-safe."""
    shorts = sorted(
        f"{short['supply']} <- {short['circuit']}.{short['pin']} x{short['count']}"
        for circuit in result.summary["circuits"]
        for short in circuit.get("supply_shorts", [])
    )
    unmatched = {
        (circuit["layout"] or circuit["reference"]): {
            what: {k: v for k, v in tally.items() if k != "match"}
            for what, tally in circuit["counts"].items()
            if any(k != "match" for k in tally)
        }
        for circuit in result.summary["circuits"]
        if circuit["status"] in ("mismatch", "nomatch")
    }
    return {
        "matched": result.matched,
        "series_order_cells": sorted(result.series_order_cells),
        "failing": list(result.failing),
        "unmatched": unmatched,
        "supply_shorts": shorts,
        "report": str(result.lvs.report),
    }


def verify(result_dir: Path, out: Path, *, cell: str | None = None, drc: bool = True) -> dict:
    from chipforge_asap7.verification import drc_counts, run_drc, run_hierarchical_lvs

    result_dir = result_dir.resolve()
    if cell is None:
        described = sorted(result_dir.glob("*.physical.json"))
        cell = json.loads(described[0].read_text())["cell"] if described else result_dir.name.rsplit("_", 2)[0]
    gds, netlist = result_dir / f"{cell}.gds", result_dir / f"{cell}.sp"
    for path in (gds, netlist):
        if not path.is_file():
            raise FileNotFoundError(f"{path} is missing; is {result_dir} a compiler result?")

    started = time.time()
    strict = run_hierarchical_lvs(gds, netlist, out / "lvs", cell_name=cell, flatten_circuits=BITCELLS)
    verdict = {"cell": cell, "gds": str(gds), "lvs": _lvs_findings(strict)}
    print(strict.describe())
    if not strict.matched:
        relaxed = run_hierarchical_lvs(gds, netlist, out / "lvs_diagnostic", cell_name=cell,
                                       flatten_circuits=BITCELLS, double_implant_is_tap=False)  # fmt: skip
        verdict["lvs_without_double_implant_taps"] = _lvs_findings(relaxed)
        print("\n-- again, with ACTIVE under both implants not acting as a tap:")
        print(relaxed.describe())
    if drc:
        counts = drc_counts(run_drc(gds, out / "drc", cell_name=cell, timeout=7200))
        verdict["drc"] = {"markers": sum(counts.values()), "rules": dict(sorted(counts.items()))}
        print(f"\nDRC: {sum(counts.values())} markers in {len(counts)} rules; "
              f"implant overlap (NSELECT.PSELECT.AUX.1): {counts.get('NSELECT.PSELECT.AUX.1', 0)}")  # fmt: skip
    verdict["seconds"] = round(time.time() - started, 1)
    out.mkdir(parents=True, exist_ok=True)
    (out / "verification.json").write_text(json.dumps(verdict, indent=1) + "\n")
    return verdict


def known_findings(verdict: dict) -> dict:
    """What a baseline pins: LVS findings exactly, DRC as a ceiling per rule."""
    relaxed = verdict.get("lvs_without_double_implant_taps", {})
    return {
        "lvs_matched": verdict["lvs"]["matched"],
        "series_order_cells": verdict["lvs"]["series_order_cells"],
        "supply_shorts": verdict["lvs"]["supply_shorts"],
        "supply_shorts_without_double_implant_taps": relaxed.get("supply_shorts", []),
        "drc_rules": verdict.get("drc", {}).get("rules", {}),
    }


def compare(found: dict, baseline: dict) -> tuple[list[str], list[str]]:
    """``(new, gone)`` against `baseline`."""
    new, gone = [], []
    if baseline["lvs_matched"] and not found["lvs_matched"]:
        new.append("LVS no longer matches")
    if found["lvs_matched"] and not baseline["lvs_matched"]:
        gone.append("LVS now matches")
    for key in ("series_order_cells", "supply_shorts", "supply_shorts_without_double_implant_taps"):
        new += [f"{key}: {item}" for item in found[key] if item not in baseline.get(key, [])]
        gone += [f"{key}: {item}" for item in baseline.get(key, []) if item not in found[key]]
    for rule, count in found["drc_rules"].items():
        if count > baseline["drc_rules"].get(rule, 0):
            new.append(f"DRC {rule}: {baseline['drc_rules'].get(rule, 0)} -> {count}")
    gone += [f"DRC {rule}: {count} -> {found['drc_rules'].get(rule, 0)}"
             for rule, count in baseline["drc_rules"].items() if found["drc_rules"].get(rule, 0) < count]  # fmt: skip
    return new, gone


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("result_dir", type=Path, help="A compiler result folder (holds <cell>.gds and <cell>.sp).")
    parser.add_argument("--cell", default=None, help="Top cell; read from <cell>.physical.json when omitted.")
    parser.add_argument("--out", type=Path, default=None, help="Work and report folder (default tmp/verify_<name>).")
    parser.add_argument("--no-drc", action="store_true")
    parser.add_argument("--baseline", type=Path, default=None, help="Known findings to hold the line against.")
    parser.add_argument("--write-baseline", type=Path, default=None, help="Record this run's findings as known.")
    args = parser.parse_args(argv)

    out = args.out or REPO_ROOT / "tmp" / f"verify_{args.result_dir.resolve().name}"
    verdict = verify(args.result_dir, out, cell=args.cell, drc=not args.no_drc)
    found = known_findings(verdict)
    print(f"\nwrote {out / 'verification.json'}")
    if args.write_baseline:
        args.write_baseline.write_text(json.dumps(found, indent=1) + "\n")
        print(f"wrote {args.write_baseline}")
    if args.baseline:
        new, gone = compare(found, json.loads(args.baseline.read_text()))
        for line in new:
            print("NEW  ", line)
        for line in gone:
            print("GONE ", line)
        if new:
            return 1
        if gone:
            print("fewer findings than the baseline records: tighten it with --write-baseline")
            return 3
        print("no findings beyond the baseline")
        return 0
    return 0 if verdict["lvs"]["matched"] else 1


if __name__ == "__main__":
    sys.exit(main())
