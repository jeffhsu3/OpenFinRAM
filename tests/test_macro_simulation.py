"""Whole-macro read/write simulation: the testbench's own logic, then the macro.

The parts that need no simulator run always.  The two Xyce runs take about six
minutes each (they run side by side) and want ``OPENFINRAM_SLOW_TESTS=1``, Xyce,
and a built ``results/sram_x4x2x1_*`` (or ``OPENFINRAM_MACRO_RESULT``).
"""

from __future__ import annotations

import os
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))

import simulate_macro as sm  # noqa: E402

NETLIST = """\
* a four-word, one-bit memory with the compiler's naming: one unsplit array
.SUBCKT sram_cell_8t WLA WLB BLA BLAN BLB BLBN VDD VSS
M0 Q QB VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2 nf=1 m=1
.ENDS
.SUBCKT array WLA[0] WLA[1] WLA[2] WLA[3] WLB[0] WLB[1] WLB[2] WLB[3] BLA[0] VDD VSS
X0 WLA[0] WLB[0] BLA[0] bn b bbn VDD VSS sram_cell_8t
X1 WLA[1] WLB[1] BLA[0] bn b bbn VDD VSS sram_cell_8t
X2 WLA[2] WLB[2] BLA[0] bn b bbn VDD VSS sram_cell_8t
X3 WLA[3] WLB[3] BLA[0] bn b bbn VDD VSS sram_cell_8t
.ENDS
.SUBCKT colgrp WLA[0] WLA[1] WLA[2] WLA[3] WLB[0] WLB[1] WLB[2] WLB[3] DA VDD VSS
X0 WLA[0] WLA[1] WLA[2] WLA[3] WLB[0] WLB[1]
+ WLB[2] WLB[3] BL_A[0] VDD VSS array
.ENDS
.SUBCKT top vdd vss D_A[0] D_A[1]
Xhigh wl_a_hi[0] wl_a_hi[1] wl_a_hi[2] wl_a_hi[3] wl_b_hi[0] wl_b_hi[1] wl_b_hi[2] wl_b_hi[3] D_A[1] vdd vss colgrp
Xlow wl_a_lo[0] wl_a_lo[1] wl_a_lo[2] wl_a_lo[3] wl_b_lo[0] wl_b_lo[1] wl_b_lo[2] wl_b_lo[3] D_A[0] vdd vss colgrp
.ENDS
"""


def test_devices_lose_what_xyce_rejects_and_nothing_else():
    text = sm.for_xyce(NETLIST)
    assert "M0 Q QB VSS VSS nmos_sram L=2e-08 nfin=2\n" in text
    assert text.count("sram_cell_8t") == NETLIST.count("sram_cell_8t")
    # Fingers and multipliers are width: folded into nfin, never dropped.
    assert sm.for_xyce("M1 Y A VDD VDD pmos_rvt nfin=18 l=20n nf=2 m=1\n") == "M1 Y A VDD VDD pmos_rvt nfin=36 l=20n\n"
    assert sm.for_xyce("MM3 Y AN VSS VSS nmos_rvt w=324.00n l=20n nfin=12\n") == "MM3 Y AN VSS VSS nmos_rvt l=20n nfin=12\n"


def test_cells_are_placed_by_what_they_are_wired_to():
    subckts = sm.parse_subckts(NETLIST)
    cells = sm.bitcells(subckts, "top")
    sm.locate(cells, wordlines=2)
    placed = {c["path"]: (c["half"], c["col"], c["row"], c["group"]) for c in cells}
    # Each stack of data bits has its own wordlines from its own driver strips.
    assert {c["path"].split(":")[0]: c["stack"] for c in cells} == {"Xhigh": "hi", "Xlow": "lo"}
    # NUM_WL = 2: wordlines 2 and 3 of the one array are the upper address half.
    assert placed == {
        "Xhigh:X0:X0": (0, 0, 0, "Xhigh"), "Xhigh:X0:X1": (0, 0, 1, "Xhigh"),
        "Xhigh:X0:X2": (1, 0, 0, "Xhigh"), "Xhigh:X0:X3": (1, 0, 1, "Xhigh"),
        "Xlow:X0:X0": (0, 0, 0, "Xlow"), "Xlow:X0:X1": (0, 0, 1, "Xlow"),
        "Xlow:X0:X2": (1, 0, 0, "Xlow"), "Xlow:X0:X3": (1, 0, 1, "Xlow"),
    }  # fmt: skip


