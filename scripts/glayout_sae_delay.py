#!/usr/bin/env python3
"""Custom ASAP7 SAE delay cell as a gLayout Component.

Bridges scripts/generate_asap7_sae_delay (chipforge_asap7 geometry, gdspy,
nanometres) into gLayout's backend Component (gdstk, micrometres, port
database, GDS export), and proves equivalence by reading the gLayout-written
GDS back and running the same fin-level self-verification on it.

Scope of gLayout use -- and its documented limit -- is deliberate:
* USED: glayout.backend Component framework (add/add_port/write_gds).  This
  is the PDK-agnostic part of gLayout; it works with any (layer, datatype).
* NOT USED: glayout MappedPDK + nmos/pmos primitives + routing + DRC/LVS.
  Those are planar-only (continuous width, dogbone fingers, no fin/gate-cut/
  MOL layers, no ASAP7 grules or decks).  chipforge_asap7 supplies the FinFET
  device model instead; what is still missing is the adapter that turns a
  chipforge_asap7 cell into a gLayout Component with ports -- which is what
  this file hand-rolls for one cell.

The backend module is loaded standalone (importlib, no glayout/__init__
chain) because OpenFinRAM's venv has no pydantic/gdsfactory and the planar
stack is inapplicable anyway.  The backend needs pydantic only for its unused
Pdk settings shims, so a minimal stub is injected.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
import tempfile
from pathlib import Path
from typing import Any

import gdstk

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

GLAYOUT_SRC = Path("/home/jeff/iv4/repos/gLayout/src")
_BACKEND_PATH = GLAYOUT_SRC / "glayout" / "backend" / "_gdstk.py"

from scripts.generate_asap7_sae_delay import (  # noqa: E402
    LAYERS,
    M1_WIDTH,
    SaeDelayConfig,
    layout as native_layout,
    new_library,
    pin_shapes,
    verify_topology,
)

_M1 = (LAYERS["M1"]["layer"], LAYERS["M1"]["datatype"])
_UM = 1e-3  # nm -> um


def _load_glayout_backend() -> Any:
    """Loads glayout.backend._gdstk standalone with a minimal pydantic stub."""
    if not _BACKEND_PATH.exists():
        raise ImportError(f"gLayout backend not found: {_BACKEND_PATH}")
    import types as _types

    if "pydantic" not in sys.modules:
        stub = _types.ModuleType("pydantic")

        class _BaseModel:
            def __init_subclass__(cls, **kw: Any) -> None:
                merged: dict[str, Any] = {}
                for base in reversed(cls.__mro__[1:]):
                    merged.update(getattr(base, "_bm_defaults", {}))
                for key, val in vars(cls).items():
                    if key.startswith("_") or key == "model_config" or callable(val):
                        continue
                    merged[key] = val
                cls._bm_defaults = merged

            def __init__(self, **kw: Any) -> None:
                vals = dict(getattr(type(self), "_bm_defaults", {}))
                vals.update(kw)
                self.__dict__.update(vals)

        def _config_dict(**kw: Any) -> dict[str, Any]:
            return dict(kw)

        stub.BaseModel = _BaseModel
        stub.ConfigDict = _config_dict
        sys.modules["pydantic"] = stub
    spec = importlib.util.spec_from_file_location("glayout_backend_gdstk", _BACKEND_PATH)
    if spec is None or spec.loader is None:
        raise ImportError("could not load gLayout backend module")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["glayout_backend_gdstk"] = mod
    spec.loader.exec_module(mod)
    return mod


def native_gds(cfg: SaeDelayConfig, path: Path) -> Path:
    """Writes the chipforge_asap7 cell (nm units) to `path`."""
    lib = new_library()
    native_layout(cfg, lib=lib)
    lib.write_gds(str(path))
    return path


def _read_um(path: Path) -> gdstk.Cell:
    """Reads a GDS with gdstk, converting the file's units to micrometres."""
    return gdstk.read_gds(str(path), unit=1e-6).top_level()[0]


