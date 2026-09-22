"""Frame, static checks, drawing and metrics for the evolvable driver-slice router.

The thing being evolved is `route_slice(frame)` in initial_program.py: given a
plain-data description of a four-wordline driver slice (chipforge_asap7's
`DriverSliceSpec`, leaf cells already placed) it returns the slice's own
M2/M3/V1/V2 rectangles and where it put the pins.  Analytical sizing covers
the devices; what it cannot cover is this part -- track assignment, via
placement, and whether the slice leaves its pins reachable and its tracks free
for the predecode bus -- which is combinatorial and only DRC can finally judge.

This module is the fixed side of that contract:

* `make_frame`   what a candidate is told (JSON-safe, nanometres, slice coords)
* `static_check` millisecond rule and connectivity checks with precise messages
* `make_router`  adapts a candidate's output to `build_driver_slice(router=...)`
* `objectives`   pin escape, M2/M3 feed-through capacity, wire cost
"""

from __future__ import annotations

from typing import Any

from chipforge_asap7.devices import (
    DriverSliceSpec,
    InverterSpec,
    NandSpec,
    build_driver_slice,
)
from chipforge_asap7.layout import LAYERS, box

Box = tuple[float, float, float, float]

HALF, CAP, M2_PAD, TRACK, SPACE = 9, 14, 17, 36, 18
ROUTE_LAYERS = ("M2", "M3")
VIA_LAYERS = ("V1", "V2")
MAX_SHAPES = 300
INPUT_PINS = ("SEL", "B0", "B1", "B2", "B3")
WL_PINS = ("WL0", "WL1", "WL2", "WL3")

#: Every candidate is judged on all of these, so it cannot overfit one size.
#: The drivers keep their full input straps: `DriverSliceSpec` would otherwise
#: trim them to where *its* router lands, and a candidate may land anywhere.
SPECS: dict[str, DriverSliceSpec] = {
    "small": DriverSliceSpec(
        nand=NandSpec(rows=((6, 4),)),
        inverter=InverterSpec(rows=((8, 8), (4, 4))),
        trim_driver_input=False,
    ),
    "one_row": DriverSliceSpec(
        nand=NandSpec(rows=((4, 2),), vt="lvt"),
        inverter=InverterSpec(rows=((6, 6),), vt="lvt"),
        trim_driver_input=False,
    ),
    "released": DriverSliceSpec(trim_driver_input=False),
}


# ── Frame ────────────────────────────────────────────────────────────────────
def _flat_boxes(cell: Any, layer: str) -> list[Box]:
    key = (LAYERS[layer]["layer"], LAYERS[layer]["datatype"])
    return sorted(
        (float(p[:, 0].min()), float(p[:, 1].min()), float(p[:, 0].max()), float(p[:, 1].max()))
        for p in cell.get_polygons(by_spec=True).get(key, [])
    )


def leaf_obstacles(spec: DriverSliceSpec) -> dict[str, list[Box]]:
    """M2 and M3 the leaf cells already own (B ties, the drivers' rail crossings)."""
    bare = build_driver_slice(spec, router=lambda _cell, _spec: None, draw_pin_labels=False)
    return {layer: _flat_boxes(bare, layer) for layer in ROUTE_LAYERS}


def make_frame(spec: DriverSliceSpec) -> dict:
    """Everything a router may rely on, as plain JSON-safe data in slice coordinates."""
    nand = spec.nand
    bar_lo, bar_hi = nand.gate_bar_y
    y_lo = nand.n_band.contact_y - HALF
    y_hi = nand.p_band.contact_y + HALF
    b_gates = nand.pad_gate_xs("B")
    nands = []
    for wordline, origin_x, top in sorted(spec.nand_placements):
        def span(a: float, b: float, top: bool = top) -> tuple[float, float]:
            ya, yb = spec.nand_point(0, top, 0, a)[1], spec.nand_point(0, top, 0, b)[1]
            return min(ya, yb), max(ya, yb)

        a0, a1 = span(bar_lo, bar_hi)
        y0, y1 = span(y_lo, y_hi)
        nands.append({
            "wordline": wordline,
            "a_bar": {"x": origin_x + nand.pad_gate_xs("A")[0], "y0": a0, "y1": a1,
                      "via_y": spec.nand_point(0, top, 0, nand.seam_y)[1]},
            "y_bar": {"x": origin_x + spec.output_bar_x, "y0": y0, "y1": y1},
            "b_tie": {"x0": origin_x + b_gates[0] - HALF, "x1": origin_x + b_gates[-1] + HALF,
                      "y": spec.nand_point(0, top, 0, nand.tie_ys["B"])[1]},
        })
    strap_lo, strap_hi = spec.input_strap_y
    drivers = [
        {"wordline": i,
         "input_strap": {"x": spec.input_xs[i], "y0": strap_lo, "y1": strap_hi},
         "wl_pad": {"x": spec.wordline_xs[i], "y": spec.wordline_via_y}}
        for i in range(4)
    ]
    obstacles = leaf_obstacles(spec)
    return {
        "width": spec.width, "height": spec.height, "driver_y0": spec.driver_y0,
        "rails": [[y, net] for y, net in spec.rails],
        "nands": nands, "drivers": drivers,
        "blocked_m2": [list(b) for b in obstacles["M2"]],
        "blocked_m3": [list(b) for b in obstacles["M3"]],
        "rules": {"track_width": 18, "min_space": SPACE, "end_to_end_space": 31,
                  "via_size": 18, "m2_past_via": 8, "metal_past_via_along_track": CAP,
                  "edge_keepout": HALF},
    }


