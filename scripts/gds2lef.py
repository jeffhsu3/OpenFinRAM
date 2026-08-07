#!/usr/bin/env python3
"""Emit a LEF abstract from an OpenFinRAM SRAM GDS — no Cadence (strmin/abstract).

OpenFinRAM's open-source flow builds a real macro GDS but still generates the LEF
via commercial tools (strmin + Cadence Abstract Generator). This reads the GDS
back with gdstk and writes the LEF directly from the pin labels + bounding box,
so we can floorplan/route the macro in OpenROAD for the pin-access congestion
trial without any commercial dependency.

Pins are gdstk *labels* (texttype 251) carrying name + point + metal layer. We
emit a small pin rect on the mapped routing layer at each label point, and a
first-order obstruction (block the lower metals the macro routes on internally).
This is a routability abstract for a congestion spike, NOT a signoff LEF.

Usage:
    uv run python scripts/gds2lef.py <macro.gds> [-o out.lef]
        [--pin-halfwidth 0.018] [--obs M1,M2] [--macro NAME]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import gdstk

# ASAP7 GDS drawing-layer number -> LEF routing-layer name
# (from OpenFinRAM tech/TechLib/asap7_TechLib.layermap; pin datatype is 251).
LAYER_TO_METAL = {
    19: "M1",
    20: "M2",
    30: "M3",
    40: "M4",
    50: "M5",
    60: "M6",
    70: "M7",
    80: "M8",
    90: "M9",
}


def infer_pin(name: str) -> tuple[str, str]:
    """Return (DIRECTION, USE) for a pin name."""
    base = name.split("[")[0].lower()
    if base in ("vdd", "vpwr", "vcc"):
        return "INOUT", "POWER"
    if base in ("vss", "vgnd", "gnd"):
        return "INOUT", "GROUND"
    if base in ("q", "dout", "do", "y"):
        return "OUTPUT", "SIGNAL"
    if base == "clk":
        return "INPUT", "CLOCK"
    # A[*] address, D[*] data-in, control (ce_n/we_n/oe_n/sdel/...) are inputs.
    return "INPUT", "SIGNAL"


def pick_top(lib: gdstk.Library, prefer: str | None) -> gdstk.Cell:
    # top_level() may include RawCell; we only handle parsed Cells (need labels/bbox)
    tops = [c for c in lib.top_level() if isinstance(c, gdstk.Cell)]
    if prefer:
        for c in tops:
            if c.name == prefer:
                return c
    named = [c for c in tops if c.name.startswith("sram")]
    if named:
        # the assembled macro is the widest sram* top cell
        return max(named, key=lambda c: _size(c)[0] * _size(c)[1])
    if len(tops) == 1:
        return tops[0]
    raise SystemExit(f"ambiguous top cell; pass --macro. tops={[c.name for c in tops]}")


def _size(cell: gdstk.Cell) -> tuple[float, float]:
    (x0, y0), (x1, y1) = cell.bounding_box()
    return (x1 - x0, y1 - y0)


def build_lef(
    gds_path: Path,
    out_path: Path,
    pin_hw: float,
    obs_layers: list[str],
    macro_name: str | None,
) -> None:
    lib = gdstk.read_gds(str(gds_path))
    top = pick_top(lib, macro_name)
    name = macro_name or top.name

    (llx, lly), (urx, ury) = top.bounding_box()
    w, h = urx - llx, ury - lly  # translate so lower-left -> (0,0)

    # One label == one pin. Group by name in case of stray duplicates.
    pins: dict[str, list[gdstk.Label]] = {}
    dropped = 0
    for lb in top.labels:
        if lb.texttype != 251 or lb.layer not in LAYER_TO_METAL:
            dropped += 1
            continue
        pins.setdefault(lb.text, []).append(lb)
    if not pins:
        raise SystemExit(
            "no pin labels (texttype 251 on a metal layer) found on top cell"
        )

    lines: list[str] = []
    lines.append("VERSION 5.8 ;")
    lines.append('BUSBITCHARS "[]" ;')
    lines.append('DIVIDERCHAR "/" ;')
    lines.append("")
    lines.append(f"MACRO {name}")
    lines.append("  CLASS BLOCK ;")
    lines.append(f"  FOREIGN {name} 0 0 ;")
    lines.append("  ORIGIN 0 0 ;")
    lines.append(f"  SIZE {w:.4f} BY {h:.4f} ;")
    lines.append("  SYMMETRY X Y R90 ;")

    for pin_name in sorted(pins, key=_natkey):
        direction, use = infer_pin(pin_name)
        lines.append(f"  PIN {pin_name}")
        lines.append(f"    DIRECTION {direction} ;")
        lines.append(f"    USE {use} ;")
        for lb in pins[pin_name]:
            metal = LAYER_TO_METAL[lb.layer]
            px, py = lb.origin[0] - llx, lb.origin[1] - lly
            x0, y0 = px - pin_hw, py - pin_hw
            x1, y1 = px + pin_hw, py + pin_hw
            # clamp inside the macro outline
            x0, y0 = max(0.0, x0), max(0.0, y0)
            x1, y1 = min(w, x1), min(h, y1)
            lines.append("    PORT")
            lines.append(f"      LAYER {metal} ;")
            lines.append(f"        RECT {x0:.4f} {y0:.4f} {x1:.4f} {y1:.4f} ;")
            lines.append("    END")
        lines.append(f"  END {pin_name}")

    if obs_layers:
        lines.append("  OBS")
        for metal in obs_layers:
            lines.append(f"    LAYER {metal} ;")
            lines.append(f"      RECT 0.0000 0.0000 {w:.4f} {h:.4f} ;")
        lines.append("  END")

    lines.append(f"END {name}")
    lines.append("")
    lines.append("END LIBRARY")
    lines.append("")

    out_path.write_text("\n".join(lines))

    metal_hist: dict[str, int] = {}
    for ps in pins.values():
        for lb in ps:
            metal_hist[LAYER_TO_METAL[lb.layer]] = (
                metal_hist.get(LAYER_TO_METAL[lb.layer], 0) + 1
            )
    print(f"MACRO {name}: SIZE {w:.3f} x {h:.3f} um")
    print(f"  pins: {len(pins)}  (pin ports by metal: {metal_hist})")
    print(f"  OBS layers: {obs_layers or '(none)'}   dropped labels: {dropped}")
    print(f"  -> {out_path}")


def _natkey(s: str):
    # A[10] after A[2]: split digits so bus bits sort numerically
    import re

    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", s)]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("gds", type=Path, help="OpenFinRAM SRAM macro GDS")
    ap.add_argument("-o", "--out", type=Path, default=None, help="output .lef")
    ap.add_argument(
        "--pin-halfwidth",
        type=float,
        default=0.018,
        help="half-size of the synthetic pin rect in um (default 0.018 ~ 1 M-track)",
    )
    ap.add_argument(
        "--obs",
        default="M1,M2",
        help="comma list of metals to blanket-obstruct (default M1,M2; '' = none)",
    )
    ap.add_argument("--macro", default=None, help="explicit top macro name")
    args = ap.parse_args(argv)

    if not args.gds.exists():
        print(f"no such GDS: {args.gds}", file=sys.stderr)
        return 2
    out = args.out or args.gds.with_suffix(".lef")
    obs = [m.strip() for m in args.obs.split(",") if m.strip()]
    build_lef(args.gds, out, args.pin_halfwidth, obs, args.macro)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
