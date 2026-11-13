"""The 6T timing model (tech/timing/sram_6t_timing.json) and its estimate for the Liberty emitter."""

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MODEL = REPO / "tech/timing/sram_6t_timing.json"
sys.path.insert(0, str(REPO / "scripts"))

import timing_model_6t as tm  # noqa: E402


def model():
    return json.loads(MODEL.read_text())


def test_the_fit_reproduces_every_simulated_macro():
    m = model()
    assert m["schema"] == tm.SCHEMA
    assert len(m["points"]) >= 6
    for point in m["points"]:
        predicted = tm.predict(m, point)
        assert abs(predicted["clk_to_q_ps"] - point["clk_to_q_ps"]) < 10, point
        assert abs(predicted["clk_to_wl_ps"] - point["clk_to_wl_ps"]) < 10, point


def test_bigger_macros_are_slower_and_cost_more():
    m = model()
    def macro(rows, bits, segments=1, mux=4):
        segment = bits // 2 // segments
        return {"bitline_cells": rows, "slice_inputs": tm.slice_inputs(rows, segments),
                "wordline_cells": mux * segment, "mux": mux, "bits": bits,
                "bitline_cells_switched": bits * mux * rows}

    small = macro(4, 2)
    for grown in (macro(64, 2), macro(4, 64), macro(4, 64, segments=4), macro(64, 64, segments=4)):
        assert tm.predict(m, grown)["clk_to_q_ps"] > tm.predict(m, small)["clk_to_q_ps"], grown
        assert tm.predict(m, grown)["read_fJ"] > tm.predict(m, small)["read_fJ"], grown
    # The read runs in the clock-high phase: the period is more than twice clk->Q.
    p = tm.predict(m, small)
    assert p["min_period_ps"] > 2 * p["clk_to_q_ps"]


def test_outer_strips_add_a_strip_a_stack_and_halve_what_a_slice_drives():
    # x256x64x1 at 8:1: 8-bit segments with outer strips against 4-bit ones without.
    assert tm.slice_inputs(256, 8) == 64 * 2 * 15
    assert tm.slice_inputs(256, 4, outer=True) == 64 * 2 * 8
    described = {"wordlines_per_half": 128, "bits": 64, "column_mux": 8, "cells_along_wordline": 64,
                 "wordline_segments_per_stack": 4}  # fmt: skip
    assert tm.geometry(described)["wordline_cells"] == 64
    with_outer = tm.geometry({**described, "outer_strips": 2})
    assert with_outer["wordline_cells"] == 32
    assert with_outer["slice_inputs"] == tm.slice_inputs(256, 4, outer=True)
    # The assembler writes 0 when the macro has none.
    assert tm.geometry({**described, "outer_strips": 0}) == tm.geometry(described)


def test_estimate_writes_the_characterization_the_emitter_reads(tmp_path):
    out = tmp_path / "timing.json"
    subprocess.run([sys.executable, str(REPO / "scripts/timing_model_6t.py"), "estimate", "--wordlines", "32",
                    "--bits", "64", "--segment-bits", "8", "--output", str(out)], check=True)  # fmt: skip
    doc = json.loads(out.read_text())
    assert doc["schema"] == "openfinram-characterization-1"
    delay = doc["timing"]["delay"]
    assert len(delay["cell_rise"]) == len(delay["index_1"]) == 1
    assert 0.2 < delay["cell_rise"][0][0] < 1.0
    assert doc["clock_min_period"] > 2 * delay["cell_rise"][0][0]
    assert doc["power"]["read_access_pj"] > 0


def test_d_hold_is_the_write_latch_close_plus_a_margin(tmp_path):
    """Fit from runs that measured the close; estimate emits it as D's own constraint."""
    specs = []
    for bits, close in ((2, 70.0), (16, 98.0)):
        result = tmp_path / f"result_{bits}"
        result.mkdir()
        (result / "m.physical.json").write_text(json.dumps({"bits": bits}))
        run = tmp_path / f"run_{bits}"
        run.mkdir()
        writes = [{"clk_to_write_close_ps": close - 3}, {"clk_to_write_close_ps": close}, {}]
        (run / "simulation.json").write_text(json.dumps({"writes": writes, "result_dir": str(result)}))
        specs.append(str(run))
    hold = tm.fit_write_hold(tm.write_hold_points(specs))
    assert abs(hold["base"] - 66.0) < 1e-6 and abs(hold["per_bit"] - 2.0) < 1e-6
    m = {**model(), "write_hold_ps": hold}
    assert tm.write_hold_ps(m, 64) == 66.0 + 2.0 * 64 + tm.WRITE_HOLD_MARGIN_PS
    g = {"bitline_cells": 8, "slice_inputs": tm.slice_inputs(8, 1), "wordline_cells": 4, "mux": 4, "bits": 2,
         "bitline_cells_switched": 64}  # fmt: skip
    doc = tm.characterization(m, g, "x8x2")
    data = doc["timing"]["data_constraints"]
    assert data["hold_rise"] == [[round((70.0 + tm.WRITE_HOLD_MARGIN_PS) / 1000, 4)]]
    assert data["setup_rise"] == [[tm.FLAT_CONSTRAINT_NS]]
    assert "constraints" not in doc["timing"]  # the controls keep the emitter's flat estimate
    # A model without the fit emits no D constraint (the emitter's flat estimate).
    bare = {k: v for k, v in model().items() if k != "write_hold_ps"}
    assert "data_constraints" not in tm.characterization(bare, g, "x8x2")["timing"]
