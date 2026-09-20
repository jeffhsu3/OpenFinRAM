"""The OpenEvolve driver-slice harness: frame, static checker, objectives, ladder.

Everything except the last test is pure Python and runs in well under a
second; the ladder test needs KLayout and the public ASAP7 runset.
"""

import importlib.util
import shutil
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
PROBLEM = REPO_ROOT / "scripts" / "openevolve_slice"
sys.path.insert(0, str(PROBLEM))

pytest.importorskip("chipforge_asap7")
from slice_frame import INPUT_PINS, SPECS, make_frame, objectives, static_check  # noqa: E402


def _load(path: Path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASELINE = _load(PROBLEM / "initial_program.py")
FRAMES = {name: make_frame(spec) for name, spec in SPECS.items()}


def _with_plan(**changes):
    """The baseline router with some of its plan() entries replaced."""
    original = BASELINE.plan

    def route(frame):
        BASELINE.plan = lambda f: {**original(f), **changes}
        try:
            return BASELINE.route_slice(frame)
        finally:
            BASELINE.plan = original

    return route


def test_frame_is_json_safe_and_describes_four_wordlines():
    import json

    for name, frame in FRAMES.items():
        json.dumps(frame)
        assert [n["wordline"] for n in frame["nands"]] == [0, 1, 2, 3], name
        assert [d["wordline"] for d in frame["drivers"]] == [0, 1, 2, 3], name
        assert frame["width"] == 432
        # Wordlines leave on the 108 nm bitcell pitch.
        assert [d["wl_pad"]["x"] for d in frame["drivers"]] == [54, 162, 270, 378]
        assert frame["blocked_m2"], "the leaf cells' B ties are M2 obstacles"
    assert FRAMES["released"]["height"] == 3240


def test_baseline_router_is_statically_clean_on_every_size():
    for name, frame in FRAMES.items():
        errors, _ = static_check(frame, BASELINE.route_slice(frame))
        assert errors == [], (name, errors[:3])


def test_static_check_names_the_short_a_bad_plan_makes():
    """Moving N3's column onto N2's must come back as a short, in milliseconds."""
    route = _with_plan(n_column_offset=[-54, 0, 108, 54])
    errors, _ = static_check(FRAMES["small"], route(FRAMES["small"]))
    assert any("short" in e and "N2" in e and "N3" in e for e in errors), errors


def test_static_check_catches_an_open_a_stray_shape_and_a_missing_pin():
    frame = FRAMES["small"]
    good = BASELINE.route_slice(frame)
    no_wl = {"shapes": [s for s in good["shapes"] if not (s[0] == "M3" and s[4] == frame["height"])],
             "pins": good["pins"]}
    assert any("top edge" in e for e in static_check(frame, no_wl)[0])
    outside = {"shapes": [*good["shapes"], ["M2", -40, 100, 60, 118]], "pins": good["pins"]}
    assert any("leaves the slice" in e for e in static_check(frame, outside)[0])
    no_pin = {"shapes": good["shapes"], "pins": {k: v for k, v in good["pins"].items() if k != "SEL"}}
    assert any("pins['SEL']" in e for e in static_check(frame, no_pin)[0])
    assert static_check(frame, "not a dict")[0]


def test_objectives_reward_moving_the_climbing_columns_aside():
    """The headroom the search is after: the upper B pins gain escape columns."""
    frame = FRAMES["released"]
    base = objectives(frame, BASELINE.route_slice(frame))
    route = _with_plan(n_column_offset=[-54, 0, 108, 0])
    assert static_check(frame, route(frame))[0] == []
    better = objectives(frame, route(frame))
    assert set(f"escape_{p}" for p in INPUT_PINS) <= set(base)
    assert better["escape_B1"] > base["escape_B1"] and better["escape_B3"] > base["escape_B3"]
    assert better["pin_escape"] > base["pin_escape"]
    assert better["m3_feedthrough"] > base["m3_feedthrough"]
    assert better["wire_cost"] > base["wire_cost"]  # and it pays for it in wire


def test_last_code_block_keeps_a_reasoning_models_answer():
    pytest.importorskip("openevolve")
    from local_llm import last_code_block

    reply = "First I might try\n```python\ndraft = 1\n```\nbut better:\n```python\nfinal = 2\n```"
    assert last_code_block(reply) == "```python\nfinal = 2\n```"
    assert last_code_block("no code here") == "no code here"


@pytest.mark.skipif(shutil.which("klayout") is None, reason="KLayout not installed")
def test_baseline_clears_the_whole_ladder():
    pytest.importorskip("openevolve")
    import evaluator

    if not evaluator.DRC_DECK.is_file():
        pytest.skip("public ASAP7 runset not installed")
    result = evaluator.evaluate(str(PROBLEM / "initial_program.py"))
    assert result.metrics["stage"] == 3.0
    assert result.metrics["combined_score"] > 0.8
    assert "legal on all sizes" in result.artifacts["notes"]
