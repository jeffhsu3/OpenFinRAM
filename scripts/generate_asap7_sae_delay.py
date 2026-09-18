#!/usr/bin/env python3
"""Parameterized SAE/replica delay cell for ASAP7, assembled from chipforge_asap7.

The cell is an inverter chain with an optional NAND2 enable on the first stage
(the replica loop's ``sdel`` function).  One `SaeDelayConfig` yields

  1. a BSIM-CMG SPICE subcircuit (Xyce, ASAP7 TT, ``nfin``-sized devices),
  2. a fin-level GDS cell on the ASAP7 7.5-track row (gdspy, nm units),
  3. a LEF abstract with the real M1 pin shapes and obstructions,
  4. a Xyce delay testbench + JSON calibration for the Tier-0 surrogate,
  5. DRC (public ASAP7 KLayout runset) and LVS (chipforge_asap7 deck) gates.

The generator is the evolvable surface: OpenEvolve/AlphaEvolve-style loops
mutate `SaeDelayConfig` (or this file) and score with `fitness`.

What comes from chipforge_asap7
-------------------------------
* ``chipforge_asap7.layout``: the ASAP7 layer map and the 27/54 nm fin/gate
  grid constants -- no local copies of either.
* ``FinFETSpec``: per-device fin arithmetic (ACTIVE span, contact rows) and
  the BSIM-CMG instance line, so ``nfin`` in SPICE and fins in GDS are the same
  number by construction.
* ``RowStack``/``RowBand``: the nFET-below / pFET-above row, its rails, its
  n/p seam and its fin grid, with ``band_height`` pinned to the released
  270 nm 7.5-track row (or the next fin-legal height for 4 fins).
* ``build_device_band``: ACTIVE + SDT + LISD on every S/D column of a tile.
* ``RowSupportSpec``/``build_row_support``: the filler and tap for this exact
  stack, so DRC runs on a properly terminated row (``filler cell filler tap
  filler``, the recipe chipforge_asap7 documents).
* ``chipforge_asap7.verification.run_lvs``: KLayout LVS against a unit-fin
  reference rendered from the same device list as the SPICE netlist.

What this script still draws itself
-----------------------------------
The tiles and the M1 routing, copied from the released NAND2xp33 / BUFx2 /
INVx1 standard cells (``asap7sc7p5t_28``):

  nand   VSS A n B Y  /  VDD A Y B VDD      first stage with EN
  pair   D G S G D    (two chain stages sharing one source column)
  inv    S G D        (a trailing odd stage)

Each tile is an island with its own edge dummy gates and a 46 nm ACTIVE
inset, so consecutive tiles keep the 92 nm S/D-to-S/D spacing (ACTIVE.S.2A)
that abutted released cells keep.  M1 uses the released vocabulary: 18 nm
"flags" on the drain-via rows, vertical bars over source columns or dummy
gates, and an 18 nm bar on the n/p seam for gate contacts.  Nothing above M1
is used.  Everything drawn here is a candidate for a chipforge_asap7 logic-cell
builder; docs/asap7_sae_delay_cell.md lists the gaps this cell exposed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal

from chipforge_asap7.devices import (
    FinFETSpec,
    RowStack,
    RowSupportSpec,
    build_device_band,
    build_row_support,
)
from chipforge_asap7.devices.finfet import (
    CONTACT_SIZE,
    DEVICE_GATE_CUT_HEIGHT,
    GATE_LIG_HEIGHT,
    GATE_V0_DX,
    LI_RAIL_HEIGHT,
    M1_MIN_SPACE,
    M1_V0_ENCLOSURE,
    M1_WIDTH,
    POLY_OVERHANG,
    SD_BAR_WIDTH,
    SELECT_X_ENC,
    VT_LAYERS,
)
from chipforge_asap7.layout import (
    FIN_WIDTH,
    GATE_PITCH,
    GATE_WIDTH,
    LAYERS,
    STD_CELL_HEIGHT,
    box,
    require_gdspy,
)
from chipforge_asap7.verification import find_klayout, run_lvs

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

VDD = 0.7
CAL_PATH = REPO_ROOT / "results" / "sae_delay_cal.json"
XYCE_BIN = Path("/home/jeff/iv4/local/xyce-14.4/bin/Xyce")
TT_MODELS = REPO_ROOT / "tech" / "models" / "hspice" / "7nm_TT.pm"
DRC_DECK_ENV = "ASAP7_DRC_DECK"
DRC_DECK_DEFAULT = Path.home() / "iv4/repos/ASAP7_for_KLayout/drc/drc_ASAP7.lydrc"

# ── Geometry vocabulary of the released 7.5-track cells (nm) ─────────────────
HALF_CT = CONTACT_SIZE // 2  # 9
HALF_M1 = M1_WIDTH // 2  # 9
HALF_SD = SD_BAR_WIDTH // 2  # 12
HALF_LIG = GATE_LIG_HEIGHT // 2  # 11: gate-contact LIG straddling the seam
HALF_CUT = DEVICE_GATE_CUT_HEIGHT // 2  # 22
HALF_RAIL = LI_RAIL_HEIGHT // 2  # 8
M1_PAD = HALF_CT + M1_V0_ENCLOSURE  # 14: M1 past a V0 along its own track
#: Pin-access flags sit one M1 track inside the drain-via rows (BUFx2's A pin).
PIN_ROW_OFFSET = M1_WIDTH + M1_MIN_SPACE  # 36
#: LIG.GATE.EX.1 minimum.  The released cells draw exactly 1 nm, which is what
#: keeps two neighbouring 22 nm gate straps at the 31 nm LIG.S.4-5 spacing.
LIG_GATE_EXT = 1
#: Tile widths in gate tracks: NAND2xp33 and BUFx2-minus-a-finger are 4 CPP,
#: INVx1 is 3.  Each tile owns both of its edge dummy gates.
TILE_TRACKS = {"nand": 4, "pair": 4, "inv": 3}

_M1_PIN = LAYERS["M1_PIN"]

Box = tuple[float, float, float, float]


def _assert(cond: bool, msg: str) -> None:
    if not cond:
        raise ValueError(f"sae_delay topology violation: {msg}")


# ── Configuration ────────────────────────────────────────────────────────────
@dataclass(frozen=True)
class Device:
    """One transistor of the chain: the single source of SPICE, LVS and GDS."""

    name: str
    flavor: Literal["n", "p"]
    drain: str
    gate: str
    source: str
    bulk: str
    stage: int


@dataclass(frozen=True)
class Tile:
    """One diffusion island of the cell, on the 54 nm track grid."""

    kind: Literal["nand", "pair", "inv"]
    x0: int
    stages: tuple[int, ...]

    @property
    def tracks(self) -> int:
        return TILE_TRACKS[self.kind]

    @property
    def width(self) -> int:
        return self.tracks * GATE_PITCH

    @property
    def x1(self) -> int:
        return self.x0 + self.width

    @property
    def gate_xs(self) -> tuple[int, ...]:
        """Center X of the active gates, left to right (cell coordinates)."""
        count = 2 if self.kind in ("nand", "pair") else 1
        return tuple(self.x0 + GATE_PITCH // 2 + (1 + i) * GATE_PITCH for i in range(count))

    @property
    def dummy_xs(self) -> tuple[int, int]:
        return (self.x0 + GATE_PITCH // 2, self.x1 - GATE_PITCH // 2)


@dataclass(frozen=True)
class SaeDelayConfig:
    """Evolvable parameters of the custom delay cell."""

    stages: int = 6  # inverting stages including the NAND (even: non-inverting)
    nfin_n: int = 2  # NMOS fins per device
    nfin_p: int = 2  # PMOS fins per device
    nand_enable: bool = True  # first stage is a NAND2 with EN (replica gate)
    load_fF: float = 8.0  # characterization load (sense-enable fanout)
    vt: Literal["rvt", "lvt", "slvt", "sram"] = "sram"

    def __post_init__(self) -> None:
        if not 2 <= self.stages <= 12:
            raise ValueError("stages must be in [2, 12]")
        if self.stages % 2 != 0:
            raise ValueError("stages must be even (SAE needs non-inverting delay)")
        for name in ("nfin_n", "nfin_p"):
            v = getattr(self, name)
            if not 1 <= v <= 4:
                raise ValueError(f"{name} must be in [1, 4] fins")
        if not 0.5 <= self.load_fF <= 64.0:
            raise ValueError("load_fF must be in [0.5, 64]")
        if self.vt not in VT_LAYERS:
            raise ValueError(f"vt must be one of {tuple(VT_LAYERS)}")
        self.stack  # FinFETSpec validates the fin/height grid

    # ── Naming ────────────────────────────────────────────────────────────────
    @property
    def cell_name(self) -> str:
        en = "_en" if self.nand_enable else ""
        return f"sae_delay_{self.stages}s_{self.nfin_n}n{self.nfin_p}p{en}_{self.vt}"

    # ── Row ───────────────────────────────────────────────────────────────────
    @property
    def band_height(self) -> int:
        """Half the 7.5-track row, or the next fin-legal height (4 fins)."""
        tallest = FinFETSpec(fins=max(self.nfin_n, self.nfin_p)).default_height_per_row
        return max(STD_CELL_HEIGHT // 2, tallest)

    @property
    def stack(self) -> RowStack:
        """The row, pinned to `band_height` so the cell, its filler and its tap share rails."""
        return RowStack(
            rows=((self.nfin_n, self.nfin_p),), vt=self.vt, band_height=self.band_height
        )

    @property
    def height(self) -> int:
        return self.stack.height

    # ── Floorplan ─────────────────────────────────────────────────────────────
    @property
    def tiles(self) -> tuple[Tile, ...]:
        tiles: list[Tile] = []
        x = 0
        stage = 0
        if self.nand_enable:
            tiles.append(Tile("nand", x, (0,)))
            x += tiles[-1].width
            stage = 1
        while self.stages - stage >= 2:
            tiles.append(Tile("pair", x, (stage, stage + 1)))
            x += tiles[-1].width
            stage += 2
        if self.stages - stage == 1:
            tiles.append(Tile("inv", x, (stage,)))
        return tuple(tiles)

    @property
    def width_cpp(self) -> int:
        return sum(t.tracks for t in self.tiles)

    @property
    def width(self) -> int:
        return self.width_cpp * GATE_PITCH

    @property
    def gate_track_xs(self) -> list[int]:
        return [GATE_PITCH // 2 + t * GATE_PITCH for t in range(self.width_cpp)]

    @property
    def dummy_xs(self) -> list[int]:
        return sorted(x for t in self.tiles for x in t.dummy_xs)

    @property
    def stage_gate_xs(self) -> dict[int, list[int]]:
        """Active gate X per stage.  A NAND first stage has two (IN and EN)."""
        out: dict[int, list[int]] = {}
        for t in self.tiles:
            if t.kind == "nand":
                out[0] = list(t.gate_xs)
            else:
                for s, gx in zip(t.stages, t.gate_xs):
                    out[s] = [gx]
        return out

    @property
    def pins(self) -> tuple[str, ...]:
        return ("IN", "OUT") + (("EN",) if self.nand_enable else ()) + ("VDD", "VSS")

    @property
    def pin_positions(self) -> dict[str, tuple[float, float]]:
        """One point inside each pin's M1, in cell-local nanometres."""
        seam = self.stack.seam_y(0)
        first, last = self.tiles[0], self.tiles[-1]
        pos = {
            "IN": (first.x0 + (135 if first.kind == "nand" else 55), seam),
            "OUT": (last.x0 + (117 if last.kind == "inv" else 189), seam),
            "VDD": (self.width / 2, self.height),
            "VSS": (self.width / 2, 0),
        }
        if self.nand_enable:
            pos["EN"] = (first.x0 + 57, seam)
        return pos

    # ── Netlist ───────────────────────────────────────────────────────────────
    @property
    def devices(self) -> tuple[Device, ...]:
        devs: list[Device] = []
        node, start = "IN", 0
        if self.nand_enable:
            devs += [
                Device("N0a", "n", "n_0", "IN", "net_n0", "VSS", 0),
                Device("N0b", "n", "net_n0", "EN", "VSS", "VSS", 0),
                Device("P0a", "p", "n_0", "IN", "VDD", "VDD", 0),
                Device("P0b", "p", "n_0", "EN", "VDD", "VDD", 0),
            ]
            node, start = "n_0", 1
        for i in range(start, self.stages):
            out = "OUT" if i == self.stages - 1 else f"n_{i}"
            devs.append(Device(f"N{i}", "n", out, node, "VSS", "VSS", i))
            devs.append(Device(f"P{i}", "p", out, node, "VDD", "VDD", i))
            node = out
        return tuple(devs)

    def device_spec(self, flavor: Literal["n", "p"]) -> FinFETSpec:
        return self.stack.band_spec(flavor, self.nfin_n if flavor == "n" else self.nfin_p)


