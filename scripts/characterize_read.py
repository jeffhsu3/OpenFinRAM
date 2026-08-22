#!/usr/bin/env python3
"""Read-access characterization prototype for an OpenFinRAM 6T SRAM column.

Measures the SRAM read-access delay (wordline rise -> bitline develops a sense
margin) as a real transient SPICE measurement, and sweeps column depth to show
the delay is NOT constant -- the thing a FakeRAM .lib (fixed clk->Q) cannot
capture. This is the measurement kernel a real Liberty characterizer wraps
(one arc, one corner); setup/hold/power/leakage arcs follow the same pattern.

Method (standard SRAM read critical path):
  * one REAL 6T bitcell (sram_cell_6t_122 topology) holding Q=0,
  * bitlines precharged to VDD, loaded by the rest of the column as an
    equivalent per-cell capacitance (depth = N cells on the bitline),
  * pulse the accessed wordline; the cell discharges BL,
  * t_access = WL(50%) -> |BL-BLN| reaches the sense margin.

Simulators:
  * ngspice (default) has NO BSIM-CMG in this build, so it runs a planar
    stand-in device -- the HARNESS is exercised but the ps are NOT ASAP7.
  * Xyce (--simulator xyce) has BSIM-CMG. For real ASAP7 numbers the harness
    remaps the card's HSPICE `level 72` -> Xyce `level 110` and emits `NFIN`
    (BSIM-CMG rejects `W`). That is the only device-syntax delta; same deck.

Modes: `--mode access` times WL->BL to the sense margin (the read critical
path up to the sense amp). `--mode clkq` runs the full read through the REAL
OpenFinRAM sense amp + io_nand latch + tristate buffer to Q, self-timing SAE
per depth from the access measurement (replica timing) -- the true clk->Q arc.

    # stand-in, ngspice (harness smoke test):
    uv run python scripts/characterize_read.py --depths 16,32,64,128,256
    # REAL ASAP7 TT access, Xyce:
    uv run python scripts/characterize_read.py --simulator xyce --real-device \
        --models /home/jeff/iv4/repos/OpenFinRAM/tech/models/hspice/7nm_TT.pm
    # REAL ASAP7 TT true clk->Q (through the sense amp):
    uv run python scripts/characterize_read.py --mode clkq --simulator xyce \
        --real-device --models .../7nm_TT.pm
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

VDD = 0.7
SENSE_MARGIN = 0.1  # V of BL develop that trips the sense amp
CBL_PER_CELL_F = 0.06e-15  # per-cell bitline capacitance (wire + junction), F

# Planar stand-in devices so the harness runs in a BSIM-CMG-less ngspice.
# Tuned for ~0.7 V bistability + a readable bitline discharge, NOT ASAP7-accurate.
STANDIN_MODELS = """\
* planar stand-in for nmos_sram/pmos_sram (ngspice has no BSIM-CMG here)
.model nmos_sram nmos level=1 vto=0.16 kp=700u lambda=0.08 \
cgso=8e-11 cgdo=8e-11 cbd=1e-16 cbs=1e-16
.model pmos_sram pmos level=1 vto=-0.16 kp=350u lambda=0.08 \
cgso=8e-11 cgdo=8e-11 cbd=1e-16 cbs=1e-16
"""


def gen_deck(depth: int, models_inc: str, real_device: bool, simulator: str) -> str:
    """One read-access deck for a column of `depth` cells (accessed cell reads 0)."""

    # BSIM-CMG (FinFET) takes NFIN and rejects W; the planar stand-in takes W.
    def dev(name, d, g, s, b, model, nfin):
        geom = (
            f"nfin={nfin}"
            if real_device
            else ("W=5.4e-08" if model[0] == "n" else "W=2.7e-08")
        )
        return f"{name} {d} {g} {s} {b} {model} L=2e-08 {geom}"

    cbl = depth * CBL_PER_CELL_F
    sense_v = VDD - SENSE_MARGIN
    uic = " uic" if simulator == "ngspice" else ""  # Xyce honors .ic via its DCOP
    cells = "\n".join(
        [
            dev("M0", "QB", "WL", "BLN", "0", "nmos_sram", 2),
            dev("M1", "Q", "QB", "0", "0", "nmos_sram", 2),
            dev("M2", "0", "Q", "QB", "0", "nmos_sram", 2),
            dev("M3", "BL", "WL", "Q", "0", "nmos_sram", 2),
            dev("M4", "Q", "QB", "VDD", "VDD", "pmos_sram", 1),
            dev("M5", "VDD", "Q", "QB", "VDD", "pmos_sram", 1),
        ]
    )
    return f"""* read-access characterization, column depth = {depth}
{models_inc}
VVDD VDD 0 {VDD}

