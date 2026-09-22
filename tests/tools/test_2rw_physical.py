"""Fast physical compiler contract tests; no router or simulator required."""

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

import gdstk

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from asap7_connectivity import MetalGraph
from compile_asap7_2rw import abstract, build_leaf, check_route_drc, leaf_net, verify
from mapped_verilog_to_spice import convert


class PhysicalMacroTests(unittest.TestCase):
    def test_controller_access_uses_pin_landing_not_internal_upper_metal(self):
        cell = gdstk.Cell("ctrl")
        cell.add(
            gdstk.rectangle((0, 0), (0.1, 0.1), layer=50),
            gdstk.rectangle((0, 0), (0.1, 0.1), layer=60),
            gdstk.rectangle((0.02, 0.02), (0.04, 0.04), layer=55),
            gdstk.rectangle((0, 0), (0.1, 0.1), layer=50, datatype=251),
            gdstk.rectangle((0.2, 0), (0.3, 0.1), layer=19),
            gdstk.rectangle((0.4, 0), (0.5, 0.1), layer=19),
        )
        label = gdstk.Label("ysel_A[0]", (0.05, 0.05), layer=50, texttype=251)
        cell.add(
            label,
            gdstk.Label("vdd", (0.25, 0.05), layer=19),
            gdstk.Label("vss", (0.45, 0.05), layer=19),
        )
        _, pins, lef, _ = abstract(cell, "dp_controller", [label])
        self.assertEqual(
            next(p for p in pins.values() if p["net"] == label.text)["layer"], 50
        )
        self.assertIn("LAYER M6", lef.split("  OBS")[1])

    def test_abutting_rectangles_survive_hierarchy_roundoff(self):
        cell = gdstk.Cell("test")
        cell.add(
            gdstk.rectangle((0, 0), (0.1, 0.594), layer=30),
            gdstk.rectangle((0, 0.5940000000000001), (0.1, 1.188), layer=30),
            gdstk.rectangle((0, 1.189), (0.1, 1.5), layer=30),
        )
        graph = MetalGraph(cell)
        self.assertEqual(graph.at(30, (0.05, 0.3)), graph.at(30, (0.05, 0.9)))
        self.assertNotEqual(graph.at(30, (0.05, 0.9)), graph.at(30, (0.05, 1.3)))

    def test_mapped_spice_uses_cdl_order_and_expands_bus(self):
        design = {
            "modules": {
                "ctrl_decode": {
                    "ports": {"A": {"bits": [2, 3]}, "Q": {"bits": [4]}},
                    "netnames": {},
                    "cells": {
                        "inst": {
                            "type": "gate",
                            "connections": {"Y": [4], "RESETN": [3], "A": [2]},
                        }
                    },
                }
            }
        }
        cdl = ".SUBCKT gate RESETn A VDD VSS Y\n.ENDS gate\n"
        spice = convert(design, cdl)
        self.assertIn(".SUBCKT ctrl_decode A[0] A[1] Q VDD VSS", spice)
        self.assertIn("X0 A[1] A[0] VDD VSS Q gate", spice)
        with self.assertRaisesRegex(RuntimeError, "no CDL"):
            convert(design, "")
        del design["modules"]["ctrl_decode"]["cells"]["inst"]["connections"]["A"]
        with self.assertRaisesRegex(RuntimeError, "missing/non-scalar pin A"):
            convert(design, cdl)

    def test_vias_only_join_adjacent_layers(self):
        cell = gdstk.Cell("test")
        for layer in (19, 20, 30):
            cell.add(gdstk.rectangle((0, 0), (0.1, 0.1), layer=layer))
        cell.add(gdstk.rectangle((0.02, 0.02), (0.04, 0.04), layer=21))
        graph = MetalGraph(cell)
        self.assertEqual(graph.at(19, (0.03, 0.03)), graph.at(20, (0.03, 0.03)))
        self.assertNotEqual(graph.at(19, (0.03, 0.03)), graph.at(30, (0.03, 0.03)))

    def test_mapped_spice_preserves_single_bit_vectors_and_supply_ports(self):
        design = {
            "modules": {
                "ctrl_decode": {
                    "ports": {
                        "A": {"bits": [2]},
                        "Y": {"bits": [3]},
                        "VDD": {"bits": [4]},
                        "VSS": {"bits": [5]},
                    },
                    "netnames": {"A": {"attributes": {"single_bit_vector": "1"}}},
                    "cells": {
                        "inst": {
                            "type": "gate",
                            "connections": {"A": [2], "Y": [3], "VDD": [4], "VSS": [5]},
                        }
                    },
                }
            }
        }
        spice = convert(design, ".SUBCKT gate A VDD VSS Y\n.ENDS gate\n")
        self.assertIn(".SUBCKT ctrl_decode A[0] Y VDD VSS\n", spice)
        self.assertIn("X0 A[0] VDD VSS Y gate", spice)

    def test_topology_verifier_rejects_opens_and_shorts(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "test.gds"
            manifest = {
                "cell": "test",
                "external": [],
                "nets": {
                    "a": [{"layer": 19, "point": [0.01, 0.01]}],
                    "b": [{"layer": 19, "point": [0.21, 0.01]}],
                },
            }
            lib = gdstk.Library()
            cell = lib.new_cell("test")
            cell.add(
                gdstk.rectangle((0, 0), (0.1, 0.1), layer=19),
                gdstk.rectangle((0.2, 0), (0.3, 0.1), layer=19),
            )
            lib.write_gds(str(path))
            verify(path, manifest)
            opened = json.loads(json.dumps(manifest))
            opened["nets"]["a"].extend(opened["nets"].pop("b"))
            with self.assertRaisesRegex(RuntimeError, "disconnected"):
                verify(path, opened)
            cell.add(gdstk.rectangle((0.09, 0), (0.21, 0.1), layer=19))
            lib.write_gds(str(path))
            with self.assertRaisesRegex(RuntimeError, "short"):
                verify(path, manifest)

    def test_route_report_fails_closed(self):
        with tempfile.TemporaryDirectory() as scratch:
            report = Path(scratch) / "drc.rpt"
            with self.assertRaisesRegex(RuntimeError, "missing"):
                check_route_drc(report)
            report.write_text("Short M3\n")
            with self.assertRaisesRegex(RuntimeError, "violations"):
                check_route_drc(report)
            report.write_text("")
            check_route_drc(report)

    def test_both_ports_and_banks_map_onto_one_unsplit_array(self):
        for port in "AB":
            self.assertEqual(leaf_net(f"D{port}", 1, 3, 4), f"D_{port}[3]")
            self.assertEqual(leaf_net(f"wrena{port}", 1, 3, 4), f"wrena_{port}[1]")
            self.assertEqual(leaf_net(f"wrenan{port}", 1, 3, 4), f"wrenan_{port}[1]")
            self.assertEqual(leaf_net(f"blprechn{port}", 1, 3, 4), f"blprechn_{port}[1]")
            # NUM_WL = 4: a bank's array has eight wordlines, the upper four
            # being the ones the address bit above the column select picks.
            self.assertEqual(leaf_net(f"WL{port}[2]", 1, 3, 4), f"wl_{port}[10]")
            self.assertEqual(leaf_net(f"WL{port}[7]", 0, 3, 4), f"wl_{port}[7]")
            self.assertEqual(leaf_net(f"ysel{port}[2]", 1, 3, 4), f"ysel_{port}[6]")
            self.assertEqual(leaf_net(f"yseln{port}[2]", 1, 3, 4), f"yseln_{port}[6]")
            for split_era in (f"WLT{port}[2]", f"yselt{port}[2]", f"blprechtn{port}"):
                with self.assertRaisesRegex(RuntimeError, "unmapped"):
                    leaf_net(split_era, 0, 0, 4)
        with self.assertRaisesRegex(RuntimeError, "unmapped"):
            leaf_net("typo", 0, 0, 2)

    def test_capped_tapped_leaf_has_isolated_named_pins(self):
        for wordlines in (2, 4, 18):
            with self.subTest(wordlines=wordlines):
                import math

                leaf = build_leaf(wordlines, math.gcd(wordlines, 16))
                labels = [
                    label
                    for label in leaf.labels
                    if label.text.lower() not in ("vdd", "vss")
                ]
                master, pins, lef, size = abstract(leaf, "column", labels)
                nets = {p["net"] for p in pins.values() if not p.get("private")}
                self.assertTrue({"DA", "DB", "QA", "QB", "vdd", "vss"} <= nets)
                self.assertTrue(any(p.get("private") for p in pins.values()))
                for port in "AB":
                    self.assertTrue(
                        {f"WL{port}[{i}]" for i in range(wordlines)} <= nets
                    )
                    self.assertTrue(
                        {f"{sel}{port}[{i}]" for sel in ("ysel", "yseln") for i in range(4)}
                        | {f"blprechn{port}"} <= nets
                    )
                self.assertFalse(
                    [n for n in nets if n.startswith(
                        ("WLTA", "WLTB", "WLBA", "WLBB", "yselt", "yselb", "blprecht", "blprechb")
                    )]
                )
                # The idle face of each wrapper (A's left, B's right) is tied
                # off through the router: each of its nine control pins is a
                # supply terminal, selects and precharge enable low, complement
                # selects high.
                graph = MetalGraph(master)
                net_of = {
                    root: p["net"]
                    for p in pins.values()
                    for root in graph.at(p["layer"], tuple(p["point"]))
                }
                idle, tied = {"A": "T", "B": "B"}, 0
                for label in master.get_labels():
                    pin = re.fullmatch(r"(YSEL|BLPRECH)([TB])(N?)_([AB])(\[\d\])?", label.text)
                    if not pin or pin[2] != idle[pin[4]]:
                        continue
                    want = "vdd" if pin[1] == "YSEL" and pin[3] else "vss"
                    self.assertEqual(net_of[graph.label_root(label)], want, label.text)
                    tied += 1
                self.assertEqual(tied, 18)

                def count(cell):
                    return (
                        1
                        if cell.name == "sram_cell_8t"
                        else sum(count(ref.cell) for ref in cell.references)
                    )

                self.assertEqual(count(master), 4 * wordlines)
                self.assertIn("OBS", lef)
                self.assertGreater(size[1], 6 * 0.594)


if __name__ == "__main__":
    unittest.main()