def netlist(cfg: SaeDelayConfig) -> str:
    """BSIM-CMG subcircuit, one ``nfin``-sized instance per device (Xyce dialect).

    `FinFETSpec.netlist` writes the HSPICE form with ``nf=`` and ``m=``; Xyce's
    BSIM-CMG (level 107) rejects both tokens, so they are dropped here.  Every
    device is single-finger, single-multiplier anyway.
    """
    lines = [f".subckt {cfg.cell_name} {' '.join(cfg.pins[:2])}"
             + (" VDD VSS EN" if cfg.nand_enable else " VDD VSS")]
    for d in cfg.devices:
        spec = cfg.device_spec(d.flavor)
        line = spec.netlist(f"M_{d.name}", (d.drain, d.gate, d.source, d.bulk))
        lines.append(re.sub(r"\s+(?:nf|m)=\d+", "", line))
    lines.append(f".ends {cfg.cell_name}")
    return "\n".join(lines) + "\n"


#: Nets that exist only as uncontacted diffusion between two series gates.  The
#: released NAND2 draws no SDT/LISD there and neither does the `nand` tile, so
#: the unit-fin extractor sees one such net per fin; the reference says so too.
_UNCONTACTED_NETS = {"net_n0"}


def lvs_schematic(cfg: SaeDelayConfig) -> str:
    """Unit-fin LVS reference: one MOS per FIN x GATE channel, as the deck extracts."""
    lines = [
        "* ASAP7 SAE delay cell LVS reference: one MOS per FIN x GATE channel",
        f".SUBCKT {cfg.cell_name} {' '.join(cfg.pins)}",
    ]
    for d in cfg.devices:
        spec = cfg.device_spec(d.flavor)
        for fin in range(spec.fins):
            nets = [f"{n}_f{fin}" if n in _UNCONTACTED_NETS else n
                    for n in (d.drain, d.gate, d.source, d.bulk)]
            lines.append(
                f"M{d.name}_n{fin} {' '.join(nets)} "
                f"{spec.model} L={spec.gate_length}n W={FIN_WIDTH}n"
            )
    lines += [f".ENDS {cfg.cell_name}", ".END", ""]
    return "\n".join(lines)