def test_a_cell_whose_two_ports_disagree_about_its_row_is_refused():
    crossed = NETLIST.replace("X1 WLA[1] WLB[1]", "X1 WLA[1] WLB[2]")
    cells = sm.bitcells(sm.parse_subckts(crossed), "top")
    with pytest.raises(ValueError, match="cannot place"):
        sm.locate(cells, wordlines=2)


def test_an_address_is_sliced_the_way_the_controller_slices_it():
    g = sm.Geometry(wordlines=2, mux=4, bits=2)
    assert (g.row_bits, g.col_bits, g.words) == (1, 2, 16)
    assert g.split(0b1101) == (1, 2, 1)  # upper half, column 2, row 1
    assert g.wordline(0b1101) == 3 and g.wordline(0b0101) == 1  # one bus over the whole array
    assert sm.Geometry(wordlines=32, mux=4, bits=8).split((1 << 7) | (3 << 5) | 17) == (1, 3, 17)


def test_the_bank_is_the_address_bits_above_the_half():
    # sram_control.v: row, column, the half bit, then the bank index.  A
    # bank's wordlines follow the previous bank's in the strips' buses.
    g = sm.Geometry(wordlines=2, mux=4, bits=2, banks=2)
    assert (g.bank_shift, g.words) == (4, 32)
    assert g.bank(0b1_1101) == 1 and g.split(0b1_1101) == (1, 2, 1)
    assert g.wordline(0b1_1101) == 4 + 3 and g.wordline(0b0_1101) == 3
    program = sm.default_program(g)
    assert any(g.bank(op.address) == 1 for ops in program for op in ops if op.kind == "W")
    assert any(g.bank(op.address) == 1 for ops in program for op in ops if op.kind == "R")


def test_a_shared_port_b_enable_serves_a_pair_of_banks():
    g = sm.Geometry(wordlines=2, mux=4, bits=2, banks=4, shared_b=True)
    assert [g.io("B", bank) for bank in range(4)] == [0, 0, 1, 1]
    assert [g.io("A", bank) for bank in range(4)] == [0, 1, 2, 3]
    assert (g.ios("A"), g.ios("B")) == (4, 2)
    assert sm.Geometry(2, 4, 2, banks=4).ios("B") == 4


@pytest.mark.parametrize("g", [sm.Geometry(2, 4, 2), sm.Geometry(2, 4, 4), sm.Geometry(32, 4, 16)])
def test_no_read_in_the_program_can_pass_by_standing_still(g):
    """A port's output latch shows the last word it read; every read must differ from it."""
    program = sm.default_program(g)
    memory = {a: sum(sm.background(a, b) << b for b in range(g.bits)) for a in range(g.words)}
    last = {"A": None, "B": None}
    toggled = {(port, bit, to): False for port in "AB" for bit in range(g.bits) for to in (0, 1)}
    for ops in program:
        for port, op in zip("AB", ops):
            if op.kind == "R":
                word = memory[op.address]
                assert word != last[port], f"port {port} reads {word:b} twice running"
                if last[port] is not None:
                    for bit in range(g.bits):
                        if (word ^ last[port]) >> bit & 1:
                            toggled[(port, bit, word >> bit & 1)] = True
                last[port] = word
        for op in ops:
            if op.kind == "W":
                memory[op.address] = op.data
    assert all(toggled.values()), [key for key, seen in toggled.items() if not seen]
    halves = {g.split(op.address)[0] for ops in program for op in ops if op.kind != "-"}
    columns = {g.split(op.address)[1] for ops in program for op in ops if op.kind == "R"}
    assert halves == {0, 1} and {0, g.mux - 1} <= columns
    assert any(a.kind == "R" and b.kind == "R" and a.address == b.address for a, b in program)  # same-address read
    assert any(a.kind == "W" and b.kind == "W" for a, b in program)  # two-port write


