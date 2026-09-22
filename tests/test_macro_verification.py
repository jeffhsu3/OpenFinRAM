"""Transistor-level LVS and DRC of an assembled two-port macro.

`scripts/verify_macro.py` is held against `tests/golden/asap7_2rw_macro_verification.json`
the way cell DRC is held against its baseline: the macro does not match yet, for
one known reason, and this fails when there is a *new* reason, or when the known
one is fixed and the baseline should be tightened.

Set ``OPENFINRAM_MACRO_RESULT`` to a result folder to check that one; otherwise
the newest ``results/sram_x4x2x1_*`` is used, or one is built when the compiler,
yosys and openroad are all here.  Skips without KLayout and the ASAP7 runset.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))

import verify_macro  # noqa: E402

BASELINE = REPO / "tests/golden/asap7_2rw_macro_verification.json"


def _found(**changes):
    found = {"lvs_matched": False, "series_order_cells": ["AO21x1_ASAP7_75t_R"],
             "supply_shorts": ["VSS <- CTRL_DECODE.WL_A[1] x1"],
             "supply_shorts_without_double_implant_taps": [], "drc_rules": {"M1.S.2": 3, "V0.S.1": 1}}  # fmt: skip
    return {**found, **changes}


def test_the_baseline_is_a_ratchet():
    base = _found()
    assert verify_macro.compare(_found(), base) == ([], [])
    new, gone = verify_macro.compare(_found(drc_rules={"M1.S.2": 4, "V0.S.1": 1, "M2.W.1": 1}), base)
    assert new == ["DRC M1.S.2: 3 -> 4", "DRC M2.W.1: 0 -> 1"] and gone == []
    new, gone = verify_macro.compare(_found(supply_shorts=["VSS <- IOPRECH_SRAM_8T_A.BLBN_A[0] x2"]), base)
    assert new == ["supply_shorts: VSS <- IOPRECH_SRAM_8T_A.BLBN_A[0] x2"]
    assert gone == ["supply_shorts: VSS <- CTRL_DECODE.WL_A[1] x1"]
    new, gone = verify_macro.compare(_found(lvs_matched=True, supply_shorts=[], drc_rules={"M1.S.2": 3}), base)
    assert new == [] and gone == ["LVS now matches", "supply_shorts: VSS <- CTRL_DECODE.WL_A[1] x1", "DRC V0.S.1: 1 -> 0"]
    assert verify_macro.compare(_found(), _found(lvs_matched=True))[0] == ["LVS no longer matches"]


def _result_dir(tmp_path: Path) -> Path:
    given = os.environ.get("OPENFINRAM_MACRO_RESULT")
    if given:
        return Path(given)
    built = sorted((REPO / "results").glob("sram_x4x2x1_*"))
    if built:
        return built[-1]
    binary = Path(os.environ.get("OPENFINRAM_BIN", REPO / "build/OpenFinRAM"))
    if not binary.is_file() or not all(shutil.which(tool) for tool in ("yosys", "openroad")):
        pytest.skip("no built sram_x4x2x1 result, and no compiler with yosys and openroad to build one")
    log = tmp_path / "compiler.log"
    with log.open("w") as stream:
        subprocess.run([str(binary), "--openroad", "--num-wls", "2", "--num-data-bits", "2", "--num-banks", "1",
                        "--skip-characterization"], cwd=REPO, stdout=stream, stderr=subprocess.STDOUT, check=True)  # fmt: skip
    return sorted((REPO / "results").glob("sram_x4x2x1_*"))[-1]


def test_macro_lvs_and_drc_hold_the_baseline(tmp_path: Path):
    pytest.importorskip("chipforge_asap7")
    from chipforge_asap7.verification import find_drc_deck, find_klayout

    try:
        find_klayout(None)
        find_drc_deck(None)
    except FileNotFoundError as error:
        pytest.skip(str(error))

    verdict = verify_macro.verify(_result_dir(tmp_path), tmp_path / "verify")
    found = verify_macro.known_findings(verdict)
    new, gone = verify_macro.compare(found, json.loads(BASELINE.read_text()))
    assert new == [], "new findings"
    assert gone == [], "known findings are gone: tighten the baseline with --write-baseline"

    # What the baseline says, spelled out, so that a change to it is a decision.
    lvs, relaxed = verdict["lvs"], verdict["lvs_without_double_implant_taps"]
    assert lvs["failing"] == ["sram_x4x2x1"]  # the controller, both IO columns and every cell below match
    assert lvs["series_order_cells"] == ["AND4x1_ASAP7_75t_R", "AO21x1_ASAP7_75t_R"]
    # One defect is left: the array tap's implant overlap ties every complement bitline to VSS.
    # A column is one unsplit array with an IO at each end, so that is four complement bitlines
    # a port -- on the one face of each wrapper that meets the array -- and half the overlaps
    # the two half arrays had.
    assert sorted(lvs["supply_shorts"]) == sorted(
        f"VSS <- IOPRECH_SRAM_8T_{port}.{pin}_{port}[{i}] x2"
        for port, pin in (("A", "BLBN"), ("B", "BLTN")) for i in range(4)
    )
    assert verdict["drc"]["rules"]["NSELECT.PSELECT.AUX.1"] == 10
    # ... and with that set aside the whole macro matches, transistor for transistor.  (It did not
    # until the WLA gate contacts were taken off the placement seam: wordlines were shorted there.)
    assert relaxed["matched"] and relaxed["failing"] == [] and relaxed["supply_shorts"] == []
