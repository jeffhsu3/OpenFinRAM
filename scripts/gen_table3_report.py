#!/usr/bin/env python3
"""Generate reports/table3_timing_comparison.md.

Assembles a MOST-report-Table-III-style timing comparison from every source
available without commercial tools:

  * vendor characterized Liberty (asap7_sram_0p0/generated/LIB, Cadence flow)
    -- clk->Q arcs evaluated at the report's stimulus point
       (input slew 40 ps, output load 46.08 fF);
  * MOST-report Table III columns (transcribed from the PDF);
  * the OpenFinRAM estimated .lib written by --skip-characterization
    (deliberately flat FakeRAM-style early-PPA tables);
  * OpenFinRAM Xyce transient measurements (characterize_read.py --mode
    table3 workdir).

Units caveat handled here: the srambank .lib headers declare ns/pF, but their
delay tables are numerically in ps/fF (the grid contains exactly the report's
40 ps / 46.08 fF stimulus point). We interpret table numerics as ps and
convert to ns.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

DEFAULT_VENDOR_LIB = Path(
    "/home/jeff/iv4/repos/asap7/asap7_sram_0p0/generated/LIB"
)

# MOST-report Table III (transcribed): instance words -> (PDK, ours)
# metric order: cell rise, rise transition, cell fall, fall transition [ns]
REPORT = {
    256: {
        "pdk": (0.250, 0.072, 0.252, 0.057),
        "ours": (0.216, 0.064, 0.216, 0.048),
    },
    512: {
        "pdk": (0.270, 0.070, 0.264, 0.053),
        "ours": (0.245, 0.063, 0.246, 0.048),
    },
    1024: {
        "pdk": (0.297, 0.068, 0.287, 0.058),
        "ours": (0.369, 0.061, 0.367, 0.047),
    },
}

METRICS = ["Cell rise", "Rise transition", "Cell fall", "Fall transition"]


def _numbers(text: str) -> list[float]:
    return [float(x) for x in re.findall(r"[-+]?[\d.]+(?:[eE][-+]?\d+)?", text)]


def parse_vendor_lib(path: Path) -> dict[str, float]:
    """clk->Q timing at (slew 40 ps, cap 46.08 fF) from a srambank .lib."""
    text = path.read_text()
    out: dict[str, float] = {}
    for block_name, key in [
        ("cell_rise", "cell_rise"),
        ("rise_transition", "rise_transition"),
        ("cell_fall", "cell_fall"),
        ("fall_transition", "fall_transition"),
    ]:
        m = re.search(
            rf"{block_name}\s*\([^)]*\)\s*\{{(.*?)\n\s*\}}", text, re.S
        )
        if m is None:
            continue
        body = m.group(1)
        i1 = _numbers(re.search(r'index_1\s*\("([^"]*)"', body).group(1))
        i2 = _numbers(re.search(r'index_2\s*\("([^"]*)"', body).group(1))
        rows = [
            _numbers(r)
            for r in re.findall(r'"([^"]*)"', re.search(r"values\s*\((.*?)\)", body, re.S).group(1))
        ]
        # Grid numerics are ps (slew) / fF (cap).
        si, sj = i1.index(40.0), i2.index(46.08)
        out[key] = rows[si][sj] / 1000.0  # -> ns
    return out


def parse_estimate_lib(path: Path) -> dict[str, float]:
    """Flat early-PPA estimate .lib: first table entry per metric (ns)."""
    text = path.read_text()
    out = {}
    for key in ["cell_rise", "cell_fall"]:
        m = re.search(rf"{key}\s*\([^)]*\)\s*\{{.*?values\s*\(([^)]*)\)", text, re.S)
        if m:
            out[key] = _numbers(m.group(1))[0]
    for key in ["rise_transition", "fall_transition"]:
        m = re.search(rf"{key}\s*\([^)]*\)\s*\{{.*?index_1\s*\((\"[^\"]*\")\).*?values\s*\(([^)]*)\)", text, re.S)
        if m:
            # Flat slew template: value scales with the load index; take the
            # entry nearest the report's 0.04608 pF point of index space.
            vals = _numbers(m.group(2))
            out[key] = vals[len(vals) // 2]
    return out


def parse_xyce(mt0: Path) -> dict[str, float]:
    """Xyce .measure results (seconds -> ns)."""
    text = mt0.read_text()

    def get(name: str) -> float | None:
        m = re.search(rf"^{name}\s*=\s*([-+\d.eE]+)", text, re.M | re.I)
        return float(m.group(1)) * 1e9 if m else None

    out = {}
    if (r := get("t_cell_rise")) is not None:
        out["cell_rise"] = r
    if (f := get("t_cell_fall")) is not None:
        out["cell_fall"] = f
    qr_hi, qr_lo = get("t_qr_hi"), get("t_qr_lo")
    qf_hi, qf_lo = get("t_qf_hi"), get("t_qf_lo")
    if None not in (qr_hi, qr_lo):
        out["rise_transition"] = abs(qr_hi - qr_lo)
    if None not in (qf_hi, qf_lo):
        out["fall_transition"] = abs(qf_lo - qf_hi)
    return out


def fmt(v: float | None) -> str:
    return f"{v:.3f}" if v is not None else "--"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vendor-lib-dir", type=Path, default=DEFAULT_VENDOR_LIB)
    ap.add_argument(
        "--estimate-lib",
        type=Path,
        nargs="*",
        help="OpenFinRAM estimated .lib files (results/<run>/<cell>.lib)",
    )
    ap.add_argument(
        "--xyce-mt0",
        type=Path,
        nargs="*",
        help="Xyce measure files named <anything>_d<words>.sp.mt0",
    )
    ap.add_argument("--words", type=int, nargs="*", default=[128, 256, 512, 1024])
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args(argv)

    est_by_words: dict[int, tuple[Path, dict]] = {}
    for p in args.estimate_lib or []:
        m = re.search(r"x(\d+)x\d+x\d+\.lib$", p.name)
        if m:
            est_by_words[int(m.group(1))] = (p, parse_estimate_lib(p))

    xyce_by_words: dict[int, dict] = {}
    for p in args.xyce_mt0 or []:
        m = re.search(r"_d(\d+)\.sp\.mt0$", p.name)
        if m:
            xyce_by_words[int(m.group(1))] = parse_xyce(p)

    lines = [
        "# Table III reproduction: read-timing comparison",
        "",
        "All values are ns. Stimulus point: input slew 0.04 ns, output load",
        "0.04608 pF, TT corner.",
        "",
    ]
    for w in args.words:
        vendor = {}
        vpath = args.vendor_lib_dir / f"srambank_{w}x4x64_6t122.lib"
        if vpath.exists():
            vendor = parse_vendor_lib(vpath)
        est = est_by_words.get(w, (None, {}))[1]
        xyce = xyce_by_words.get(w, {})
        rep = REPORT.get(w)

        title = f"## {w}-word x 64-bit (16 Kb class)"
        if w == 128:
            title = f"## {w}-word x 64-bit (8 Kb class)"
        lines += [title, "", "| Metric | Vendor .lib (Cadence char.) | Report PDK | "
                  "Report Ours | OS .lib estimate | OS Xyce transient |",
                  "|---|---|---|---|---|---|"]
        for i, metric in enumerate(METRICS):
            v = vendor.get(_VKEY[i])
            r_pdk = rep["pdk"][i] if rep else None
            r_ours = rep["ours"][i] if rep else None
            e = est.get(_EKEY[i])
            x = xyce.get(_EKEY[i])
            lines.append(
                f"| {metric} | {fmt(v)} | {fmt(r_pdk)} | {fmt(r_ours)} | "
                f"{fmt(e)} | {fmt(x)} |"
            )
        lines.append("")

    lines += [
        "## Notes",
        "",
        "- **Vendor .lib**: `srambank_<rows>x4mux<bits>_6t122.lib` from the",
        "  ASAP7 `asap7_sram_0p0` PDK add-on (Cadence-characterized). Its",
        "  headers declare ns/pF but the delay tables are numerically ps/fF;",
        "  interpreted as ps here (their grid contains exactly the 40 ps /",
        "  46.08 fF stimulus point). No 512/1024-word vendor libs exist.",
        "- **Report PDK / Ours**: transcribed from `MOST_report.pdf`, Table III.",
        "- **OS .lib estimate**: `--skip-characterization` output; deliberately",
        "  flat FakeRAM-style early-PPA tables (constant across slew/load).",
        "- **OS Xyce transient**: `characterize_read.py --mode table3`, full",
        "  read path through the real sense amp/latch/tristate with",
        "  replica-timed SAE; transitions measured 10%-90% (Liberty slew",
        "  conventions differ, typically 30%-70% derated).",
        "- Config caveat: the vendor parts are mux-4 banks while the OpenFinRAM",
        "  transient sweep varies bitline depth directly; capacities shown are",
        "  the 16 Kb class unless stated otherwise.",
        "",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))
    print(f"wrote {args.output}")
    return 0


_VKEY = ["cell_rise", "rise_transition", "cell_fall", "fall_transition"]
_EKEY = ["cell_rise", "rise_transition", "cell_fall", "fall_transition"]

if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))