# ── Static checks ────────────────────────────────────────────────────────────
def _overlap(a: Box, b: Box, margin: float = 0.0) -> bool:
    return (a[0] - margin < b[2] and b[0] - margin < a[2]
            and a[1] - margin < b[3] and b[1] - margin < a[3])


def _contains(outer: Box, inner: Box) -> bool:
    return outer[0] <= inner[0] and outer[1] <= inner[1] and outer[2] >= inner[2] and outer[3] >= inner[3]


def normalise(result: Any) -> tuple[list[tuple[str, Box]], dict[str, tuple[str, float, float]], list[str]]:
    """Validate the shape of a candidate's return value; never raises."""
    errors: list[str] = []
    if not isinstance(result, dict) or "shapes" not in result or "pins" not in result:
        return [], {}, ["route_slice must return {'shapes': [...], 'pins': {...}}"]
    shapes: list[tuple[str, Box]] = []
    for index, item in enumerate(result["shapes"][: MAX_SHAPES + 1]):
        try:
            layer, x0, y0, x1, y1 = item
            rect = (float(min(x0, x1)), float(min(y0, y1)), float(max(x0, x1)), float(max(y0, y1)))
        except (TypeError, ValueError):
            errors.append(f"shape {index} is not [layer, x0, y0, x1, y1]: {item!r}"[:160])
            continue
        if layer not in ROUTE_LAYERS + VIA_LAYERS:
            errors.append(f"shape {index}: layer {layer!r} not allowed (use M2, M3, V1, V2)")
            continue
        if any(v != v or abs(v) > 1e6 for v in rect):
            errors.append(f"shape {index}: coordinate is not a finite nanometre value")
            continue
        shapes.append((layer, rect))
    if len(result["shapes"]) > MAX_SHAPES:
        errors.append(f"more than {MAX_SHAPES} shapes")
    pins: dict[str, tuple[str, float, float]] = {}
    for pin in INPUT_PINS + WL_PINS:
        try:
            metal, x, y = result["pins"][pin]
            pins[pin] = (str(metal), float(x), float(y))
        except (KeyError, TypeError, ValueError):
            errors.append(f"pins[{pin!r}] must be [metal, x, y]")
    return shapes, pins, errors