def test_the_spaced_program_is_the_same_program_with_room_after_each_write():
    g = sm.Geometry(2, 4, 2)
    tight, spaced = sm.default_program(g), sm.default_program(g, spaced=True)
    assert [ops for ops in spaced if any(op.kind != "-" for op in ops)] == tight
    for k, ops in enumerate(spaced[:-1]):
        if any(op.kind == "W" for op in ops):
            assert all(op.kind == "-" for op in spaced[k + 1])
    # ... and the tight one really does put a read straight after a write, on both ports.
    assert any(tight[k][p].kind == "W" and tight[k + 1][p].kind == "R" for k in range(len(tight) - 1) for p in (0, 1))


def test_inputs_move_on_the_falling_edge_before_the_edge_that_takes_them():
    line = sm.stimulus("we_n_A", [1, 1, 0, 1], period=2e-9, vdd=0.7, edge=20e-12, first=2e-9)
    assert line.startswith("Vwe_n_A we_n_A 0 PWL(0 0.7 ")
    assert "5e-09 0.7 5.02e-09 0" in line  # falls half a period before edge 2 (at 6 ns)
    assert "7e-09 0 7.02e-09 0.7" in line  # and rises again half a period before edge 3


def test_a_synchronous_driver_launches_inputs_after_the_edge_before():
    # Edge k is at 2 ns + 2k ns: the value for edge 2 (6 ns) launches 60 ps after edge 1 (4 ns).
    line = sm.stimulus("we_n_A", [1, 1, 0, 1], period=2e-9, vdd=0.7, edge=20e-12, first=2e-9,
                       input_delay=60e-12)  # fmt: skip
    assert "4.06e-09 0.7 4.08e-09 0" in line
    assert "6.06e-09 0 6.08e-09 0.7" in line


def test_data_delay_launches_d_alone_later():
    """`data_delay` moves D (the 6T write latch's hold) and leaves the controls at `input_delay`."""
    g = sm.Geometry(wordlines=4, mux=4, bits=2, ports="A")
    cells = [{"path": f"Xc{a}_{b}", "address": a, "bit": b, "stack": "lo"} for a in range(g.words) for b in range(g.bits)]
    pins = ("clk", "rst_n", "ce_n", "we_n", "oe_n", *(f"A[{i}]" for i in range(5)), "D[0]", "D[1]", "Q[0]", "Q[1]")
    program = [(sm.Op("W", 1, 0b11),), (sm.Op("R", 1),)]
    deck, plan = sm.build_deck(Path("dut.sp"), Path("m.pm"), "top", pins, cells, g, program, period=1e-9,
                               vdd=0.7, load=1e-15, input_delay=60e-12, data_delay=150e-12,
                               write_latches=("XIO:X_block",))  # fmt: skip
    d0 = next(line for line in deck.splitlines() if line.startswith("VD_A_0 "))
    ce = next(line for line in deck.splitlines() if line.startswith("Vce_n_A "))
    assert "1.15e-09 0 1.17e-09 0.7" in d0  # D for edge 1 (2 ns) launches 150 ps after edge 0
    assert "1.06e-09 0.7 1.08e-09 0" in ce  # ce_n 60 ps after it
    assert "V(Xdut:XIO:X_block:WRENAN)" in deck
    assert plan["data_delay"] == 150e-12
    with pytest.raises(ValueError, match="data delay needs an input delay"):
        sm.build_deck(Path("dut.sp"), Path("m.pm"), "top", pins, cells, g, program, period=1e-9, vdd=0.7,
                      load=1e-15, data_delay=150e-12)  # fmt: skip


