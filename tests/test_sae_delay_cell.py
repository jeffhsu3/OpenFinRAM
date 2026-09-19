"""Tests for the custom ASAP7 SAE/replica delay cell (chipforge_asap7 build).

Covers: config validation, SPICE netlist structure, fin-level layout
self-verification, LEF abstract, the gLayout Component bridge, the public
KLayout DRC runset on a terminated row, KLayout LVS, Xyce delay/functional
measurement, and the Tier-0 surrogate calibration hook.
"""

import shutil
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.generate_asap7_sae_delay import (
    SaeDelayConfig,
    drc,
    find_drc_deck,
    fitness,
    layout,
    lef_abstract,
    load_calibration,
    lvs,
    lvs_schematic,
    netlist,
    simulate,
    verify_topology,
    write_cell,
)
from scripts.rl_env.surrogate_model import SramSurrogateModel
from scripts.rl_env.types import SramConfig, SramSpec

XYCE = Path("/home/jeff/iv4/local/xyce-14.4/bin/Xyce")
needs_xyce = pytest.mark.skipif(
    not (shutil.which(str(XYCE)) or XYCE.exists()), reason="Xyce not installed"
)


def _klayout_available() -> bool:
    try:
        from chipforge_asap7.verification import find_klayout
        find_klayout()
        return True
    except FileNotFoundError:
        return False


needs_klayout = pytest.mark.skipif(not _klayout_available(), reason="KLayout not installed")
needs_drc_deck = pytest.mark.skipif(find_drc_deck() is None, reason="public ASAP7 runset not installed")


class TestSaeDelayConfig:
    def test_rejects_odd_stages(self):
        with pytest.raises(ValueError):
            SaeDelayConfig(stages=5)

    def test_rejects_bad_fins(self):
        with pytest.raises(ValueError):
            SaeDelayConfig(stages=4, nfin_n=5)

    def test_floorplan_tiles(self):
        # NAND (4 CPP) + pair (4) + trailing inverter (3); plain chain: two pairs.
        assert [t.kind for t in SaeDelayConfig(stages=4).tiles] == ["nand", "pair", "inv"]
        assert SaeDelayConfig(stages=4, nand_enable=True).width_cpp == 11
        assert SaeDelayConfig(stages=4, nand_enable=False).width_cpp == 8

    def test_row_height_follows_the_standard_cell_row(self):
        assert SaeDelayConfig(nfin_n=1, nfin_p=3).height == 270
        assert SaeDelayConfig(nfin_n=4, nfin_p=2).height == 324  # next fin-legal band

    def test_pitched_cells_match_the_bitcell_row_and_column_pitch(self):
        eight = SaeDelayConfig(stages=6, pitch="8t")
        assert (eight.height, eight.width, eight.columns) == (594, 864, 8)
        assert [t.tracks for t in eight.tiles] == [4, 4, 4, 4]  # padded trailing inverter
        six = SaeDelayConfig(stages=4, nfin_n=3, nfin_p=3, nand_enable=False, pitch="6t")
        assert (six.height, six.width, six.columns) == (270, 432, 4)
        with pytest.raises(ValueError):
            SaeDelayConfig(nfin_n=4, pitch="6t")  # 4 fins do not fit one 270 nm row
        assert SaeDelayConfig(stages=6).columns == 7.5  # the CORE cell is not column-pitched
        lef = lef_abstract(eight)
        assert "CLASS BLOCK" in lef and "SITE" not in lef and "SIZE 0.864 BY 0.594" in lef
        assert verify_topology(layout(eight), eight)[5] == {"n": 2, "p": 2}


class TestCellOutputs:
    def test_netlist_device_counts(self):
        nl = netlist(SaeDelayConfig(stages=4, nfin_n=2, nfin_p=3, nand_enable=True))
        assert nl.count("nmos_sram") == 2 + 3  # NAND series N + 3 inverter N
        assert nl.count("pmos_sram") == 2 + 3
        assert "nfin=2" in nl and "nfin=3" in nl

    def test_lvs_reference_is_unit_fin(self):
        cfg = SaeDelayConfig(stages=4, nfin_n=2, nfin_p=3, nand_enable=True)
        ref = lvs_schematic(cfg)
        assert ref.count("W=7n") == 5 * 2 + 5 * 3
        assert ".SUBCKT sae_delay_4s_2n3p_en_sram IN OUT EN VDD VSS" in ref

    def test_layout_self_verification(self):
        for nand in (True, False):
            cfg = SaeDelayConfig(stages=4, nfin_n=3, nfin_p=1, nand_enable=nand)
            measured = verify_topology(layout(cfg), cfg)
            assert measured[0] == {"n": 3, "p": 1}
            assert measured[3] == {"n": 3, "p": 1}

    def test_nand_tile_is_chipforge_nandspec(self):
        """The first stage is `NandSpec(fingers=1, abut=False)`, flattened in place."""
        from chipforge_asap7.devices import build_nand
        from scripts.generate_asap7_sae_delay import layer_boxes

        cfg = SaeDelayConfig(stages=4, nfin_n=2, nfin_p=3)
        spec = cfg.nand_spec
        assert (spec.fingers, spec.abut) == (1, False)
        assert (spec.width, spec.height) == (cfg.tiles[0].width, cfg.height)
        assert list(spec.gate_xs) == cfg.stage_gate_xs[0]
        cell = layout(cfg)
        assert cell.references == []  # one flat cell, as before
        nand = build_nand(spec)
        for layer in ("ACTIVE", "SDT", "LISD", "LIG", "V0", "M1", "GATE", "FIN"):
            assert set(layer_boxes(nand, layer)) <= set(layer_boxes(cell, layer)), layer
        assert layer_boxes(cell, "BOUNDARY") == [(0, 0, cfg.width, cfg.height)]
        # IN is NandSpec's output-side input, EN its rail-side one.
        seam = spec.seam_y
        assert cfg.pin_positions["IN"] == (135, seam) and cfg.pin_positions["EN"] == (81, seam)

    def test_lef_abstract(self):
        lef = lef_abstract(SaeDelayConfig(stages=6, nand_enable=True))
        assert "MACRO sae_delay_6s_2n2p_en_sram" in lef
        assert "PIN EN" in lef
        assert "SIZE 0.810 BY 0.270" in lef
        assert "SITE asap7sc7p5t" in lef
        assert "LAYER M1" in lef and "OBS" in lef

    def test_write_cell(self, tmp_path):
        summary = write_cell(SaeDelayConfig(stages=2), tmp_path)
        for suffix in (".gds", ".sp", ".lef", ".json", "_lvs_ref.sp"):
            assert (tmp_path / f"{summary['cell']}{suffix}").stat().st_size > 0