# ── Layout ───────────────────────────────────────────────────────────────────
class _Canvas:
    """Row geometry from the stack plus the released cells' M1 vocabulary."""

    def __init__(self, cfg: SaeDelayConfig, cell: Any) -> None:
        self.cfg, self.cell = cfg, cell
        stack = cfg.stack
        self.n, self.p = stack.bands()
        self.seam = stack.seam_y(0)
        self.y_n = int(self.n.contact_y)  # nFET drain-via row
        self.y_p = int(self.p.contact_y)  # pFET drain-via row
        self.y_pin_n = self.y_n + PIN_ROW_OFFSET
        self.y_pin_p = self.y_p - PIN_ROW_OFFSET
        self.bar_lo = self.y_n + HALF_CT  # vertical bars join the two flag rows
        self.bar_hi = self.y_p - HALF_CT
        self.pins: dict[str, list[Box]] = {}
        self.obstructions: list[Box] = []

    def rect(self, layer: str, x0: float, y0: float, x1: float, y1: float) -> None:
        box(self.cell, layer, x0, y0, x1, y1)

    def m1(self, x0: float, y0: float, x1: float, y1: float, pin: str | None = None) -> None:
        self.rect("M1", x0, y0, x1, y1)
        shape = (min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1))
        if pin is None:
            self.obstructions.append(shape)
        else:
            self.pins.setdefault(pin, []).append(shape)

    def flag(self, x0: float, x1: float, y: float, pin: str | None = None) -> None:
        """An 18 nm horizontal M1 bar on a via row (V0.M1.AUX.3 wants exactly 18)."""
        self.m1(x0, y - HALF_M1, x1, y + HALF_M1, pin)

    def vbar(self, x0: float, x1: float, y0: float, y1: float, pin: str | None = None) -> None:
        self.m1(x0, y0, x1, y1, pin)

    def seam_bar(self, x0: float, x1: float, pin: str | None = None) -> None:
        self.m1(x0, self.seam - HALF_M1, x1, self.seam + HALF_M1, pin)

    def v0(self, x: float, y: float) -> None:
        self.rect("V0", x - HALF_CT, y - HALF_CT, x + HALF_CT, y + HALF_CT)

    def gate_contact(self, gate_x: int, *, from_column: int | None = None) -> None:
        """LIG strap on the seam plus its V0.

        With `from_column`, the released 'A' style: LIG runs from the S/D
        column to 1 nm past the gate, V0 sits `GATE_V0_DX` past the column.
        Without, the 'B'/'G2' style: a 22 nm pad centred on the gate.
        """
        if from_column is None:
            self.rect("LIG", gate_x - HALF_LIG, self.seam - HALF_LIG,
                      gate_x + HALF_LIG, self.seam + HALF_LIG)
            self.v0(gate_x, self.seam)
        else:
            self.rect("LIG", from_column, self.seam - HALF_LIG,
                      gate_x + GATE_WIDTH // 2 + LIG_GATE_EXT, self.seam + HALF_LIG)
            self.v0(from_column + GATE_V0_DX, self.seam)

    def diffusion(self, tile: Tile, *, n_cols, p_cols, n_sources, p_sources,
                  n_drains, p_drains) -> None:
        """ACTIVE/SDT/LISD per band via chipforge_asap7, plus rail ties and drain vias."""
        active_x = (tile.x0 + SELECT_X_ENC, tile.x1 - SELECT_X_ENC)
        for band, cols, sources, drains in (
            (self.n, n_cols, n_sources, n_drains),
            (self.p, p_cols, p_sources, p_drains),
        ):
            build_device_band(
                self.cell, band.spec, y0=band.y0,
                sd_xs=[tile.x0 + c for c in cols], active_x=active_x,
            )
            act_lo, act_hi = band.active_span
            for c in sources:
                x = tile.x0 + c
                lo, hi = (band.rail_y, act_hi) if band.rail_below else (act_lo, band.rail_y)
                self.rect("LISD", x - HALF_SD, lo, x + HALF_SD, hi)
                self.v0(x, band.rail_y)
            for c in drains:
                self.v0(tile.x0 + c, band.contact_y)


def _draw_nand(cv: _Canvas, tile: Tile) -> None:
    """NAND2xp33: N series ``VSS A n B Y``, P parallel ``VDD A Y B VDD``.

    The released cell's A gate (VSS side of the stack) carries EN and its B
    gate (Y side) carries IN: the timing-critical input drives the device
    nearest the output, and the series order matches `SaeDelayConfig.devices`
    (LVS compares structure, so the order is not a free choice).
    """
    x = tile.x0
    cv.diffusion(tile, n_cols=(54, 162), p_cols=(54, 108, 162),
                 n_sources=(54,), p_sources=(54, 162), n_drains=(162,), p_drains=(108,))
    # A = EN: seam bar, vertical bar, pin flags on the (free) drain rows.
    cv.gate_contact(x + 81, from_column=x + 54)
    cv.seam_bar(x + 36, x + 78, "EN")
    cv.vbar(x + 36, x + 55, cv.bar_lo, cv.bar_hi, "EN")
    cv.flag(x + 18, x + 55, cv.y_n, "EN")
    cv.flag(x + 18, x + 55, cv.y_p, "EN")
    # B = IN: pad on the gate, vertical bar, pin flags one track inside.
    cv.gate_contact(x + 135)
    cv.vbar(x + 126, x + 144, cv.y_pin_n + HALF_M1, cv.y_pin_p - HALF_M1, "IN")
    cv.flag(x + 107, x + 144, cv.y_pin_n, "IN")
    cv.flag(x + 107, x + 144, cv.y_pin_p, "IN")
    # Y: P drain (108) on the top flag, N drain (162) on the bottom flag, joined
    # by a bar over the right dummy, handed on along the seam.
    cv.flag(x + 94, x + 198, cv.y_p)
    cv.flag(x + 143, x + 198, cv.y_n)
    cv.vbar(x + 180, x + 198, cv.bar_lo, cv.bar_hi)
    cv.seam_bar(x + 180, tile.x1)


def _draw_pair(cv: _Canvas, tile: Tile, *, first: bool, last: bool) -> None:
    """BUFx2 minus inv2's second finger: ``D Ga S Gb D`` in both bands."""
    x = tile.x0
    cv.diffusion(tile, n_cols=(54, 108, 162), p_cols=(54, 108, 162),
                 n_sources=(108,), p_sources=(108,), n_drains=(54, 162), p_drains=(54, 162))
    cv.gate_contact(x + 81, from_column=x + 54)
    if first:  # BUFx2's A pin: flags sit one track inside the drain rows
        cv.seam_bar(x + 36, x + 73, "IN")
        cv.vbar(x + 36, x + 55, cv.y_pin_n + HALF_M1, cv.y_pin_p - HALF_M1, "IN")
        cv.flag(x + 18, x + 55, cv.y_pin_n, "IN")
        cv.flag(x + 18, x + 55, cv.y_pin_p, "IN")
    else:  # continues the previous tile's seam bar
        cv.seam_bar(x, x + 73)
    # Internal node: D_a flags, bar over the shared source column, seam to G_b.
    cv.flag(x + 40, x + 120, cv.y_n)
    cv.flag(x + 40, x + 120, cv.y_p)
    cv.vbar(x + 102, x + 120, cv.bar_lo, cv.bar_hi)
    cv.seam_bar(x + 120, x + 152)
    cv.gate_contact(x + 135)
    # Output D_b: flags plus a bar over the right dummy gate.
    pin = "OUT" if last else None
    cv.flag(x + 145, x + 198, cv.y_n, pin)
    cv.flag(x + 145, x + 198, cv.y_p, pin)
    cv.vbar(x + 180, x + 198, cv.bar_lo, cv.bar_hi, pin)
    if not last:
        cv.seam_bar(x + 180, tile.x1)


def _draw_inv(cv: _Canvas, tile: Tile) -> None:
    """INVx1 with the input arriving along the seam: ``S G D``."""
    x = tile.x0
    cv.diffusion(tile, n_cols=(54, 108), p_cols=(54, 108),
                 n_sources=(54,), p_sources=(54,), n_drains=(108,), p_drains=(108,))
    cv.gate_contact(x + 81, from_column=x + 54)
    cv.seam_bar(x, x + 78)
    cv.flag(x + 94, x + 144, cv.y_n, "OUT")
    cv.flag(x + 94, x + 144, cv.y_p, "OUT")
    cv.vbar(x + 108, x + 126, cv.bar_lo, cv.bar_hi, "OUT")


def _merge_spans(spans: list[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []
    for x0, x1 in sorted(spans):
        if merged and x0 <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], x1))
        else:
            merged.append((x0, x1))
    return merged


