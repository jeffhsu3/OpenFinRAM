"""OpenEvolve evaluator for the driver-slice router: a three-stage verification ladder.

stage 1  static   run the candidate (subprocess, 20 s cap) on every spec and
                  check rules and connectivity in pure Python.  Milliseconds.
stage 2  one size KLayout LVS and the public ASAP7 DRC runset on the small slice,
                  between its filler and tap columns.
stage 3  matrix   LVS and DRC on the other sizes, DRC on two butted slices, then
                  the objectives: pin escape, M2/M3 feed-through, wire cost.

`combined_score` climbs the ladder, so a candidate that gets further always
beats one that stopped earlier: below 0.25 fails static, 0.25-0.5 fails the
first physical check, 0.5-0.6 fails somewhere in the matrix, and 0.6-1.0 is a
legal router ranked by its objectives.  Every result carries `pin_escape` and
`m3_feedthrough` because they are the MAP-Elites feature axes.

The candidate is untrusted code: it runs in a subprocess with a timeout, which
contains hangs and crashes but is not a security sandbox.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import gdspy  # noqa: E402
from chipforge_asap7.devices import build_driver_slice, build_driver_slice_support  # noqa: E402
from chipforge_asap7.verification import find_klayout, render_driver_slice_lvs_schematic, run_lvs  # noqa: E402
from openevolve.evaluation_result import EvaluationResult  # noqa: E402
from slice_frame import SPECS, make_frame, make_router, objectives, static_check  # noqa: E402

DRC_DECK = Path(os.environ.get(
    "ASAP7_DRC_DECK", Path.home() / "iv4/repos/ASAP7_for_KLayout/drc/drc_ASAP7.lydrc"))
#: The runset enumerates ACTIVE/SDT heights up to 12 fins; the released slice is taller.
DECK_ARTEFACTS = {"ACTIVE.W.2", "SDT.W.3"}
WEIGHTS = {"pin_escape": 0.35, "m2_feedthrough": 0.25, "m3_feedthrough": 0.20, "wire": 0.20}
ZERO = {"pin_escape": 0.0, "m2_feedthrough": 0.0, "m3_feedthrough": 0.0, "wire_cost": 0.0}

_RUNNER = """
import importlib.util, json, sys
sys.path.insert(0, sys.argv[4])  # router_kit lives beside the evaluator
spec = importlib.util.spec_from_file_location("candidate", sys.argv[1])
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
frames = json.load(open(sys.argv[2]))
json.dump({name: module.route_slice(frame) for name, frame in frames.items()}, open(sys.argv[3], "w"))
"""
_FRAMES = {name: make_frame(spec) for name, spec in SPECS.items()}


def _route(program_path: str) -> tuple[dict, str | None]:
    """Run the candidate on every frame in a subprocess.  Returns (results, error)."""
    with tempfile.TemporaryDirectory(prefix="slice_route_") as tmp:
        frames, out = Path(tmp) / "frames.json", Path(tmp) / "out.json"
        frames.write_text(json.dumps(_FRAMES))
        try:
            proc = subprocess.run(
                [sys.executable, "-c", _RUNNER, program_path, str(frames), str(out), str(HERE)],
                capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired:
            return {}, "route_slice did not finish within 20 s"
        if proc.returncode != 0 or not out.exists():
            return {}, "route_slice raised:\n" + proc.stderr.strip()[-700:]
        try:
            return json.loads(out.read_text()), None
        except ValueError as exc:
            return {}, f"route_slice returned something JSON cannot hold: {exc}"


def _library():
    library = gdspy.GdsLibrary(unit=1e-9, precision=1e-10)
    gdspy.current_library = library
    return library


def _drc(spec, routing, count: int, workdir: Path, tag: str) -> list[str]:
    """Public runset on `filler slice(s) filler tap filler`; returns 'category @ x,y' strings."""
    library = _library()
    cell = build_driver_slice(spec, lib=library, router=make_router(routing))
    support = {k: build_driver_slice_support(spec, k, lib=library) for k in ("filler", "tap")}
    top = library.new_cell(f"term_{tag}")
    cursor = 0
    for item in ("filler", *["slice"] * count, "filler", "tap", "filler"):
        top.add(gdspy.CellReference(cell if item == "slice" else support[item], origin=(cursor, 0)))
        cursor += spec.width if item == "slice" else 108
    gds, report = workdir / f"{tag}.gds", workdir / f"{tag}.lyrdb"
    library.write_gds(str(gds))
    subprocess.run([str(find_klayout()), "-b", "-r", str(DRC_DECK), "-rd", f"input={gds}",
                    "-rd", f"topcell={top.name}", "-rd", f"output={report}"],
                   check=True, capture_output=True, timeout=240)
    found = []
    for item in ET.parse(report).getroot().findall("./items/item"):
        category = (item.findtext("category") or "").strip("'\"").split(" ")[0][:24]
        where = (item.findtext("./values/value") or "")[:60]
        found.append(f"{category} @ {where}")
    return found


def _lvs(spec, routing, workdir: Path, tag: str) -> bool:
    library = _library()
    build_driver_slice(spec, lib=library, router=make_router(routing))
    gds, reference = workdir / f"{tag}_lvs.gds", workdir / f"{tag}_ref.sp"
    library.write_gds(str(gds))
    reference.write_text(render_driver_slice_lvs_schematic(spec))
    try:
        return run_lvs(gds, reference, workdir / f"{tag}_lvs", cell_name=spec.cell_name,
                       tie_bodies=True, timeout=240).matched
    except RuntimeError:
        return False


def _physical(name: str, routing: dict, workdir: Path, *, butted: bool = False) -> list[str]:
    """LVS then DRC for one spec; returns failure messages (empty = passed)."""
    spec = SPECS[name]
    if not _lvs(spec, routing, workdir, name):
        return [f"{name}: LVS mismatch -- the slice is not four ANDs sharing SEL "
                "(an open, a short, or a via that misses its metal)"]
    failures = []
    for count in (1, 2) if butted else (1,):
        found = [v for v in _drc(spec, routing, count, workdir, f"{name}_x{count}")
                 if v.split(" ")[0] not in DECK_ARTEFACTS]
        if found:
            label = f"{name}, {count} slice(s)"
            failures.append(f"{label}: {len(found)} DRC violation(s): " + "; ".join(found[:5]))
    return failures


def _result(score: float, metrics: dict, **artifacts: str) -> EvaluationResult:
    return EvaluationResult(metrics={**ZERO, **metrics, "combined_score": round(score, 5)},
                            artifacts={k: v[:1400] for k, v in artifacts.items() if v})


def evaluate_stage1(program_path: str) -> EvaluationResult:
    routings, error = _route(program_path)
    if error:
        return _result(0.0, {"stage": 0.0}, failure=error)
    errors = {name: static_check(_FRAMES[name], routings.get(name))[0] for name in SPECS}
    total = sum(len(e) for e in errors.values())
    if total:
        text = "\n".join(f"[{name}] {msg}" for name, msgs in errors.items() for msg in msgs[:6])
        clean = sum(1 for e in errors.values() if not e)
        return _result(0.2 * clean / len(SPECS) + 0.04 / (1 + total), {"stage": 1.0}, static_errors=text)
    return _result(0.25, {"stage": 1.0})


def evaluate_stage2(program_path: str) -> EvaluationResult:
    routings, error = _route(program_path)
    if error:
        return _result(0.0, {"stage": 0.0}, failure=error)
    with tempfile.TemporaryDirectory(prefix="slice_eval_") as tmp:
        failures = _physical("small", routings["small"], Path(tmp))
    if failures:
        return _result(0.3 if "LVS" in failures[0] else 0.4, {"stage": 2.0}, physical_errors="\n".join(failures))
    return _result(0.5, {"stage": 2.0})


def evaluate_stage3(program_path: str) -> EvaluationResult:
    routings, error = _route(program_path)
    if error:
        return _result(0.0, {"stage": 0.0}, failure=error)
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="slice_eval_") as tmp:
        failures += _physical("small", routings["small"], Path(tmp), butted=True)
        for name in ("one_row", "released"):
            failures += _physical(name, routings[name], Path(tmp))
    if failures:
        passed = 1 - len(failures) / 4
        return _result(0.5 + 0.1 * max(0.0, passed), {"stage": 3.0}, physical_errors="\n".join(failures))

    per_spec = [objectives(_FRAMES[name], routings[name]) for name in SPECS]
    mean = {k: sum(m[k] for m in per_spec) / len(per_spec) for k in per_spec[0]}
    baseline = float(os.environ.get("SLICE_BASELINE_WIRE_COST", "0") or 0) or mean["wire_cost"]
    wire = min(1.0, baseline / max(mean["wire_cost"], 1.0) / 1.25)
    objective = (WEIGHTS["pin_escape"] * mean["pin_escape"] + WEIGHTS["m2_feedthrough"] * mean["m2_feedthrough"]
                 + WEIGHTS["m3_feedthrough"] * mean["m3_feedthrough"] + WEIGHTS["wire"] * wire)
    released = per_spec[-1]
    notes = ("legal on all sizes. released slice: escapes to the bottom edge "
             + ", ".join(f"{p}={int(released['escape_' + p])}" for p in ("SEL", "B0", "B1", "B2", "B3"))
             + f"; free M2 rows {released['m2_feedthrough']:.2f}, free M3 columns {released['m3_feedthrough']:.2f}, "
             f"wire cost {released['wire_cost']:.0f} nm")
    return _result(0.6 + 0.4 * objective, {"stage": 3.0, **mean, "wire_score": wire}, notes=notes)


def evaluate(program_path: str) -> EvaluationResult:
    """The whole ladder in one call, for use without cascade evaluation."""
    for stage, threshold in ((evaluate_stage1, 0.25), (evaluate_stage2, 0.5)):
        result = stage(program_path)
        if result.metrics["combined_score"] < threshold:
            return result
    return evaluate_stage3(program_path)


if __name__ == "__main__":
    outcome = evaluate(sys.argv[1] if len(sys.argv) > 1 else str(HERE / "initial_program.py"))
    print(json.dumps(outcome.metrics, indent=1))
    for key, value in outcome.artifacts.items():
        print(f"--- {key}\n{value}")