def static_check(frame: dict, result: Any) -> tuple[list[str], dict]:
    """Rule and connectivity checks that need no EDA tool.  Returns (errors, netinfo)."""
    shapes, pins, errors = normalise(result)
    width, height = frame["width"], frame["height"]
    metal = {layer: [r for l, r in shapes if l == layer] for layer in ROUTE_LAYERS}
    vias = {layer: [r for l, r in shapes if l == layer] for layer in VIA_LAYERS}

    for layer, rect in shapes:
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        if layer in VIA_LAYERS and (abs(w - 18) > 1e-6 or abs(h - 18) > 1e-6):
            errors.append(f"{layer} at ({rect[0]:.0f},{rect[1]:.0f}) is {w:.0f}x{h:.0f}; vias are 18x18")
        if layer in ROUTE_LAYERS and min(w, h) < 18 - 1e-6:
            errors.append(f"{layer} at ({rect[0]:.0f},{rect[1]:.0f}) is {min(w, h):.0f} nm wide; minimum 18")
        if rect[0] < HALF or rect[2] > width - HALF or rect[1] < 0 or rect[3] > height:
            errors.append(f"{layer} {tuple(round(v) for v in rect)} leaves the slice "
                          f"(keep x in [{HALF}, {width - HALF}], y in [0, {height}])")
    for layer in ROUTE_LAYERS:
        for rect in metal[layer]:
            for blocked in frame[f"blocked_{layer.lower()}"]:
                if _overlap(rect, tuple(blocked), SPACE):
                    errors.append(f"{layer} {tuple(round(v) for v in rect)} is within {SPACE} nm of "
                                  f"leaf-cell {layer} {tuple(round(v) for v in blocked)}")
                    break

    # Landings: the only M1 a V1 may touch, each with its net.
    landings: list[tuple[str, Box]] = []
    for n in frame["nands"]:
        a, y = n["a_bar"], n["y_bar"]
        landings.append(("SEL", (a["x"] - HALF, a["y0"] + CAP - HALF, a["x"] + HALF, a["y1"] - CAP + HALF)))
        landings.append((f"N{n['wordline']}", (y["x"] - HALF, y["y0"] + CAP - HALF, y["x"] + HALF, y["y1"] - CAP + HALF)))
    for d in frame["drivers"]:
        s, p = d["input_strap"], d["wl_pad"]
        landings.append((f"N{d['wordline']}", (s["x"] - HALF, s["y0"] + CAP - HALF, s["x"] + HALF, s["y1"] - CAP + HALF)))
        landings.append((f"WL{d['wordline']}", (p["x"] - HALF, p["y"] - HALF, p["x"] + HALF, p["y"] + HALF)))

    # Union-find over candidate metal; vias join layers; landings seed net names.
    nodes = [("M2", r) for r in metal["M2"]] + [("M3", r) for r in metal["M3"]]
    parent = list(range(len(nodes)))

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i, (la, ra) in enumerate(nodes):
        for j in range(i + 1, len(nodes)):
            lb, rb = nodes[j]
            if la == lb and _overlap(ra, rb, 1e-6):
                parent[find(i)] = find(j)
    seeds: dict[int, set[str]] = {}
    for via in vias["V2"]:
        lower = [i for i, (l, r) in enumerate(nodes) if l == "M2" and _contains(r, via)]
        upper = [i for i, (l, r) in enumerate(nodes) if l == "M3" and _contains(r, via)]
        if not lower or not upper:
            errors.append(f"V2 at ({via[0] + 9:.0f},{via[1] + 9:.0f}) is not inside one M2 and one M3 rectangle")
            continue
        parent[find(lower[0])] = find(upper[0])
    for via in vias["V1"]:
        nets = [net for net, land in landings if _contains(land, via)]
        lower = [i for i, (l, r) in enumerate(nodes) if l == "M2" and _contains(r, via)]
        if not nets:
            errors.append(f"V1 at ({via[0] + 9:.0f},{via[1] + 9:.0f}) is not on a landing "
                          "(a_bar, y_bar, input_strap inside their ends by 14 nm, or exactly on wl_pad)")
        if not lower:
            errors.append(f"V1 at ({via[0] + 9:.0f},{via[1] + 9:.0f}) is not inside one M2 rectangle")
        elif not any(r[0] <= via[0] - 8 or r[2] >= via[2] + 8 for l, r in [nodes[i] for i in lower]):
            errors.append(f"V1 at ({via[0] + 9:.0f},{via[1] + 9:.0f}): its M2 must run 8 nm past it on one side")
        if nets and lower:
            seeds.setdefault(find(lower[0]), set()).add(nets[0])
    groups: dict[int, set[str]] = {}
    for root, nets in seeds.items():
        groups.setdefault(find(root), set()).update(nets)
    for nets in groups.values():
        if len(nets) > 1:
            errors.append(f"short: one metal group touches nets {sorted(nets)}")
    reached = {net: sum(1 for via in vias["V1"] for n, land in landings if n == net and _contains(land, via))
               for net in {n for n, _ in landings}}
    want = {"SEL": 4, **{f"N{i}": 2 for i in range(4)}, **{f"WL{i}": 1 for i in range(4)}}
    for net, count in sorted(want.items()):
        if reached.get(net, 0) < count:
            errors.append(f"net {net}: {reached.get(net, 0)} of {count} landings have a V1")
        roots = {find(i) for i, (l, r) in enumerate(nodes)
                 for via in vias["V1"] for n, land in landings
                 if n == net and l == "M2" and _contains(land, via) and _contains(r, via)}
        if len(roots) > 1:
            errors.append(f"net {net} is left in {len(roots)} disconnected pieces")
    for i in range(4):
        root = [find(k) for k, (l, r) in enumerate(nodes) if groups.get(find(k)) == {f"WL{i}"}]
        tops = [nodes[k][1][3] for k in range(len(nodes)) if nodes[k][0] == "M3" and find(k) in root]
        if not tops or max(tops) < height - 1e-6:
            errors.append(f"WL{i}: no M3 of that net reaches the top edge y={height}")

    net_of = {i: next(iter(groups[find(i)])) for i in range(len(nodes)) if len(groups.get(find(i), ())) == 1}
    for pin, (layer, x, y) in pins.items():
        if pin in WL_PINS or pin == "SEL":
            on = [i for i, (l, r) in enumerate(nodes) if l == layer and r[0] <= x <= r[2] and r[1] <= y <= r[3]]
            if not on or net_of.get(on[0]) != pin:
                errors.append(f"pin {pin} at ({x:.0f},{y:.0f}) on {layer} is not on that net's metal")
    return errors, {"nodes": nodes, "net_of": net_of}