class TestGLayoutBackend:
    def test_glayout_equivalence(self, tmp_path):
        gl = pytest.importorskip("scripts.glayout_sae_delay")
        cfg = SaeDelayConfig(stages=6, nfin_n=2, nfin_p=2, nand_enable=True)
        result = gl.check_equivalence(cfg, tmp_path)
        assert result["ports"] == ["EN", "IN", "OUT", "VDD", "VSS"]
        assert result["verify_stage0"] == {"n": 2, "p": 2}

    def test_glayout_plain_chain(self, tmp_path):
        gl = pytest.importorskip("scripts.glayout_sae_delay")
        cfg = SaeDelayConfig(stages=2, nfin_n=1, nfin_p=4, nand_enable=False)
        result = gl.check_equivalence(cfg, tmp_path)
        assert "EN" not in result["ports"]


class TestPhysicalVerification:
    @needs_klayout
    @needs_drc_deck
    @pytest.mark.parametrize("cfg", [
        SaeDelayConfig(stages=6, nfin_n=2, nfin_p=2, nand_enable=True),
        SaeDelayConfig(stages=4, nfin_n=3, nfin_p=3, nand_enable=False),
        SaeDelayConfig(stages=2, nfin_n=1, nfin_p=1, nand_enable=True, vt="rvt"),
        SaeDelayConfig(stages=6, nfin_n=2, nfin_p=2, pitch="8t"),
        SaeDelayConfig(stages=4, nfin_n=1, nfin_p=3, pitch="6t"),
    ])
    def test_terminated_row_is_drc_clean(self, cfg, tmp_path):
        assert drc(cfg, tmp_path) == []

    @needs_klayout
    @pytest.mark.parametrize("cfg", [
        SaeDelayConfig(stages=6, nfin_n=2, nfin_p=2, pitch="8t"),
        SaeDelayConfig(stages=4, nfin_n=1, nfin_p=3, pitch="6t"),
    ])
    def test_pitched_cells_pass_lvs(self, cfg, tmp_path):
        assert lvs(cfg, tmp_path).matched

    @needs_klayout
    def test_lvs_matches_and_detects_wrong_sizing(self, tmp_path):
        cfg = SaeDelayConfig(stages=4, nfin_n=2, nfin_p=3, nand_enable=True)
        assert lvs(cfg, tmp_path / "ok").matched
        wrong = tmp_path / "wrong"
        wrong.mkdir()
        # A reference with one fin too many must not match the drawn cell.
        from scripts.generate_asap7_sae_delay import new_library, run_lvs
        lib = new_library()
        layout(cfg, lib=lib)
        gds = wrong / "cell.gds"
        lib.write_gds(str(gds))
        ref = wrong / "ref.sp"
        ref.write_text(lvs_schematic(SaeDelayConfig(stages=4, nfin_n=3, nfin_p=3, nand_enable=True))
                       .replace("sae_delay_4s_3n3p_en_sram", cfg.cell_name))
        assert not run_lvs(gds, ref, wrong / "lvs", cell_name=cfg.cell_name, tie_bodies=True).matched


class TestMeasuredCell:
    @needs_xyce
    def test_xyce_delay_and_swing(self):
        cfg = SaeDelayConfig(stages=4, nfin_n=2, nfin_p=2, load_fF=8.0)
        m = simulate(cfg)
        assert m["functional"]
        assert 10.0 < m["delay_ps"] < 300.0

    @needs_xyce
    def test_more_fins_is_faster(self):
        weak = simulate(SaeDelayConfig(stages=4, nfin_n=1, nfin_p=1, load_fF=8.0))
        strong = simulate(SaeDelayConfig(stages=4, nfin_n=4, nfin_p=4, load_fF=8.0))
        assert weak["delay_ps"] > strong["delay_ps"]

    def test_calibration_loads(self):
        cal = load_calibration()
        assert cal is not None
        assert set(cal) == {"a", "b", "c"}

    def test_surrogate_uses_measured_sae(self):
        spec = SramSpec(num_words=64, num_data_bits=16, target_clock_period_ps=10000.0)
        small = SramConfig(num_wls=64, num_data_bits=16, mux_ratio=4,
                           sae_delay_chain=2, sae_nfin=4)
        big = SramConfig(num_wls=64, num_data_bits=16, mux_ratio=4,
                         sae_delay_chain=8, sae_nfin=1)
        m_small, _ = SramSurrogateModel.evaluate(spec, small)
        m_big, _ = SramSurrogateModel.evaluate(spec, big)
        # Few fat stages beat many thin ones under sense-enable fanout load.
        assert m_small.access_delay_ps < m_big.access_delay_ps