* accessed 6T cell (sram_cell_6t_122 topology), storage nodes Q/QB exposed
{cells}

* rest of the column modeled as equivalent bitline capacitance (depth = load)
Cbl  BL  0 {cbl:.4e}
Cbln BLN 0 {cbl:.4e}

* precharge bitlines to VDD, cell holds Q=0 / QB=1
.ic v(BL)={VDD} v(BLN)={VDD} v(Q)=0 v(QB)={VDD}

* wordline read pulse at t=1n
VWL WL 0 PULSE(0 {VDD} 1n 10p 10p 5n 10n)

.tran 1p 3n{uic}
* t_access: WL crosses 50% -> BL develops SENSE_MARGIN below precharge
.measure tran t_access TRIG v(WL) VAL={VDD / 2} RISE=1 \
TARG v(BL) VAL={sense_v:.4f} FALL=1
.measure tran bl_final FIND v(BL) AT=2.9n
.end
"""


# Real OpenFinRAM periphery subckts (W stripped at emit time for BSIM-CMG).
SUBCKTS = r"""
.SUBCKT sram_prech_ymux_6t112_v1 blprechn yseln ysel san sa bln bl VDD VSS
M0 bln ysel san VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1 bl ysel sa VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M2 bln blprechn VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M3 bl blprechn VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M4 san yseln bln VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5 sa yseln bl VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
.ENDS
.SUBCKT io_nand_3f_6f A B Y VDD VSS
M0 VSS A 6 VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1 6 A VSS VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M2 Y B 6 VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M3 6 B Y VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M4 VDD A Y VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5 Y B VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
.ENDS
.SUBCKT TBUF_INV A ENB EN VDD VSS Y
MP1 n1 A VDD   VDD pmos_rvt L=2e-08 W=1296.00n nfin=48
MP2 Y   ENB  n1 VDD pmos_rvt L=2e-08 W=972.00n nfin=36
MN1 n2 A VSS   VSS nmos_rvt L=2e-08 W=1296.00n nfin=48
MN2 Y   EN  n2 VSS nmos_rvt L=2e-08 W=972.00n nfin=36
.ENDS
.SUBCKT sense_amp_sram sa san SAE SAPRECHN qa qan vdd vss
M0 qan SAPRECHN vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M1 59  SAPRECHN qan vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M2 qa  SAPRECHN 59  vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M3 vdd SAPRECHN qa  vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M4 vdd qa  qan vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M5 qa  qan vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M6  52  sa  57  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M7  58  san 52  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M8  57  qa  qan vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M9 qa  qan 58  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M10  vss SAE 52  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M11  52  SAE vss vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M12  qan vss vss vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M13 vss vss qa  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M14 qan vdd vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M15 vdd vdd qa  vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
.ENDS
"""


def strip_w(text: str) -> str:
    """Drop `W=<val>` from device lines (BSIM-CMG uses NFIN and rejects W)."""
    return re.sub(r"\s+[wW]=\S+", "", text)


# Real ASAP7 D flop (from tech/cdl); the SRAM's A/D/WE input register.
DFF_SUBCKT = r"""
.SUBCKT DFFHQNx1_ASAP7_75t_R CLK D QN VDD VSS
MM24 QN SH VSS VSS nmos_rvt w=81.0n l=20n nfin=3
MM23 clkb clkn VSS VSS nmos_rvt w=54.0n l=20n nfin=2
MM20 clkn CLK VSS VSS nmos_rvt w=54.0n l=20n nfin=2
MM17 SH clkn pd5 VSS nmos_rvt w=27.0n l=20n nfin=1
MM16 pd5 SS VSS VSS nmos_rvt w=27.0n l=20n nfin=1
MM14 SS SH VSS VSS nmos_rvt w=27.0n l=20n nfin=1
MM12 MS clkb SH VSS nmos_rvt w=27.0n l=20n nfin=1
MM9 MH clkb pd3 VSS nmos_rvt w=27.0n l=20n nfin=1
MM8 pd3 MS VSS VSS nmos_rvt w=27.0n l=20n nfin=1
MM6 MS MH VSS VSS nmos_rvt w=27.0n l=20n nfin=1
MM5 pd1 D VSS VSS nmos_rvt w=81.0n l=20n nfin=3
MM4 MH clkn pd1 VSS nmos_rvt w=81.0n l=20n nfin=3
MM25 QN SH VDD VDD pmos_rvt w=81.0n l=20n nfin=3
MM22 clkb clkn VDD VDD pmos_rvt w=54.0n l=20n nfin=2
MM21 clkn CLK VDD VDD pmos_rvt w=54.0n l=20n nfin=2
MM19 pd4 SS VDD VDD pmos_rvt w=27n l=20n nfin=1
MM18 SH clkb pd4 VDD pmos_rvt w=27n l=20n nfin=1
MM15 SS SH VDD VDD pmos_rvt w=27n l=20n nfin=1
MM13 MS clkn SH VDD pmos_rvt w=27n l=20n nfin=1
MM11 pd2 MS VDD VDD pmos_rvt w=27n l=20n nfin=1
MM10 MH clkn pd2 VDD pmos_rvt w=27n l=20n nfin=1
MM7 MS MH VDD VDD pmos_rvt w=27n l=20n nfin=1
MM1 MH clkb pu1 VDD pmos_rvt w=81.0n l=20n nfin=3
MM3 pu1 D VDD VDD pmos_rvt w=81.0n l=20n nfin=3
.ENDS
"""


def gen_dff_deck(t_ps: float, kind: str, models_inc: str) -> str:
    """DFF timing deck. setup: D rises to 1 at clk - t_ps (captured -> QN=0).
    hold: D=1 held, falls to 0 at clk + t_ps (still captured -> QN=0)."""
    tclk = 2.0
    if kind == "setup":
        td = tclk - t_ps / 1000.0
        dsrc = f"VD D 0 PWL(0 0 {td - 0.005:.4f}n 0 {td:.4f}n {VDD} 4n {VDD})"
    else:  # hold: D high before clk, falls after
        th = tclk + t_ps / 1000.0
        dsrc = f"VD D 0 PWL(0 {VDD} {th - 0.005:.4f}n {VDD} {th:.4f}n 0 4n 0)"
    return f"""* DFF {kind} bisection point, t={t_ps:.2f}ps
{models_inc}
{strip_w(DFF_SUBCKT)}
VVDD VDD 0 {VDD}
Xdff CLK D QN VDD 0 DFFHQNx1_ASAP7_75t_R
Cqn QN 0 0.4e-15
Vclk CLK 0 PULSE(0 {VDD} {tclk}n 5p 5p 2n 8n)
{dsrc}
.ic v(QN)={VDD}
.tran 1p 3.8n
.measure tran qn_final FIND v(QN) AT=3.6n
.end
"""


def bisect_timing(
    kind: str, models_inc: str, sim: str, exe: str, workdir: Path
) -> float:
    """Binary-search the capture-failure boundary. Returns the constraint in ps.

    A read of QN < VDD/2 means D=1 was captured (pass). We find the smallest
    setup (or hold) at which the flop still captures correctly.
    """
    lo, hi = -30.0, 250.0  # ps: hi = generous (passes), lo = violated (fails)
    for i in range(13):
        mid = (lo + hi) / 2
        d = workdir / f"dff_{kind}_{i}.sp"
        d.write_text(gen_dff_deck(mid, kind, models_inc))
        log = run_sim(d, sim, exe)
        qn = parse_measure(log, "qn_final")
        captured = qn is not None and qn < VDD / 2
        if captured:
            hi = mid  # met -> try tighter
        else:
            lo = mid  # violated -> need more margin
    return hi


def _cell(prefix: str, stored: int, wl: str) -> tuple[str, str]:
    """A top-level 6T cell storing `stored`, accessed by wordline `wl`."""
    q, qb = (f"{prefix}Q", f"{prefix}QB")
    ic_q, ic_qb = (0, VDD) if stored == 0 else (VDD, 0)
    lines = [
        f"{prefix}0 {qb} {wl} BLN 0 nmos_sram L=2e-08 nfin=2",
        f"{prefix}1 {q} {qb} 0 0 nmos_sram L=2e-08 nfin=2",
        f"{prefix}2 0 {q} {qb} 0 nmos_sram L=2e-08 nfin=2",
        f"{prefix}3 BL {wl} {q} 0 nmos_sram L=2e-08 nfin=2",
        f"{prefix}4 {q} {qb} VDD VDD pmos_sram L=2e-08 nfin=1",
        f"{prefix}5 VDD {q} {qb} VDD pmos_sram L=2e-08 nfin=1",
    ]
    return "\n".join(lines), f".ic v({q})={ic_q} v({qb})={ic_qb}"


def gen_clkq_deck(
    depth: int,
    tsae_ps: float,
    models_inc: str,
    slew_s: float = 10e-12,
    cq_f: float = 1e-15,
    table3: bool = False,
) -> str:
    """True clk->Q via the real sense amp + output stage (Xyce/BSIM-CMG only).

    Two 4 ns cycles: read a stored-1 (Q settles high), then the stored-0 under
    test (Q falls) -> a clean clk->Q edge. SAE is replica-timed at clk+tsae.

    table3=True applies the MOST-report stimulus conditions (input slew
    0.04 ns, output load 0.04608 pF) and adds cell rise/fall access plus
    output rise/fall transition measurements.
    """
    cbl = depth * CBL_PER_CELL_F
    sae1 = 1.0 + tsae_ps / 1000.0
    sl = slew_s * 1e9  # s -> ns for the PULSE cards
    cell_lo, ic_lo = _cell("ML", 0, "WLlo")  # value under test (cycle 2)
    cell_hi, ic_hi = _cell("MH", 1, "WLhi")  # pre-sets Q high (cycle 1)
    vth = VDD / 2
    # 10%/90% points for the output transition measurements.
    v10, v90 = 0.1 * VDD, 0.9 * VDD
    extra_measures = ""
    if table3:
        extra_measures = f"""\