# ── Drawing ──────────────────────────────────────────────────────────────────
def make_router(result: dict):
    """Adapt a candidate's output to `build_driver_slice(router=...)`."""
    shapes, pins, _ = normalise(result)

    def router(cell: Any, _spec: DriverSliceSpec):
        for layer, (x0, y0, x1, y1) in shapes:
            box(cell, layer, round(x0), round(y0), round(x1), round(y1))
        return {pin: (metal, (x, y)) for pin, (metal, x, y) in pins.items()}

    return router


# ── Objectives ───────────────────────────────────────────────────────────────
def objectives(frame: dict, result: dict) -> dict[str, float]:
    """Congestion and pin-access measures of a legal routing (all in [0, 1] except cost).

    pin_escape      per input pin, how many M3 columns can drop from the pin's M2
                    straight to the slice's bottom edge, where the predecode bus
                    and the slice's small gates are; capped at four, averaged
    m2_feedthrough  share of horizontal M2 tracks over the NAND rows a bus wire
                    could cross the whole slice on
    m3_feedthrough  share of vertical M3 columns clear from the bottom edge to
                    the drivers
    wire_cost       added M2 + M3 length plus 100 nm per via (an RC proxy)
    """
    shapes, pins, _ = normalise(result)
    _, netinfo = static_check(frame, result)
    width, y_top = frame["width"], frame["driver_y0"]
    m2 = [r for l, r in shapes if l == "M2"] + [tuple(b) for b in frame["blocked_m2"]]
    m3_named = [(netinfo["net_of"].get(i), r) for i, (l, r) in enumerate(netinfo["nodes"]) if l == "M3"]
    m3_named += [(None, tuple(b)) for b in frame["blocked_m3"]]

    rows = [y for y in range(TRACK // 2, int(y_top), TRACK)]
    free_rows = [y for y in rows if not any(r[1] < y + HALF + SPACE and r[3] > y - HALF - SPACE for r in m2)]
    columns = [x for x in range(TRACK // 2, int(width), TRACK) if HALF <= x - HALF and x + HALF <= width - HALF]

    def column_clear(x: float, y_to: float, net: str | None) -> bool:
        lane = (x - HALF - SPACE, 0.0, x + HALF + SPACE, y_to + HALF + CAP)
        return not any(_overlap(lane, r) for owner, r in m3_named if owner is None or owner != net)

    free_columns = [x for x in columns if column_clear(x, y_top, None)]

    pin_m2: dict[str, list[Box]] = {f"B{n['wordline']}": [(n["b_tie"]["x0"], n["b_tie"]["y"] - HALF,
                                                          n["b_tie"]["x1"], n["b_tie"]["y"] + HALF)]
                                    for n in frame["nands"]}
    pin_m2["SEL"] = [r for i, (l, r) in enumerate(netinfo["nodes"])
                     if l == "M2" and netinfo["net_of"].get(i) == "SEL"]
    escapes = {}
    for pin in INPUT_PINS:
        count = 0
        for x in columns:
            via = (x - HALF, 0.0, x + HALF, 0.0)
            for r in pin_m2.get(pin, []):
                lands = r[0] <= via[0] and via[2] <= r[2] and (r[0] <= via[0] - 8 or r[2] >= via[2] + 8)
                if lands and r[3] - r[1] <= 18 + 1e-6 and column_clear(x, r[1] + HALF, pin):
                    count += 1
                    break
        escapes[pin] = count
    length = sum((r[2] - r[0]) + (r[3] - r[1]) - 18 for l, r in shapes if l in ROUTE_LAYERS)
    via_count = sum(1 for l, _ in shapes if l in VIA_LAYERS)
    return {
        "pin_escape": sum(min(c, 4) for c in escapes.values()) / (4 * len(INPUT_PINS)),
        "m2_feedthrough": len(free_rows) / max(1, len(rows)),
        "m3_feedthrough": len(free_columns) / max(1, len(columns)),
        "wire_cost": length + 100.0 * via_count,
        **{f"escape_{pin}": float(c) for pin, c in escapes.items()},
    }