def build_component(cfg: SaeDelayConfig, backend: Any | None = None,
                    workdir: Path | None = None) -> Any:
    """Assembles the SAE cell as a gLayout Component with electrical ports.

    Geometry is the chipforge_asap7 drawing, round-tripped through GDS so the
    Component holds micrometre gdstk polygons; ports come from the generator's
    pin database (one port per pin, on its labelled M1 shape).
    """
    be = backend or _load_glayout_backend()
    work = Path(tempfile.mkdtemp(prefix="sae_glayout_")) if workdir is None else workdir
    native = _read_um(native_gds(cfg, work / f"{cfg.cell_name}_native.gds"))
    comp = be.Component(cfg.cell_name)
    comp.add(list(native.polygons))
    comp.add(list(native.labels))

    pins, _ = pin_shapes(cfg)
    orientation = {"IN": 180.0, "EN": 270.0, "OUT": 0.0, "VDD": 90.0, "VSS": 270.0}
    for pin, (x, y) in cfg.pin_positions.items():
        # Port width is the M1 track it sits on; the label point is inside a
        # pin rectangle by construction, which pin_shapes lets us assert.
        assert any(x0 <= x <= x1 and y0 <= y <= y1 for x0, y0, x1, y1 in pins[pin]), pin
        comp.add_port(pin, center=(x * _UM, y * _UM), width=M1_WIDTH * _UM,
                      orientation=orientation[pin], layer=_M1)
    return comp


def _sig(p: gdstk.Polygon) -> tuple:
    (x0, y0), (x1, y1) = p.bounding_box()
    return (p.layer, p.datatype, float(x0), float(y0), float(x1), float(y1))


def _boxes_match(a: list, b: list, tol_um: float = 1e-3) -> bool:
    """Bipartite match on layer + bbox within GDS grid tolerance (1 nm)."""
    unmatched = list(b)
    for pa in a:
        la, _, ax0, ay0, ax1, ay1 = pa
        hit = None
        for i, pb in enumerate(unmatched):
            lb, _, bx0, by0, bx1, by1 = pb
            if lb == la and abs(ax0 - bx0) <= tol_um and abs(ay0 - by0) <= tol_um \
                    and abs(ax1 - bx1) <= tol_um and abs(ay1 - by1) <= tol_um:
                hit = i
                break
        if hit is None:
            return False
        unmatched.pop(hit)
    return not unmatched


def check_equivalence(cfg: SaeDelayConfig, workdir: Path) -> dict:
    """Writes the cell via gLayout, reads it back, verifies fins + equality."""
    import gdspy

    be = _load_glayout_backend()
    assert be.__name__ == "glayout_backend_gdstk"  # native gdstk backend module
    comp = build_component(cfg, be, workdir)
    gds = workdir / f"{cfg.cell_name}_glayout.gds"
    comp.write_gds(str(gds))
    # Fin-level self-verification on the gLayout-written file, in nanometres.
    readback_nm = gdspy.GdsLibrary(infile=str(gds), unit=1e-9, precision=1e-10,
                                   units="convert")
    measured = verify_topology(readback_nm.cells[cfg.cell_name], cfg)
    native_boxes = [_sig(p) for p in _read_um(workdir / f"{cfg.cell_name}_native.gds").polygons]
    glayout_boxes = [_sig(p) for p in _read_um(gds).polygons]
    assert _boxes_match(native_boxes, glayout_boxes), "gLayout GDS differs beyond 1 nm"
    assert sorted(comp.ports) == sorted(cfg.pins), f"port db mismatch: {sorted(comp.ports)}"
    return {"gds": str(gds), "ports": sorted(comp.ports), "verify_stage0": measured[0]}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="SAE delay cell via gLayout backend")
    ap.add_argument("--stages", type=int, default=6)
    ap.add_argument("--nfin-n", type=int, default=2)
    ap.add_argument("--nfin-p", type=int, default=2)
    ap.add_argument("--no-nand", action="store_true")
    ap.add_argument("--out", type=str, default="results/sae_delay_cell_glayout")
    args = ap.parse_args(argv)
    cfg = SaeDelayConfig(stages=args.stages, nfin_n=args.nfin_n, nfin_p=args.nfin_p,
                         nand_enable=not args.no_nand)
    out = REPO_ROOT / args.out
    out.mkdir(parents=True, exist_ok=True)
    result = check_equivalence(cfg, out)
    print(f"gLayout backend: {result['gds']}")
    print(f"ports: {result['ports']}, stage-0 fins: {result['verify_stage0']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