* cell rise/fall access (clk 50% -> Q 50%)
.measure tran t_cell_rise TRIG v(clk) VAL={vth} RISE=1 TARG v(Q) VAL={vth} RISE=1
.measure tran t_cell_fall TRIG v(clk) VAL={vth} RISE=2 TARG v(Q) VAL={vth} FALL=1
* output transitions (10%->90% rise, 90%->10% fall)
.measure tran t_qr_hi WHEN v(Q)={v90:.3f} RISE=1
.measure tran t_qr_lo WHEN v(Q)={v10:.3f} RISE=1
.measure tran t_qf_hi WHEN v(Q)={v90:.3f} FALL=1
.measure tran t_qf_lo WHEN v(Q)={v10:.3f} FALL=1
.measure tran q_hi FIND v(Q) AT=4.5n
"""
    return f"""* true clk->Q read (2-cycle), depth={depth}, SAE +{tsae_ps:.0f}ps
{models_inc}
{strip_w(SUBCKTS)}
VVDD VDD 0 {VDD}
{cell_lo}
{cell_hi}
Xmux  blprechn yseln ysel san sa BLN BL VDD 0 sram_prech_ymux_6t112_v1
Xsa   sa san SAE SAPRECHN qa qan VDD 0 sense_amp_sram
Xn0   n49 qa  n54 VDD 0 io_nand_3f_6f
Xn1   n54 qan n49 VDD 0 io_nand_3f_6f
Mi_n n48 n49 0   0   nmos_rvt L=2e-08 nfin=3
Mi_p n48 n49 VDD VDD pmos_rvt L=2e-08 nfin=3
Xtbuf n48 oeb_out oe_out VDD 0 Q TBUF_INV
Cq Q 0 {cq_f:.5e}
Cbl  BL  0 {cbl:.4e}
Cbln BLN 0 {cbl:.4e}
{ic_lo}
{ic_hi}
.ic v(BL)={VDD} v(BLN)={VDD} v(sa)={VDD} v(san)={VDD} v(qa)={VDD} v(qan)={VDD} v(Q)=0
Vclk  clk 0 PULSE(0 {VDD} 1n {sl:g}n {sl:g}n 2n 4n)
Vbpn  blprechn 0 PULSE(0 {VDD} 1n {sl:g}n {sl:g}n 2n 4n)
Vysel ysel 0 PULSE(0 {VDD} 1n {sl:g}n {sl:g}n 2n 4n)
Vyseln yseln 0 PULSE({VDD} 0 1n {sl:g}n {sl:g}n 2n 4n)
Voe   oe_out  0 PULSE(0 {VDD} 1n {sl:g}n {sl:g}n 2n 4n)
Voeb  oeb_out 0 PULSE({VDD} 0 1n {sl:g}n {sl:g}n 2n 4n)
Vwlh  WLhi 0 PULSE(0 {VDD} 1n {sl:g}n {sl:g}n 1.8n 100n)
Vwll  WLlo 0 PULSE(0 {VDD} 5n {sl:g}n {sl:g}n 1.8n 100n)
Vsapn SAPRECHN 0 PULSE(0 {VDD} {sae1 - 0.02:.4f}n {sl:g}n {sl:g}n 1.85n 4n)
Vsae  SAE 0 PULSE(0 {VDD} {sae1:.4f}n {sl:g}n {sl:g}n 1.8n 4n)
.tran 1p 8n
.measure tran t_clkq TRIG v(clk) VAL={vth} RISE=2 TARG v(Q) VAL={vth} FALL=1 TD=4n
.measure tran q_lo FIND v(Q) AT=7.5n
{extra_measures}.end
"""


def run_sim(deck: Path, sim: str, exe: str) -> str:
    """Run the deck; return the text that carries the .measure results."""
    if sim == "ngspice":
        proc = subprocess.run(
            [exe, "-b", str(deck)], capture_output=True, text=True, timeout=300
        )
        return proc.stdout + "\n" + proc.stderr
    # Xyce writes measures to <deck>.mt0
    subprocess.run([exe, str(deck)], capture_output=True, text=True, timeout=300)
    mt0 = Path(str(deck) + ".mt0")
    return mt0.read_text() if mt0.exists() else ""


XYCE_DEFAULT = "/home/jeff/iv4/local/xyce-14.4/bin/Xyce"

# Table III stimulus conditions (MOST report): input slew 0.04 ns,
# output load 0.04608 pF.
TABLE3_SLEW_S = 0.04e-9
TABLE3_CQ_F = 0.04608e-12

# PDK reference column of MOST-report Table III (ns):
# instance -> (cell rise, rise transition, cell fall, fall transition)
TABLE3_PDK = {
    256: (0.250, 0.072, 0.252, 0.057),
    512: (0.270, 0.070, 0.264, 0.053),
    1024: (0.297, 0.068, 0.287, 0.058),
}


def prep_models(models: Path | None, simulator: str, workdir: Path) -> tuple[str, str]:
    """Return (include_text, description). Adapt an ASAP7 HSPICE card for Xyce."""
    if models is None:
        return STANDIN_MODELS, "planar stand-in (NOT ASAP7-accurate)"
    if simulator == "xyce":
        # ASAP7 ships HSPICE level 72; Xyce BSIM-CMG is level 110.
        adapted = workdir / f"{models.stem}_xyce.pm"
        adapted.write_text(models.read_text().replace("level = 72", "level = 110"))
        return (
            f".include {adapted}",
            f"{models.name} -> Xyce BSIM-CMG L110 (real ASAP7)",
        )
    return f".include {models}", f"{models.name} (real device card)"


def parse_measure(log: str, name: str) -> float | None:
    # ngspice: "t_access = 1.234560e-10"; Xyce: "T_ACCESS = 1.23e-10"
    m = re.search(rf"{name}\s*=\s*([0-9.eE+\-]+)", log, re.I)
    if not m:
        return None
    try:
        return float(m.group(1))
    except ValueError:
        return None


def run_table3(
    args, depths_arg: str, models_inc: str, model_desc: str, exe: str
) -> int:
    """Reproduce MOST-report Table III: cell rise/fall access and output
    rise/fall transitions for 256/512/1024-word x64-bit configurations,
    measured on the real read path (cell -> bitline -> sense amp -> latch ->
    tristate -> Q) with replica-timed SAE, at the report's stimulus point
    (input slew 0.04 ns, output load 0.04608 pF, TT corner).

    Depth = words per bitline (the bitline spans every row of the column).
    """
    if args.simulator != "xyce" or not args.real_device:
        print(
            "table3 requires --simulator xyce --real-device (real ASAP7 "
            "BSIM-CMG models); stand-in devices would produce meaningless ns."
        )
        return 1

    words = [int(x) for x in depths_arg.split(",")]
    print(
        f"Table III reproduction | sim=xyce | model={model_desc} | "
        f"input slew {TABLE3_SLEW_S * 1e9:.2f} ns, load {TABLE3_CQ_F * 1e12:.5f} pF"
    )
    print(
        "  methodology: transient clk->Q through the real periphery with "
        "replica-timed SAE (not an interpolated .lib); TT corner only."
    )

    def diff(ours_s: float | None, pdk_ns: float) -> str:
        if ours_s is None:
            return "FAIL"
        pct = (ours_s * 1e9 - pdk_ns) / pdk_ns * 100.0
        return f"{pct:+.1f}"

    header = (
        f"{'instance':>18} | {'metric':>16} | {'PDK (ns)':>8} | "
        f"{'ours (ns)':>9} | {'diff (%)':>8}"
    )
    print(header)
    print("-" * len(header))

    had_fail = False
    for w in words:
        # Replica timing: measure BL-develop access first, then fire SAE with
        # the same guard used by clkq mode.
        adeck = args.workdir / f"read_d{w}.sp"
        adeck.write_text(gen_deck(w, models_inc, True, "xyce"))
        alog = run_sim(adeck, "xyce", exe)
        t_acc = parse_measure(alog, "t_access")
        sae_ps = (t_acc * 1e12) + 85 if t_acc else 150.0

        deck = args.workdir / f"table3_d{w}.sp"
        deck.write_text(
            gen_clkq_deck(
                w,
                sae_ps,
                models_inc,
                slew_s=TABLE3_SLEW_S,
                cq_f=TABLE3_CQ_F,
                table3=True,
            )
        )
        log = run_sim(deck, "xyce", exe)
        (args.workdir / f"table3_d{w}.log").write_text(log)

        g = lambda n: parse_measure(log, n)  # noqa: E731
        rise = g("t_cell_rise")
        fall = g("t_cell_fall")
        qr10, qr90 = g("t_qr_lo"), g("t_qr_hi")
        qf90, qf10 = g("t_qf_hi"), g("t_qf_lo")
        rise_tr = (qr90 - qr10) if (qr90 is not None and qr10 is not None) else None
        fall_tr = (qf10 - qf90) if (qf10 is not None and qf90 is not None) else None
        q_hi, q_lo = g("q_hi"), g("q_lo")
        sane = (
            q_hi is not None
            and q_hi > 0.6
            and q_lo is not None
            and q_lo < 0.1
        )
        if not sane:
            had_fail = True
            print(f"{w}-word x 64-bit | OUTPUT SANITY FAIL (q_hi/q_lo) — see log")
            continue

        pdk = TABLE3_PDK.get(w, (None,) * 4)
        rows = [
            ("Cell rise", rise, pdk[0]),
            ("Rise transition", rise_tr, pdk[1]),
            ("Cell fall", fall, pdk[2]),
            ("Fall transition", fall_tr, pdk[3]),
        ]
        inst = f"{w}-word x 64-bit"
        for metric, ours, ref in rows:
            ours_v = f"{ours * 1e9:.3f}" if ours is not None else "FAIL"
            if ours is None:
                had_fail = True
            if ref is not None:
                print(f"{inst:>18} | {metric:>16} | {ref:.3f} | {ours_v:>9} | "
                      f"{diff(ours, ref):>8}")
            else:
                print(f"{inst:>18} | {metric:>16} |       - | {ours_v:>9} | "
                      f"{'-':>8}")
        print()

    print(
        "* Note: input slew = 0.04 ns, output load cap. = 0.04608 pF, TT "
        "corner (tech/models/hspice/7nm_TT.pm). PDK column from MOST-report "
        "Table III (SiliconSmart-characterized .lib)."
    )
    return 1 if had_fail else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--depths", default="16,32,64,128,256")
    ap.add_argument(
        "--mode",
        default="access",
        choices=["access", "clkq", "setuphold", "table3"],
        help="access = WL->BL sense-margin; clkq = full read through the real "
        "sense amp; setuphold = A/D/WE input-register setup/hold via bisection; "
        "table3 = MOST-report Table III reproduction (cell rise/fall access + "
        "output transitions vs the report's PDK column; needs --simulator xyce "
        "--real-device)",
    )
    ap.add_argument("--simulator", default="ngspice", choices=["ngspice", "xyce"])
    ap.add_argument(
        "--models",
        type=Path,
        default=None,
        help="path to a device model card to .include (else planar stand-in)",
    )
    ap.add_argument(
        "--real-device",
        action="store_true",
        help="FinFET: emit NFIN (not W); use with a real ASAP7 card + Xyce",
    )
    ap.add_argument(
        "--sim-exe",
        default=None,
        help=f"simulator executable (default: ngspice, or {XYCE_DEFAULT} for xyce)",
    )
    ap.add_argument("--workdir", type=Path, default=Path("/tmp/ofr_charread"))
    args = ap.parse_args(argv)
    args.workdir.mkdir(parents=True, exist_ok=True)

    exe = args.sim_exe or (XYCE_DEFAULT if args.simulator == "xyce" else "ngspice")
    models_inc, model_desc = prep_models(args.models, args.simulator, args.workdir)

    if args.mode == "table3":
        return run_table3(args, depths_arg=args.depths, models_inc=models_inc,
                          model_desc=model_desc, exe=exe)

    if args.mode == "setuphold":
        print(
            f"setup/hold (A/D/WE input register) | sim={args.simulator} | "
            f"model={model_desc}"
        )
        t_su = bisect_timing("setup", models_inc, args.simulator, exe, args.workdir)
        t_hold = bisect_timing("hold", models_inc, args.simulator, exe, args.workdir)
        print("  DFFHQNx1 (real ASAP7 flop), rising edge, D 0->1 capture:")
        print(f"    setup = {t_su:.1f} ps   hold = {t_hold:.1f} ps")
        print(
            "  (capture-failure boundary via bisection; real Liberty adds a "
            "clk->Q pushout criterion. This is the constraint on A/D/WE vs CLK.)"
        )
        return 0

    depths = [int(x) for x in args.depths.split(",")]
    metric = "clk->Q" if args.mode == "clkq" else "t_access"
    print(f"{args.mode} characterization | sim={args.simulator} | model={model_desc}")
    hdr = (
        "clk->Q (ps) | SAE (ps) | read"
        if args.mode == "clkq"
        else "t_access (ps) | BL final (V)"
    )
    print(f"{'depth (cells)':>14} | {'BL cap (fF)':>11} | {hdr}")
    print("-" * 68)

    rows: list[tuple[int, float | None]] = []
    for d in depths:
        # BL-develop access always measured (it is the result in access mode, and
        # the replica timing that sets SAE in clkq mode).
        adeck = args.workdir / f"read_d{d}.sp"
        adeck.write_text(gen_deck(d, models_inc, args.real_device, args.simulator))
        alog = run_sim(adeck, args.simulator, exe)
        (args.workdir / f"read_d{d}.log").write_text(alog)
        t_acc = parse_measure(alog, "t_access")

        if args.mode == "access":
            vbl = parse_measure(alog, "bl_final")
            rows.append((d, t_acc))
            tps = f"{t_acc * 1e12:.2f}" if t_acc else "FAIL"
            vbls = f"{vbl:.3f}" if vbl is not None else "-"
            print(
                f"{d:>14} | {d * CBL_PER_CELL_F * 1e15:>11.2f} | {tps:>13} | {vbls:>12}"
            )
        else:
            # replica timing: fire SAE after BL develops the sense margin + guard
            sae_ps = (t_acc * 1e12) + 85 if t_acc else 150.0
            qdeck = args.workdir / f"clkq_d{d}.sp"
            qdeck.write_text(gen_clkq_deck(d, sae_ps, models_inc))
            qlog = run_sim(qdeck, args.simulator, exe)
            (args.workdir / f"clkq_d{d}.log").write_text(qlog)
            tq = parse_measure(qlog, "t_clkq")
            qlo = parse_measure(qlog, "q_lo")
            read_ok = "ok" if (qlo is not None and qlo < 0.1) else "FAIL"
            rows.append((d, tq))
            tqs = f"{tq * 1e12:.1f}" if tq else "FAIL"
            print(
                f"{d:>14} | {d * CBL_PER_CELL_F * 1e15:>11.2f} | {tqs:>11} | "
                f"{sae_ps:>8.0f} | {read_ok}"
            )

    ok = [(d, t) for d, t in rows if t is not None]
    if len(ok) >= 2:
        (d0, t0), (d1, t1) = ok[0], ok[-1]
        if t0 and t1:
            print("-" * 68)
            print(
                f"{metric} grew {t1 / t0:.2f}x from depth {d0} to {d1} "
                f"({t0 * 1e12:.1f} -> {t1 * 1e12:.1f} ps) -> NON-CONSTANT "
                f"(FakeRAM emits one fixed value)."
            )
    else:
        print("!! no successful measurements; check a .log in", args.workdir)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