def test_a_probe_on_a_watched_node_is_printed_and_read_once(tmp_path: Path):
    """`--probe sae_A[0]` names a node the deck already prints: a second column
    of that name doubled its samples and slid the wave against TIME, so every
    sense time was wrong and no Q edge was proven."""
    g = sm.Geometry(wordlines=4, mux=4, bits=2, ports="A")
    cells = [{"path": f"Xc{a}_{b}", "address": a, "bit": b, "stack": "lo"} for a in range(g.words) for b in range(g.bits)]
    pins = ("clk", "rst_n", "ce_n", "we_n", "oe_n", *(f"A[{i}]" for i in range(5)), "D[0]", "D[1]", "Q[0]", "Q[1]")
    program = [(sm.Op("W", 1, 0b11),), (sm.Op("R", 1),)]
    deck, _ = sm.build_deck(Path("dut.sp"), Path("m.pm"), "top", pins, cells, g, program, period=1e-9,
                            vdd=0.7, load=1e-15, probes=("SAE_A[0]", "blprechn_A[0]"))  # fmt: skip
    printed = next(line for line in deck.splitlines() if line.startswith(".print")).lower().split()
    assert printed.count("v(xdut:sae_a[0])") == 1
    assert printed.count("v(xdut:blprechn_a[0])") == 1

    # An older run's CSV may still carry the node twice: the reader keeps one column.
    waves = tmp_path / "twice.csv"
    waves.write_text("TIME,V(A),V(B),V(A)\n0,1,2,1\n1e-12,3,4,3\n")
    assert sm.read_csv(waves) == {"TIME": [0.0, 1e-12], "V(A)": [1.0, 3.0], "V(B)": [2.0, 4.0]}


def _result_dir() -> Path:
    given = os.environ.get("OPENFINRAM_MACRO_RESULT")
    built = [Path(given)] if given else sorted((REPO / "results").glob("sram_x4x2x1_*"))
    if not built or not list(built[-1].glob("*.physical.json")):
        pytest.skip("no built two-port sram_x4x2x1 result")
    return built[-1]


@pytest.mark.skipif(os.environ.get("OPENFINRAM_SLOW_TESTS") != "1", reason="two six-minute Xyce runs")
def test_the_macro_reads_and_writes_back_to_back_and_spaced(tmp_path: Path):
    """Both programs pass: every read right and proven after sense enable, every
    cell intact, no hazard.

    Until 2026-09-22 `sram_control.v` made ``wrena = clk`` while the state
    register said WRITE, so on the rising edge after a write write enable
    pulsed for one clock-to-Q and the back-to-back program failed on it.  Write
    enable now follows the delayed clock (``wl_any_fire``), as the wordline
    does, and by the time that edge arrives the state has settled.
    """
    try:
        sm.find_xyce()
    except FileNotFoundError as error:
        pytest.skip(str(error))
    result = _result_dir()
    with ThreadPoolExecutor(2) as pool:
        spaced = pool.submit(sm.simulate, result, tmp_path / "spaced", spaced=True)
        tight = pool.submit(sm.simulate, result, tmp_path / "tight")
        spaced, tight = spaced.result(), tight.result()

    assert spaced["passed"], sm.describe(spaced)
    assert spaced["cells_checked"] == 32 and not spaced["hazards"] and not spaced["unproven"]
    # A read whose Q already held the word has no arrival to time.
    assert max(r["clk_to_q_ps"] for r in spaced["reads"] if r["clk_to_q_ps"] is not None) < 400  # 285 ps, TT

    assert tight["passed"], sm.describe(tight)
    assert tight["cells_checked"] == 32 and not tight["hazards"] and not tight["unproven"]
    assert tight["cells_wrong"] == [] and all(r["ok"] for r in tight["reads"])