def _build(cfg: SaeDelayConfig, *, lib: Any = None, name: str | None = None,
           draw_pin_labels: bool = True) -> tuple[Any, _Canvas]:
    gdspy = require_gdspy()
    cell_name = name or cfg.cell_name
    cell = lib.new_cell(cell_name) if lib is not None else gdspy.Cell(cell_name, exclude_from_current=True)
    cv = _Canvas(cfg, cell)
    stack = cfg.stack
    width, height = cfg.width, cfg.height

    # Implant tiles the row band by band; the p band and its well coincide.
    for band in (cv.n, cv.p):
        cv.rect(band.implant, 0, band.y0, width, band.y1)
        if band.in_nwell:
            cv.rect("NWELL", 0, band.y0, width, band.y1)
    if vt_layer := VT_LAYERS[cfg.vt]:
        cv.rect(vt_layer, 0, 0, width, height)

    # FIN and GATE are manufacturing grids across the whole cell.
    for y_fin in stack.fin_grid_ys:
        cv.rect("FIN", 0, y_fin, width, y_fin + FIN_WIDTH)
    for x_gate in cfg.gate_track_xs:
        cv.rect("GATE", x_gate - GATE_WIDTH // 2, -POLY_OVERHANG,
                x_gate + GATE_WIDTH // 2, height + POLY_OVERHANG)
    # Poly is cut on both rails, and dummy tracks are also cut on the seam
    # (the released cells' split GCUT), 17 nm clear of the active gates.
    for y_rail, _ in stack.rails:
        cv.rect("GATE_CUT", 0, y_rail - HALF_CUT, width, y_rail + HALF_CUT)
    for x0, x1 in _merge_spans([(x - GATE_PITCH // 2, x + GATE_PITCH // 2) for x in cfg.dummy_xs]):
        cv.rect("GATE_CUT", x0, cv.seam - HALF_CUT, x1, cv.seam + HALF_CUT)

    # Rails: 16 nm LI under 18 nm M1, centred on the row boundary.
    for y_rail, net in stack.rails:
        cv.rect("LIG", 0, y_rail - HALF_RAIL, width, y_rail + HALF_RAIL)
        cv.m1(0, y_rail - HALF_M1, width, y_rail + HALF_M1, net)

    tiles = cfg.tiles
    for index, tile in enumerate(tiles):
        if tile.kind == "nand":
            _draw_nand(cv, tile)
        elif tile.kind == "pair":
            _draw_pair(cv, tile, first=index == 0, last=index == len(tiles) - 1)
        else:
            _draw_inv(cv, tile)

    if draw_pin_labels:
        for pin, origin in cfg.pin_positions.items():
            cell.add(gdspy.Label(pin, origin, layer=_M1_PIN["layer"], texttype=_M1_PIN["datatype"]))
    cv.rect("BOUNDARY", 0, 0, width, height)
    return cell, cv


def layout(cfg: SaeDelayConfig, *, lib: Any = None, name: str | None = None,
           draw_pin_labels: bool = True) -> Any:
    """Draw the cell and return the gdspy Cell (nanometre coordinates)."""
    return _build(cfg, lib=lib, name=name, draw_pin_labels=draw_pin_labels)[0]


def pin_shapes(cfg: SaeDelayConfig) -> tuple[dict[str, list[Box]], list[Box]]:
    """M1 rectangles per pin, and the internal M1 obstructions, in nm."""
    _, cv = _build(cfg, draw_pin_labels=False)
    return cv.pins, cv.obstructions


def new_library() -> Any:
    """A gdspy library in the nanometre units every chipforge_asap7 cell uses.

    It is also made the process-global ``current_library``: gdspy registers
    every ``lib.new_cell`` there as well, so two libraries built in one process
    would otherwise collide on the (deterministic) cell names.
    """
    gdspy = require_gdspy()
    lib = gdspy.GdsLibrary(unit=1e-9, precision=1e-10)
    gdspy.current_library = lib
    return lib


def terminated_row(cfg: SaeDelayConfig, lib: Any, *, name: str | None = None) -> Any:
    """``filler cell filler tap filler`` -- the arrangement a placed row gives the cell.

    A logic cell cannot satisfy latch-up (needs a tap within 30 um) or the
    46 nm implant enclosure at its two outer edges on its own; the filler and
    tap built from the very same `SaeRowStack` supply both.
    """
    gdspy = require_gdspy()
    cell = layout(cfg, lib=lib)
    support = {
        kind: build_row_support(RowSupportSpec(stack=cfg.stack, kind=kind), lib=lib)
        for kind in ("filler", "tap")
    }
    top = lib.new_cell(name or f"{cfg.cell_name}_row")
    cursor = 0
    for item in ("filler", "cell", "filler", "tap", "filler"):
        ref = cell if item == "cell" else support[item]
        top.add(gdspy.CellReference(ref, origin=(cursor, 0)))
        cursor += cfg.width if item == "cell" else ref.get_bounding_box()[1][0]
    return top


# ── Self-verification ────────────────────────────────────────────────────────
def layer_boxes(cell: Any, layer_name: str) -> list[Box]:
    """Bounding boxes of every polygon on `layer_name` (gdspy cell), sorted."""
    target = (LAYERS[layer_name]["layer"], LAYERS[layer_name]["datatype"])
    result: list[Box] = []
    for polygon_set in cell.polygons:
        for points, gds_layer, datatype in zip(
            polygon_set.polygons, polygon_set.layers, polygon_set.datatypes
        ):
            if (gds_layer, datatype) == target:
                xs, ys = points[:, 0], points[:, 1]
                result.append((float(xs.min()), float(ys.min()), float(xs.max()), float(ys.max())))
    return sorted(result)


def verify_topology(cell: Any, cfg: SaeDelayConfig) -> dict[int, dict[str, int]]:
    """Extracts FIN/GATE/ACTIVE geometry and asserts it matches the config.

    Every stage's gate must cross exactly one ACTIVE island per band, and that
    island must span exactly the configured fin count.  Returns the measured
    per-stage fin counts.
    """
    gates = layer_boxes(cell, "GATE")
    _assert(len(gates) == cfg.width_cpp,
            f"expected {cfg.width_cpp} gate tracks, extracted {len(gates)}")
    centers = sorted((b[0] + b[2]) / 2 for b in gates)
    for a, b in zip(centers, centers[1:]):
        _assert(abs((b - a) - GATE_PITCH) < 1e-6, "gate tracks off the 54 nm pitch")
    fins = layer_boxes(cell, "FIN")
    for f in fins:
        _assert(abs((f[3] - f[1]) - FIN_WIDTH) < 1e-6, "FIN stripe width is not 7 nm")
    actives = layer_boxes(cell, "ACTIVE")
    n_band, p_band = cfg.stack.bands()
    measured: dict[int, dict[str, int]] = {}
    for stage, gxs in cfg.stage_gate_xs.items():
        counts: dict[str, int] = {}
        for band, want, tag in ((n_band, cfg.nfin_n, "n"), (p_band, cfg.nfin_p, "p")):
            for gx in gxs:
                islands = [a for a in actives
                           if a[0] < gx < a[2] and band.y0 <= a[1] and a[3] <= band.y1]
                _assert(len(islands) == 1,
                        f"stage {stage} {tag}: gate at x={gx} crosses {len(islands)} ACTIVE islands")
                a = islands[0]
                n = sum(1 for f in fins if f[1] >= a[1] and f[3] <= a[3] and f[0] <= gx <= f[2])
                _assert(n == want, f"stage {stage} {tag}: extracted {n} fins, expected {want}")
                counts[tag] = n
        measured[stage] = counts
    return measured


# ── LEF ──────────────────────────────────────────────────────────────────────
def _um(v: float) -> str:
    return f"{v / 1000:.3f}"


def lef_abstract(cfg: SaeDelayConfig) -> str:
    """LEF abstract: SIZE, real M1 pin ports and M1 obstructions."""
    pins, obstructions = pin_shapes(cfg)
    directions = {"IN": "INPUT", "EN": "INPUT", "OUT": "OUTPUT", "VDD": "INOUT", "VSS": "INOUT"}
    uses = {"VDD": "POWER", "VSS": "GROUND"}
    out = [
        "VERSION 5.8 ;",
        'BUSBITCHARS "[]" ;',
        'DIVIDERCHAR "/" ;',
        f"MACRO {cfg.cell_name}",
        "  CLASS CORE ;",
        "  ORIGIN 0 0 ;",
        f"  FOREIGN {cfg.cell_name} 0 0 ;",
        f"  SIZE {_um(cfg.width)} BY {_um(cfg.height)} ;",
        "  SYMMETRY X Y ;",
    ]
    if cfg.height == STD_CELL_HEIGHT:
        out.append("  SITE asap7sc7p5t ;")
    for pin in cfg.pins:
        out += [f"  PIN {pin}", f"    DIRECTION {directions[pin]} ;",
                f"    USE {uses.get(pin, 'SIGNAL')} ;", "    PORT", "      LAYER M1 ;"]
        out += [f"        RECT {_um(x0)} {_um(y0)} {_um(x1)} {_um(y1)} ;"
                for x0, y0, x1, y1 in pins[pin]]
        out += ["    END", f"  END {pin}"]
    out += ["  OBS", "    LAYER M1 ;"]
    out += [f"      RECT {_um(x0)} {_um(y0)} {_um(x1)} {_um(y1)} ;"
            for x0, y0, x1, y1 in obstructions]
    out += ["  END", f"END {cfg.cell_name}", "", "END LIBRARY"]
    return "\n".join(out) + "\n"


# ── Physical verification gates ──────────────────────────────────────────────
def find_drc_deck() -> Path | None:
    """The public ASAP7 KLayout runset (``ASAP7_DRC_DECK`` overrides)."""
    override = os.environ.get(DRC_DECK_ENV)
    for candidate in (Path(override) if override else None, DRC_DECK_DEFAULT):
        if candidate is not None and candidate.is_file():
            return candidate
    return None


def drc(cfg: SaeDelayConfig, workdir: Path, *, klayout: Path | None = None,
        deck: Path | None = None, terminate: bool = True) -> list[str]:
    """Runs the public runset; returns the violation categories (empty = clean).

    With `terminate` the cell is checked as ``filler cell filler tap filler``;
    alone, a logic cell always reports the missing tap (ACTIVE.LUP.1) and the
    outer-edge implant enclosure, which are properties of the placed row.
    """
    deck = deck or find_drc_deck()
    _assert(deck is not None, f"ASAP7 DRC deck not found (set {DRC_DECK_ENV})")
    workdir = Path(workdir).resolve()  # the runset resolves -rd paths on its own
    workdir.mkdir(parents=True, exist_ok=True)
    lib = new_library()
    top = terminated_row(cfg, lib) if terminate else layout(cfg, lib=lib)
    gds = workdir / f"{top.name}.gds"
    report = workdir / f"{top.name}.lyrdb"
    lib.write_gds(str(gds))
    cmd = [str(find_klayout(klayout)), "-b", "-r", str(deck), "-rd", f"input={gds}",
           "-rd", f"topcell={top.name}", "-rd", f"output={report}"]
    proc = subprocess.run(cmd, check=False, capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        raise RuntimeError(f"KLayout DRC failed: {proc.stdout[-2000:]}{proc.stderr[-2000:]}")
    items = ET.parse(report).getroot().findall("./items/item")
    return sorted((item.findtext("category") or "").strip("'\"") for item in items)


def lvs(cfg: SaeDelayConfig, workdir: Path, *, klayout: Path | None = None) -> Any:
    """KLayout LVS of the drawn cell against its unit-fin reference.

    The cell has no body tap (the row's tap cell supplies it), so bodies are
    tied by declaration exactly as for a released logic cell.
    """
    workdir = Path(workdir).resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    lib = new_library()
    layout(cfg, lib=lib)
    gds = workdir / f"{cfg.cell_name}.gds"
    lib.write_gds(str(gds))
    reference = workdir / f"{cfg.cell_name}_lvs_ref.sp"
    reference.write_text(lvs_schematic(cfg))
    return run_lvs(gds, reference, workdir / "lvs", cell_name=cfg.cell_name,
                   klayout=klayout, tie_bodies=True)


# ── Simulation ───────────────────────────────────────────────────────────────
def testbench(cfg: SaeDelayConfig, models_xyce: Path, workdir: Path) -> Path:
    """Xyce deck: pulse IN, measure IN->OUT delay and swing."""
    cload = cfg.load_fF * 1e-15
    en_src = "VEN EN 0 PULSE(0 0.7 0 5p 5p 490p 1000p)\n" if cfg.nand_enable else ""
    en_pin = " EN" if cfg.nand_enable else ""
    deck = f"""* {cfg.cell_name} characterization: stages={cfg.stages} nfin={cfg.nfin_n}/{cfg.nfin_p} nand={cfg.nand_enable}
.include {models_xyce}
{netlist(cfg)}
VVDD VDD 0 {VDD}
VIN IN 0 PULSE(0 {VDD} 100p 10p 10p 400p 1000p)
{en_src}CLOAD OUT 0 {cload:.4e}
Xdut IN OUT VDD 0{en_pin} {cfg.cell_name}
.tran 2p 1.2n
.measure tran t_pd TRIG v(IN) VAL={VDD / 2} RISE=1 TARG v(OUT) VAL={VDD / 2} RISE=1
.measure tran v_max MAX v(OUT)
.measure tran v_min MIN v(OUT) FROM=200p
.end
"""
    path = workdir / f"sae_d{cfg.stages}_n{cfg.nfin_n}p{cfg.nfin_p}.cir"
    path.write_text(deck)
    return path


def _xyce_models(workdir: Path) -> Path:
    """TT model card translated for Xyce (level 72 -> 107)."""
    _assert(TT_MODELS.exists(), f"missing ASAP7 TT model: {TT_MODELS}")
    dst = workdir / "asap7_tt_xyce.pm"
    dst.write_text(re.sub(r"level\s*=\s*72", "level = 107", TT_MODELS.read_text()))
    return dst


def simulate(cfg: SaeDelayConfig, xyce: Path = XYCE_BIN, workdir: Path | None = None) -> dict:
    """Runs the Xyce testbench. Returns measured delay/swing."""
    _assert(bool(shutil.which(str(xyce)) or xyce.exists()), f"Xyce not found: {xyce}")
    tmp = Path(tempfile.mkdtemp(prefix="sae_delay_")) if workdir is None else workdir
    models = _xyce_models(tmp)
    deck = testbench(cfg, models, tmp)
    proc = subprocess.run([str(xyce), str(deck)], cwd=tmp, capture_output=True, text=True, timeout=120)
    mt0 = tmp / (deck.stem + ".cir.mt0")
    if proc.returncode != 0 or not mt0.exists():
        raise RuntimeError(f"Xyce failed (rc={proc.returncode}): {proc.stdout[-500:]} {proc.stderr[-500:]}")
    vals: dict[str, float] = {}
    for line in mt0.read_text().splitlines():
        parts = [p for p in line.split() if p != "="]
        if len(parts) == 2 and parts[0] in ("T_PD", "V_MAX", "V_MIN"):
            try:
                vals[parts[0].lower()] = float(parts[1])
            except ValueError:
                pass
    if "t_pd" not in vals:
        raise RuntimeError(f"no t_pd measurement in {mt0}")
    return {
        "delay_ps": vals["t_pd"] * 1e12,
        "v_max": vals.get("v_max", float("nan")),
        "v_min": vals.get("v_min", float("nan")),
        "functional": bool(vals.get("v_max", 0) > 0.65 * VDD and vals.get("v_min", 1) < 0.1),
    }


def calibrate(grid_stages=(2, 4, 6, 8), grid_nfin=(1, 2, 4), load_fF=(2.0, 8.0)) -> dict:
    """Measures a delay grid in Xyce and fits delay = a + b*stages/nfin + c*load/nfin.

    The load/drive interaction term is load-bearing: the last stage slews the
    sense-enable fanout, so load sensitivity scales with 1/drive (rmse 5.5 ps
    vs 17.4 ps without it on the 24-point TT grid)."""
    import numpy as np
    rows = []
    for s in grid_stages:
        for n in grid_nfin:
            for ld in load_fF:
                cfg = SaeDelayConfig(stages=s, nfin_n=n, nfin_p=n, load_fF=ld)
                m = simulate(cfg)
                assert m["functional"], f"non-functional at {cfg}"
                rows.append((s, n, ld, m["delay_ps"]))
                print(f"  stages={s} nfin={n} load={ld}fF -> {m['delay_ps']:.1f} ps", flush=True)
    A = np.array([[1.0, s / n, ld / n] for s, n, ld, _ in rows])
    y = np.array([d for _, _, _, d in rows])
    coef, *_ = np.linalg.lstsq(A, y, rcond=None)
    pred = A @ coef
    rmse = float(np.sqrt(((pred - y) ** 2).mean()))
    cal = {"model": {"a": float(coef[0]), "b": float(coef[1]), "c": float(coef[2])},
           "rmse_ps": rmse, "points": [{"stages": s, "nfin": n, "load_fF": ld, "delay_ps": d}
                                       for s, n, ld, d in rows]}
    CAL_PATH.parent.mkdir(parents=True, exist_ok=True)
    CAL_PATH.write_text(json.dumps(cal, indent=1))
    print(f"calibration rmse={rmse:.2f} ps -> {CAL_PATH}")
    return cal


def load_calibration(path: Path = CAL_PATH) -> dict | None:
    try:
        return json.loads(path.read_text())["model"]
    except (OSError, ValueError, KeyError):
        return None


def fitness(cfg: SaeDelayConfig, target_delay_ps: float, w_energy: float = 0.02,
            *, gates: bool = False, workdir: Path | None = None) -> dict:
    """Evolvable fitness: delay-target error + energy, with hard gates.

    Returns {'fitness' (lower is better), 'delay_ps', 'energy_fJ', 'functional',
    and with `gates` also 'drc' (violation categories) and 'lvs' (matched)}.
    Energy is first-order: switched gate charge over stages x fins.  Every
    failed hard gate (function, DRC, LVS) adds 10.
    """
    m = simulate(cfg)
    energy_fJ = cfg.stages * (cfg.nfin_n + cfg.nfin_p) * 0.045 * VDD**2
    err = abs(m["delay_ps"] - target_delay_ps) / target_delay_ps
    fit = err + w_energy * energy_fJ / 10.0
    if not m["functional"]:
        fit += 10.0
    result = {"fitness": fit, "delay_ps": m["delay_ps"], "energy_fJ": energy_fJ,
              "functional": m["functional"]}
    if gates:
        work = Path(tempfile.mkdtemp(prefix="sae_gates_")) if workdir is None else workdir
        violations = drc(cfg, work / "drc")
        matched = lvs(cfg, work / "lvs").matched
        result.update({"drc": violations, "lvs": matched})
        result["fitness"] += 10.0 * (bool(violations) + (not matched))
    return result


# ── CLI ──────────────────────────────────────────────────────────────────────
def write_cell(cfg: SaeDelayConfig, out: Path) -> dict:
    """Writes GDS, SPICE, LVS reference, LEF and a JSON summary; returns the summary."""
    out.mkdir(parents=True, exist_ok=True)
    lib = new_library()
    cell = layout(cfg, lib=lib)
    measured = verify_topology(cell, cfg)
    lib.write_gds(str(out / f"{cfg.cell_name}.gds"))
    (out / f"{cfg.cell_name}.sp").write_text(netlist(cfg))
    (out / f"{cfg.cell_name}_lvs_ref.sp").write_text(lvs_schematic(cfg))
    (out / f"{cfg.cell_name}.lef").write_text(lef_abstract(cfg))
    summary = {
        "cell": cfg.cell_name, "width_nm": cfg.width, "height_nm": cfg.height,
        "width_cpp": cfg.width_cpp, "tiles": [t.kind for t in cfg.tiles],
        "pins": {k: list(v) for k, v in cfg.pin_positions.items()},
        "fins_per_stage": {str(k): v for k, v in measured.items()},
    }
    (out / f"{cfg.cell_name}.json").write_text(json.dumps(summary, indent=1))
    return summary


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Custom ASAP7 SAE delay cell generator (chipforge_asap7)")
    ap.add_argument("--stages", type=int, default=6)
    ap.add_argument("--nfin-n", type=int, default=2)
    ap.add_argument("--nfin-p", type=int, default=2)
    ap.add_argument("--no-nand", action="store_true", help="plain chain, no enable NAND")
    ap.add_argument("--vt", choices=tuple(VT_LAYERS), default="sram")
    ap.add_argument("--load-fF", type=float, default=8.0)
    ap.add_argument("--out", type=str, default="results/sae_delay_cell")
    ap.add_argument("--calibrate", action="store_true", help="run Xyce grid + write CAL_PATH")
    ap.add_argument("--sim-only", action="store_true", help="simulate one config and print")
    ap.add_argument("--drc", action="store_true", help="run the public ASAP7 runset on a terminated row")
    ap.add_argument("--lvs", action="store_true", help="run KLayout LVS against the unit-fin reference")
    args = ap.parse_args(argv)

    if args.calibrate:
        calibrate()
        return 0
    cfg = SaeDelayConfig(stages=args.stages, nfin_n=args.nfin_n, nfin_p=args.nfin_p,
                         nand_enable=not args.no_nand, load_fF=args.load_fF, vt=args.vt)
    if args.sim_only:
        print(json.dumps({"config": str(cfg), **simulate(cfg)}, indent=1))
        return 0

    out = REPO_ROOT / args.out
    summary = write_cell(cfg, out)
    print(f"{cfg.cell_name}: {cfg.width} x {cfg.height} nm ({cfg.width_cpp} CPP), "
          f"tiles={summary['tiles']}, fins/stage={summary['fins_per_stage']['0']} (stage 0)")
    print(f"wrote {out}")
    rc = 0
    if args.drc:
        violations = drc(cfg, out / "drc")
        print(f"DRC (terminated row): {'clean' if not violations else violations}")
        rc |= bool(violations)
    if args.lvs:
        result = lvs(cfg, out / "lvs")
        print(f"LVS: {'matched' if result.matched else 'MISMATCH'} ({result.log})")
        rc |= not result.matched
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
