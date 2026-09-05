#!/usr/bin/env python3
"""Assemble and route the 2RW macro from real ASAP7 hard-cell geometry.

Every disconnected supply component is a separate routing terminal.  Physical
pin connectivity is extracted again after DEF stream-out before publishing GDS.
The controller is the already placed/routed ctrl_decode from this compiler run.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re
import subprocess

import gdstk
from asap7_connectivity import MetalGraph, METALS
import generate_asap7_wordline_arrays as arrays
import generate_asap7_8t_iocolumn as columns

REPO = Path(__file__).resolve().parents[1]
LAYER_NAMES = dict(zip(METALS, (f"M{i}" for i in range(1, 10))))


def supply(text):
    name = text.lower().rstrip("!")
    return name if name in ("vdd", "vss") else None


def probe_point(polygon, offset):
    """Choose an interior point that survives GDS grid conversion."""
    inset = gdstk.offset([polygon], -0.0005, precision=1e-7)
    if not inset:
        raise RuntimeError("conductor too narrow for a physical connectivity probe")
    return tuple(float(v) - o for v, o in zip(inset[0].points[0], offset))


def abstract(cell, name, signal_labels):
    """Create an exact metal abstract and a pin for every named component."""
    graph = MetalGraph(cell)
    roots = {}
    for label in signal_labels:
        root = graph.label_root(label)
        if root in roots and roots[root] != label.text:
            raise RuntimeError(f"{name}: short {roots[root]} / {label.text}")
        roots[root] = label.text
    for label in cell.get_labels():
        net = supply(label.text)
        if net is None or label.layer not in METALS:
            continue
        root = graph.label_root(label)
        if root in roots and roots[root] != net:
            raise RuntimeError(f"{name}: short {roots[root]} / {net}")
        roots[root] = net
    if not {"vdd", "vss"}.issubset(roots.values()):
        raise RuntimeError(f"{name}: missing supply conductors")

    # Retain overhangs inside the abstract boundary; never hide device geometry
    # from the router by using only the smaller SRAM abutment rectangle.
    low, high = cell.bounding_box()
    ox, oy = (math.floor(float(v) * 1000) / 1000 for v in low)
    width, height = (
        math.ceil((float(v) - o) * 1000) / 1000 for v, o in zip(high, (ox, oy))
    )
    master = gdstk.Cell(name)
    master.add(gdstk.Reference(cell, origin=(-ox, -oy)))
    groups = defaultdict(list)
    for i, polygon in enumerate(graph.polygons):
        if polygon.layer in METALS:
            groups[graph.root(i)].append(polygon)
    terminals = {}
    obstacles = [
        p for root, polys in groups.items() if root not in roots for p in polys
    ]
    lef = [
        f"MACRO {name}",
        "  CLASS BLOCK ;",
        "  ORIGIN 0 0 ;",
        f"  SIZE {width:.6f} BY {height:.6f} ;",
        "  SYMMETRY X Y ;",
    ]

    def geometry(polygons):
        out = []
        by_layer = defaultdict(list)
        for poly in polygons:
            by_layer[poly.layer].append(poly)
        for layer, polys in sorted(by_layer.items()):
            out.append(f"    LAYER {LAYER_NAMES[layer]} ;")
            for poly in gdstk.boolean(polys, [], "or", precision=1e-6):
                points = " ".join(f"{x - ox:.6f} {y - oy:.6f}" for x, y in poly.points)
                out.append(f"    POLYGON {points} ;")
        return out

    for root, net in sorted(roots.items(), key=lambda item: (item[1], item[0])):
        pin = f"P{len(terminals)}"
        polys = groups[root]
        if name == "dp_controller" and net not in ("vdd", "vss"):
            labels = [label for label in signal_labels if label.text == net]
            access_layers = {label.layer for label in labels}
            # Include the connected landing wire, not just its pin marker:
            # marking its immediately adjacent same-net extension as OBS
            # creates artificial spacing violations during via access.
            access = [p for p in polys if p.layer in access_layers]
            if not access:
                raise RuntimeError(
                    f"{name}: no explicit physical pin landing for {net}"
                )
            obstacles.extend(p for p in polys if p.layer not in access_layers)
        else:
            access_layer = max(p.layer for p in polys)
            access = [p for p in polys if p.layer == access_layer]
            obstacles.extend(p for p in polys if p.layer != access_layer)
        # Signal use is intentional: even the supply trees must be physically
        # routed here, not collapsed into unconnected global/special nets.
        direction = (
            "OUTPUT"
            if (
                net.startswith(("QA", "QB"))
                or (
                    name == "dp_controller"
                    and net.startswith(
                        ("wl", "wrena", "ysel", "sae", "oe_out", "oeb_out", "blprech")
                    )
                )
            )
            else "INPUT"
        )
        if net in ("vdd", "vss"):
            direction = "INOUT"
        lef += [
            f"  PIN {pin}",
            f"    DIRECTION {direction} ;",
            "    USE SIGNAL ;",
            "    PORT",
        ]
        lef += geometry(access)
        lef += ["    END", f"  END {pin}"]
        polygon = max(access, key=lambda p: p.area())
        point = probe_point(polygon, (ox, oy))
        terminals[pin] = {"net": net, "layer": polygon.layer, "point": point}
    lef += ["  OBS"]
    lef += geometry(obstacles)
    lef += ["  END", f"END {name}"]
    # Freeze the partition of unnamed internal interconnect too. A route that
    # shorts into a storage node or a controller internal net must fail even
    # if all exported pins still appear mutually isolated.
    for root, polys in groups.items():
        if root in roots:
            continue
        polygon = max(polys, key=lambda p: p.area())
        point = probe_point(polygon, (ox, oy))
        terminals[f"internal_{root}"] = {
            "net": f"internal_{root}",
            "layer": polygon.layer,
            "point": point,
            "private": True,
        }
    master.add(gdstk.rectangle((0, 0), (width, height), layer=100))
    return master, terminals, "\n".join(lef), (width, height)


def leaf_net(name, bank, bit, wordlines):
    if name in ("vdd", "vss"):
        return name
    if name in ("DA", "DB", "QA", "QB"):
        return f"{name[0]}_{name[1]}[{bit}]"
    wl = re.fullmatch(r"WL([TB])([AB])\[(\d+)\]", name)
    if wl:
        return f"wl{wl[1].lower()}_{wl[2]}[{bank * wordlines + int(wl[3])}]"
    mux = re.fullmatch(r"(yseltn|yselt|yselbn|yselb)([AB])\[(\d+)\]", name)
    if mux:
        return f"{mux[1]}_{mux[2]}[{bank * 4 + int(mux[3])}]"
    ctrl = re.fullmatch(
        r"(wrena|wrenan|oeb_out|oe_out|blprechtn|blprechbn)([AB])", name
    )
    if ctrl:
        return f"{ctrl[1]}_{ctrl[2]}[{bank}]"
    if name in ("sae_A", "sae_B"):
        return f"{name}[{bank}]"
    raise RuntimeError(f"unmapped column pin {name}")


def build_leaf(wordlines, tap_pitch):
    gds = REPO / "tech/gds"
    lib = arrays.build_library(
        gds / "sram_cell_8t.gds",
        gds / "srambank_32b_boundary_2.gds",
        [wordlines],
        4,
        gds / "sram_cell_8t_tap.gds",
        tap_pitch,
    )
    cells = {c.name: c for c in lib.cells}
    wrappers = {
        c.name: c for c in gdstk.read_gds(str(gds / "sram_8t_ioprech.gds")).cells
    }
    edges = {
        c.name: c for c in gdstk.read_gds(str(gds / "sram_cell_8t_edges.gds")).cells
    }
    io = columns.build_combined_io(
        lib, wrappers["ioprech_sram_8t_a"], wrappers["ioprech_sram_8t_b"]
    )
    caps = columns.build_cap_array(lib, edges["sram_cell_8t_col_cap"])
    array = cells[f"array_x{wordlines}x4_tap{tap_pitch}_sram_8t"]
    leaf = columns.build_colgrp(lib, array, io, caps, wordlines)
    # Terminate each half-array's process frame with the generated row/corner
    # family. Preserve alternating-X orientation, including slots inserted for
    # taps. No 6T filler pitch or hand-drawn transistor geometry is used.
    capped = lib.new_cell("capped_" + leaf.name)
    capped.add(gdstk.Reference(leaf))
    for label in leaf.labels:
        capped.add(columns.clone_label(label, label.text, tuple(label.origin)))
    row = cells[f"sramcol_x{wordlines}_tap{tap_pitch}_sram_8t"]
    width = columns.boundary_box(leaf)[2]
    bx0, by0, bx1, by1 = columns.boundary_box(cells["sram_cell_8t"])
    slot_width, pitch = bx1 - bx0, by1 - by0
    rowcap, corner = edges["sram_cell_8t_row_cap"], edges["sram_cell_8t_corner"]
    # Build left end rows then mirror them to the right half-array.
    ends = lib.new_cell("dp_array_end_rows")
    for ref in row.references:
        cap = rowcap if ref.cell.name == "sram_cell_8t" else corner
        x0, y0, x1, y1 = columns.boundary_box(cap)
        # The row's bitcell references already encode the X mirror. Its
        # origin.y normalizes the canonical cell's nonzero lower boundary.
        mirror_x = bool(ref.rotation)
        slot = min(
            float(p[0])
            for p in gdstk.Reference(
                ref.cell,
                origin=ref.origin,
                rotation=ref.rotation,
                x_reflection=ref.x_reflection,
            )
            .get_polygons(layer=100, datatype=0)[0]
            .points
        )
        for bottom in (True, False):
            # Bottom end faces the row through an X-axis reflection; top
            # active mux row is already mirrored, so its end is unreflected.
            mx, my = mirror_x, bottom
            origin = (
                slot_width + slot + (x1 if mx else -x0),
                y0 if my else 4 * pitch - y0,
            )
            ends.add(
                gdstk.Reference(
                    cap,
                    origin=origin,
                    rotation=math.pi if mx else 0,
                    x_reflection=mx != my,
                )
            )
    x0, y0, x1, y1 = columns.boundary_box(corner)
    for bottom in (True, False):
        ends.add(
            gdstk.Reference(
                corner,
                origin=(-x0, y0 if bottom else 4 * pitch - y0),
                x_reflection=bottom,
            )
        )
    capped.add(gdstk.Reference(ends))
    capped.add(
        gdstk.Reference(ends, origin=(width, 0), rotation=math.pi, x_reflection=True)
    )
    columns.rect(capped, (0, -pitch, width, 5 * pitch), 100)
    return capped


def run(args):
    if args.wordlines < 2 or args.wordlines % 2 or args.bits < 2 or args.bits % 2:
        raise RuntimeError("wordlines and bits must be positive even values >= 2")
    if args.banks < 1 or args.banks & (args.banks - 1):
        raise RuntimeError("banks must be a power of two")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    tap_pitch = math.gcd(args.wordlines, 16)
    leaf = build_leaf(args.wordlines, tap_pitch)
    leaf_labels = [label for label in leaf.labels if not supply(label.text)]
    hard, pins, lef, size = abstract(leaf, "dp_column", leaf_labels)
    ctrl_lib = gdstk.read_gds(str(args.controller))
    ctrl = next(c for c in ctrl_lib.cells if c.name == "ctrl_decode")
    add_pin_conductors(ctrl)
    ctrl_labels = [
        label
        for label in ctrl.labels
        if label.texttype == 251 and not supply(label.text)
    ]
    controller, ctrl_pins, ctrl_lef, ctrl_size = abstract(
        ctrl, "dp_controller", ctrl_labels
    )
    masters = {"dp_column": (hard, pins), "dp_controller": (controller, ctrl_pins)}
    (work / "blocks.lef").write_text(
        'VERSION 5.8 ;\nBUSBITCHARS "[]" ;\nDIVIDERCHAR "/" ;\n'
        + lef
        + "\n"
        + ctrl_lef
        + "\nEND LIBRARY\n"
    )
    library = gdstk.Library(unit=1e-6, precision=1e-10)
    for c in (hard, controller):
        for dep in [c, *c.dependencies(True)]:
            if dep.name not in {p.name for p in library.cells}:
                library.add(dep)
    library.write_gds(str(work / "blocks.gds"), timestamp=columns.FIXED_GDS_TIMESTAMP)

    top_name = f"sram_x{args.wordlines * 2}x{args.bits}x{args.banks}"
    # Independent column tiles provide routing channels and avoid inheriting
    # the 6T stack's 13.5 nm overlap. Banks share data, not wordline addresses.
    gap = args.channel_width
    if gap < 2.0:
        raise RuntimeError("routing channels must be at least 2 um wide")
    # Preserve the validated pin-access phase in each bank. 2.88 um is the
    # least common multiple of all M1-M9 routing pitches; an arbitrary bank
    # stride can strand the small cap/tap ground terminals between tracks.
    bank_stride = math.ceil((size[0] + gap) / 2.88) * 2.88
    width = max(
        (args.banks - 1) * bank_stride + size[0] + 2 * gap, ctrl_size[0] + 2 * gap
    )
    height = args.bits * (size[1] + gap) + ctrl_size[1] + 3 * gap
    instances = [("CTRL", "dp_controller", (gap, height - ctrl_size[1] - gap))]
    nets = defaultdict(list)
    probes = defaultdict(list)
    for pin, info in ctrl_pins.items():
        if info.get("private"):
            continue
        net = info["net"]
        nets[net].append(("CTRL", pin))
    for bank in range(args.banks):
        for bit in range(args.bits):
            inst = f"COL_{bank}_{bit}"
            origin = (gap + bank * bank_stride, gap + bit * (size[1] + gap))
            instances.append((inst, "dp_column", origin))
            for pin, info in pins.items():
                if info.get("private"):
                    continue
                nets[leaf_net(info["net"], bank, bit, args.wordlines)].append(
                    (inst, pin)
                )

    external = {"clk", "rst_n", "vdd", "vss"}
    external.update(f"{p}_n_{port}" for p in ("ce", "we", "oe") for port in "AB")
    address_bits = (2 * args.wordlines * 4 * args.banks - 1).bit_length()
    external.update(f"A_{port}[{bit}]" for port in "AB" for bit in range(address_bits))
    external.update(
        f"{p}_{port}[{bit}]" for p in "DQ" for port in "AB" for bit in range(args.bits)
    )
    for net in nets:
        if net not in external and not any(inst == "CTRL" for inst, pin in nets[net]):
            raise RuntimeError(f"controller has no driver for {net}")
    if external - nets.keys():
        raise RuntimeError(f"missing external nets {sorted(external - nets.keys())}")

    def dbu(value):
        return round(value * 1000)

    d = [
        "VERSION 5.8 ;",
        'DIVIDERCHAR "/" ;',
        'BUSBITCHARS "[]" ;',
        f"DESIGN {top_name} ;",
        "UNITS DISTANCE MICRONS 1000 ;",
        f"DIEAREA ( 0 0 ) ( {dbu(width)} {dbu(height)} ) ;",
        f"COMPONENTS {len(instances)} ;",
    ]
    for inst, master, (x, y) in instances:
        d.append(f"- {inst} {master} + FIXED ( {dbu(x)} {dbu(y)} ) N ;")
        for pin, info in masters[master][1].items():
            if info.get("private"):
                net = f"private:{inst}:{pin}"
            else:
                net = (
                    info["net"]
                    if inst == "CTRL"
                    else leaf_net(
                        info["net"],
                        int(inst.split("_")[1]),
                        int(inst.split("_")[2]),
                        args.wordlines,
                    )
                )
            probes[net].append(
                {
                    "instance": inst,
                    "pin": pin,
                    "layer": info["layer"],
                    "point": [x + info["point"][0], y + info["point"][1]],
                }
            )
    d += ["END COMPONENTS", f"PINS {len(external)} ;"]
    for net in sorted(external):
        direction = (
            "INOUT"
            if net in ("vdd", "vss")
            else ("OUTPUT" if net.startswith("Q_") else "INPUT")
        )
        d.append(f"- {net} + NET {net} + DIRECTION {direction} + USE SIGNAL ;")
        nets[net].append(("PIN", net))
    d += ["END PINS", f"NETS {len(nets)} ;"]
    for net, ends in sorted(nets.items()):
        d.append(
            f"- {net} " + " ".join(f"( {inst} {pin} )" for inst, pin in ends) + " ;"
        )
    d += ["END NETS", "END DESIGN"]
    (work / "placed.def").write_text("\n".join(d) + "\n")
    manifest = {
        "cell": top_name,
        "size": [width, height],
        "tap_pitch": tap_pitch,
        "nets": probes,
        "external": sorted(external),
    }
    (work / "connectivity.json").write_text(json.dumps(manifest, indent=2) + "\n")
    tcl = [
        f"read_lef {{{REPO}/tech/lef/asap7_tech.lef}}",
        "read_lef blocks.lef",
        "read_def placed.def",
        "set_thread_count 2",
    ]
    for i, pitch in enumerate(
        (0.036, 0.036, 0.036, 0.048, 0.048, 0.064, 0.064, 0.080, 0.080), 1
    ):
        tcl.append(
            f"make_tracks M{i} -x_offset 0 -y_offset 0 -x_pitch {pitch} -y_pitch {pitch}"
        )
    tcl += [
        "place_pins -hor_layers M8 -ver_layers M9",
        "set_routing_layers -signal M1-M9",
        "global_route",
        f"detailed_route -droute_end_iter {args.route_iterations} -output_drc macro_drc.rpt",
        "write_def routed.def",
        "write_db routed.odb",
        "exit",
    ]
    (work / "route.tcl").write_text("\n".join(tcl) + "\n")
    with (work / "route.log").open("w") as log:
        result = subprocess.run(
            [args.openroad, "-exit", "route.tcl"],
            cwd=work,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=args.route_timeout,
        )
    if result.returncode:
        raise RuntimeError(f"macro routing failed; see {work}/route.log")
    # Distribution KLayout bindings may target system Python, while gdstk is
    # installed in a virtual environment with a different Python ABI.
    subprocess.run(
        [
            "python3",
            str(REPO / "scripts/def_to_gds.py"),
            "--def-file",
            str(work / "routed.def"),
            "--tech-lef",
            str(REPO / "tech/lef/asap7_tech.lef"),
            "--cell-lef",
            str(work / "blocks.lef"),
            "--macro-gds",
            str(work / "blocks.gds"),
            "--output-gds",
            str(work / "routed.gds"),
            "--top-cell",
            top_name,
        ],
        check=True,
    )
    lib = gdstk.read_gds(str(work / "routed.gds"))
    top = next(c for c in lib.cells if c.name == top_name)
    # KLayout writes DEF BPin rectangles on purpose 251. They are real
    # conductor landings as well as LEF pin markers, not label-only geometry.
    add_pin_conductors(top)
    lib.write_gds(str(work / "routed.gds"), timestamp=columns.FIXED_GDS_TIMESTAMP)
    # No final artifact is published until every physical terminal is joined
    # to its net and every distinct named net remains isolated.
    verify(work / "routed.gds", manifest)
    check_route_drc(work / "macro_drc.rpt")
    top.add(gdstk.rectangle((0, 0), (width, height), layer=100))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    lib.write_gds(str(args.output), timestamp=columns.FIXED_GDS_TIMESTAMP)
    report = {
        "cell": top_name,
        "size_um": [width, height],
        "banks": args.banks,
        "bits": args.bits,
        "wordlines_per_half": args.wordlines,
        "tap_pitch": tap_pitch,
        "column_tiles": len(instances) - 1,
        "checked_net_partitions": len(probes),
        "routing_drc_violations": 0,
        "physical_connectivity": "PASS",
        "signoff_lvs": "not_run",
        "device_drc": "not_run",
        "characterization": "not_measured",
        "reports": str(work),
    }
    args.output.with_suffix(".physical.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(
        f"PASS: {top_name}: {len(instances) - 1} column tiles, {len(probes)} connected nets"
    )


def add_pin_conductors(cell):
    """DEF pin rectangles are conductors as well as pin-purpose markers."""
    for polygon in list(cell.polygons):
        if polygon.layer in METALS and polygon.datatype == 251:
            drawing = polygon.copy()
            drawing.datatype = 0
            cell.add(drawing)


def check_route_drc(report):
    if not report.is_file():
        raise RuntimeError(f"missing routing DRC report: {report}")
    if report.read_text().strip():
        raise RuntimeError(f"macro routing DRC violations remain: {report}")


def verify(path, manifest):
    lib = gdstk.read_gds(str(path))
    top = next(c for c in lib.cells if c.name == manifest["cell"])
    graph = MetalGraph(top)
    roots = {}
    for net, probes in manifest["nets"].items():
        components = set()
        for probe in probes:
            found = graph.at(probe["layer"], probe["point"])
            if len(found) != 1:
                raise RuntimeError(f"{net}: missing/ambiguous terminal {probe}")
            components.update(found)
        if len(components) != 1:
            raise RuntimeError(
                f"{net}: {len(components)} disconnected physical components"
            )
        root = components.pop()
        if root in roots:
            raise RuntimeError(f"short: {net} / {roots[root]}")
        roots[root] = net
    labels = {label.text: label for label in top.labels}
    for net in manifest["external"]:
        if net not in labels or roots.get(graph.label_root(labels[net])) != net:
            raise RuntimeError(f"external pin {net} missing or disconnected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wordlines", type=int, required=True)
    parser.add_argument("--bits", type=int, required=True)
    parser.add_argument("--banks", type=int, default=1)
    parser.add_argument("--controller", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--openroad", default="openroad")
    parser.add_argument("--route-iterations", type=int, default=64)
    parser.add_argument("--route-timeout", type=int, default=900)
    parser.add_argument("--channel-width", type=float, default=2.0)
    run(parser.parse_args())


if __name__ == "__main__":
    main()
