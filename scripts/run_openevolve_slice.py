#!/usr/bin/env python3
"""Evolve the ASAP7 driver-slice router with OpenEvolve and a locally served model.

Analytical sizing (`chipforge_asap7.devices.sizing.size_decoder`) settles the
decoder's devices.  What it cannot settle is the routing inside the handcrafted
slice: which tracks the wires take, where the vias go, and whether the result
leaves its pins reachable and its M2/M3 tracks free for the predecode bus.
That is combinatorial and only DRC finally judges it, so this searches it:
OpenEvolve rewrites `route_slice(frame)` and a three-stage ladder (static
rules, LVS + DRC on one size, LVS + DRC on a size matrix and on butted slices)
scores each candidate on pin escape, feed-through capacity and wire cost.

    # what does the hand-written router score?  (no LLM needed)
    .venv/bin/python scripts/run_openevolve_slice.py --check

    # evolve against the model served at localhost:8000
    .venv/bin/python scripts/run_openevolve_slice.py --iterations 200

    # let a reasoning model think (slower, several minutes per candidate)
    .venv/bin/python scripts/run_openevolve_slice.py --iterations 60 --thinking

    # resume, or draw the winner as GDS through build_driver_slice(router=...)
    .venv/bin/python scripts/run_openevolve_slice.py --resume results/openevolve_slice/<run>/checkpoints/checkpoint_50
    .venv/bin/python scripts/run_openevolve_slice.py --apply results/openevolve_slice/<run>/best/best_program.py

Files: scripts/openevolve_slice/{config.yaml, initial_program.py, evaluator.py,
slice_frame.py, local_llm.py}.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
PROBLEM = REPO_ROOT / "scripts" / "openevolve_slice"
sys.path.insert(0, str(PROBLEM))


def served_models(api_base: str) -> list[str]:
    """Model ids the server reports; raises with a usable message if it is not there."""
    url = api_base.rstrip("/") + "/models"
    try:
        with urllib.request.urlopen(url, timeout=10) as reply:
            return [m["id"] for m in json.load(reply)["data"]]
    except (urllib.error.URLError, OSError, KeyError, ValueError) as exc:
        raise SystemExit(f"no OpenAI-style server at {url}: {exc}") from exc


def check(program: Path) -> dict:
    """Run the whole evaluation ladder on one program and print the result."""
    import evaluator

    result = evaluator.evaluate(str(program))
    print(json.dumps(result.metrics, indent=1))
    for key, value in result.artifacts.items():
        print(f"--- {key}\n{value}")
    return result.metrics


def apply(program: Path, out_dir: Path) -> None:
    """Draw every spec with the program's router and re-verify it."""
    import evaluator
    import gdspy
    from chipforge_asap7.devices import build_driver_slice
    from slice_frame import SPECS, make_router

    routings, error = evaluator._route(str(program))
    if error:
        raise SystemExit(error)
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, spec in SPECS.items():
        library = gdspy.GdsLibrary(unit=1e-9, precision=1e-10)
        gdspy.current_library = library
        build_driver_slice(spec, lib=library, router=make_router(routings[name]))
        path = out_dir / f"{spec.cell_name}_evolved.gds"
        library.write_gds(str(path))
        print(f"{name}: {path}")
    check(program)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--config", type=Path, default=PROBLEM / "config.yaml")
    ap.add_argument("--program", type=Path, default=PROBLEM / "initial_program.py",
                    help="Program to start from (or to --check).")
    ap.add_argument("--iterations", type=int, default=None, help="Overrides max_iterations.")
    ap.add_argument("--output", type=Path, default=None, help="Run directory.")
    ap.add_argument("--api-base", default=None, help="Overrides llm.api_base.")
    ap.add_argument("--model", default="auto", help="Model id, or 'auto' to use what the server reports.")
    ap.add_argument("--thinking", action="store_true",
                    help="Leave the model's reasoning on and keep only its last code block.")
    ap.add_argument("--max-tokens", type=int, default=None, help="Overrides llm.max_tokens.")
    ap.add_argument("--parallel", type=int, default=None, help="Overrides evaluator.parallel_evaluations.")
    ap.add_argument("--checkpoint-interval", type=int, default=None,
                    help="Overrides checkpoint_interval (1 keeps every candidate, for debugging).")
    ap.add_argument("--resume", type=Path, default=None, help="Checkpoint directory to continue from.")
    ap.add_argument("--check", action="store_true", help="Score --program and exit; no LLM.")
    ap.add_argument("--apply", type=Path, default=None, metavar="PROGRAM",
                    help="Write GDS for every spec using PROGRAM's router, re-verify, and exit.")
    args = ap.parse_args(argv)

    if args.check:
        check(args.program)
        return 0
    if args.apply:
        apply(args.apply, args.output or args.apply.parent / "gds")
        return 0

    from local_llm import THINKING_ENV, make_local_llm
    from openevolve.config import load_config
    from openevolve.controller import OpenEvolve

    config = load_config(args.config)
    if args.api_base:
        config.llm.api_base = args.api_base
    available = served_models(config.llm.api_base)
    model = available[0] if args.model == "auto" else args.model
    if model not in available:
        raise SystemExit(f"{model!r} is not served at {config.llm.api_base}; it has {available}")
    if args.thinking and not args.max_tokens:
        args.max_tokens = 16000
    for entry in [*config.llm.models, *config.llm.evaluator_models]:
        entry.name = model
        entry.api_base = config.llm.api_base
        entry.init_client = make_local_llm  # module-level, so worker processes can rebuild it
        if args.max_tokens:
            entry.max_tokens = args.max_tokens
        if args.thinking:
            entry.timeout = max(entry.timeout or 0, 1500)
    if args.parallel:
        config.evaluator.parallel_evaluations = args.parallel
    if args.checkpoint_interval:
        config.checkpoint_interval = args.checkpoint_interval

    # Workers are separate processes: what they need travels in the environment.
    os.environ[THINKING_ENV] = "1" if args.thinking else "0"
    os.environ["PYTHONPATH"] = os.pathsep.join(
        filter(None, [str(PROBLEM), os.environ.get("PYTHONPATH", "")]))

    print(f"model {model} at {config.llm.api_base}, thinking {'on' if args.thinking else 'off'}")
    print("scoring the starting program (also fixes the wire-cost baseline) ...")
    baseline = check(args.program)
    if baseline.get("stage", 0) < 3 or baseline["combined_score"] < 0.6:
        raise SystemExit("the starting program does not pass the ladder; fix it before evolving")
    os.environ["SLICE_BASELINE_WIRE_COST"] = str(baseline["wire_cost"])

    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = args.output or REPO_ROOT / "results" / "openevolve_slice" / stamp
    out.mkdir(parents=True, exist_ok=True)
    controller = OpenEvolve(str(args.program), str(PROBLEM / "evaluator.py"), config, str(out))
    best = asyncio.run(controller.run(
        iterations=args.iterations, checkpoint_path=str(args.resume) if args.resume else None))

    if best is None:
        print("no program survived")
        return 1
    print(f"\nbest program {best.id}: combined_score {best.metrics.get('combined_score'):.4f} "
          f"(start {baseline['combined_score']:.4f})")
    for key in ("pin_escape", "m2_feedthrough", "m3_feedthrough", "wire_cost"):
        print(f"  {key:16s} {baseline.get(key, 0):9.3f} -> {best.metrics.get(key, 0):9.3f}")
    print(f"run directory: {out}\n  draw it with: {sys.argv[0]} --apply {out}/best/best_program.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
