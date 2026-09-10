"""8T frame geometry, asymmetric mirrors, and parameterized placement checks."""

import argparse
import math
from pathlib import Path
import sys
import tempfile
import unittest

import gdstk

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import generate_asap7_8t_bitcell as edges  # noqa: E402
import generate_asap7_wordline_arrays as arrays  # noqa: E402
from compile_asap7_2rw import build_leaf  # noqa: E402


class EdgeFrameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bitcell = gdstk.read_gds(str(REPO / "tech/gds/sram_cell_8t.gds")).cells[0]
        cls.library = edges.build_edge_library(cls.bitcell)
        cls.cells = {cell.name: cell for cell in cls.library.cells}
        # The tap library also holds strapcell_sram_8t, so select by name
        # rather than by position.
        cls.tap = {
            cell.name: cell
            for cell in gdstk.read_gds(
                str(REPO / "tech/gds/sram_cell_8t_tap.gds")
            ).cells
        }["tapcell_sram_8t"]

    def assert_same_geometry(self, actual, expected):
        specs = {
            (p.layer, p.datatype)
            for p in actual.get_polygons() + expected.get_polygons()
        }
        for layer, datatype in specs:
            self.assertFalse(
                gdstk.boolean(
                    actual.get_polygons(layer=layer, datatype=datatype),
                    expected.get_polygons(layer=layer, datatype=datatype),
                    "xor",
                    precision=1e-7,
                ),
                f"geometry differs on {layer}/{datatype}",
            )

    def test_asymmetric_bands_and_pins_mirror_about_boundary(self):
        canonical = gdstk.Cell("asymmetric")
        edges.rect(canonical, (2, -3, 4, 7), edges.BOUNDARY)
        edges.rect(canonical, (1.5, -2, 3.25, 0), edges.WELL)
        edges.rect(canonical, (2.25, 1, 4.75, 5), edges.NSELECT)
        canonical.add(gdstk.Label("VDD", (2.5, -1), layer=20, texttype=251))
        for mx, my in edges.ORIENTATIONS:
            with self.subTest(mx=mx, my=my):
                cell = edges.oriented_cell(canonical, "oriented", mx, my)
                self.assertFalse(cell.references)
                self.assertEqual(edges.edge_boundary(cell), (2, -3, 4, 7))
                self.assertEqual(
                    edges.bbox(next(p for p in cell.polygons if p.layer == edges.WELL)),
                    (
                        2.75 if mx else 1.5,
                        4 if my else -2,
                        4.5 if mx else 3.25,
                        6 if my else 0,
                    ),
                )
                self.assertEqual(
                    edges.bbox(
                        next(p for p in cell.polygons if p.layer == edges.NSELECT)
                    ),
                    (
                        1.25 if mx else 2.25,
                        -1 if my else 1,
                        3.75 if mx else 4.75,
                        3 if my else 5,
                    ),
                )
                self.assertEqual(
                    tuple(cell.labels[0].origin), (3.5 if mx else 2.5, 5 if my else -1)
                )

    def test_dummy_rows_follow_real_array_slots_and_tap_parity(self):
        for count, tap_pitch in (
            (1, 0),
            (3, 0),
            (18, 0),
            (64, 0),
            (129, 0),
            (1, 1),
            (3, 1),
            (18, 6),
            (64, 16),
            (129, 3),
        ):
            lib = gdstk.Library()
            row = arrays.build_row(
                lib, self.bitcell, arrays.CONTRACTS[0], count, self.tap, tap_pitch
            )
            reference_end = gdstk.Cell("expected_end")
            for ref in row.references:
                master = self.cells[
                    "sram_cell_8t_row_cap"
                    if ref.cell_name == edges.CELL_NAME
                    else "sram_cell_8t_corner"
                ]
                reference_end.add(
                    gdstk.Reference(
                        master,
                        origin=ref.origin,
                        rotation=ref.rotation,
                        x_reflection=ref.x_reflection,
                    )
                )
            boundary = arrays.boundary_box(row)
            edges.rect(reference_end, boundary, edges.BOUNDARY)
            for mx, my in edges.ORIENTATIONS:
                with self.subTest(count=count, tap_pitch=tap_pitch, mx=mx, my=my):
                    end = edges.build_dummy_vertical_array(
                        lib, self.cells, count, tap_pitch, mirror_x=mx, mirror_y=my
                    )
                    self.assertEqual(edges.edge_boundary(end), boundary)
                    self.assertEqual(
                        len(end.references),
                        count + (count // tap_pitch if tap_pitch else 0),
                    )
                    self.assertTrue(
                        all(
                            not ref.rotation and not ref.x_reflection
                            for ref in end.references
                        )
                    )
                    expected = gdstk.Reference(
                        reference_end,
                        origin=(boundary[2] if mx else 0, boundary[3] if my else 0),
                        rotation=math.pi if mx else 0,
                        x_reflection=mx != my,
                    )
                    self.assert_same_geometry(end, expected)

    def test_invalid_counts_and_tap_pitches_fail(self):
        for count, tap_pitch in (
            (0, 0),
            (-1, 0),
            (1.5, 0),
            (True, 0),
            (3, 2),
            (4, -1),
            (4, 1.5),
        ):
            with (
                self.subTest(count=count, tap_pitch=tap_pitch),
                self.assertRaises(ValueError),
            ):
                edges.build_dummy_vertical_array(
                    gdstk.Library(), self.cells, count, tap_pitch
                )
        for value in ("0", "-1,64", "", "3,no"):
            with (
                self.subTest(value=value),
                self.assertRaises(argparse.ArgumentTypeError),
            ):
                edges.parse_row_counts(value)

    def test_leaf_masters_have_matching_spice_interfaces(self):
        spice = (REPO / "tech/spice/sram_cell_8t_edges.sp").read_text()
        ports = {
            line.split()[1]: set(line.split()[2:])
            for line in spice.splitlines()
            if line.startswith(".SUBCKT ")
        }
        for name in edges.EDGE_CELL_NAMES:
            with self.subTest(name=name):
                cell = self.cells[name]
                pins = {label.text.upper().rstrip("!") for label in cell.get_labels()}
                self.assertIn(name, ports)
                self.assertEqual(pins, ports[name])

    def test_roundtrip_nonstandard_counts_and_reject_corrupt_mirror(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "edges.gds"
            lib = edges.build_edge_library(self.bitcell, row_counts=(1, 3, 18))
            lib.write_gds(str(path))
            edges.verify_edge_gds(path, (1, 3, 18))
            corner = next(c for c in lib.cells if c.name == "sram_cell_8t_corner_v2_lr")
            next(p for p in corner.polygons if p.layer == edges.NSELECT).translate(
                0.001, 0
            )
            lib.write_gds(str(path))
            with self.assertRaisesRegex(ValueError, "canonical orientation"):
                edges.verify_edge_gds(path, (1, 3, 18))

    def test_compiler_places_all_corners_and_end_rows_without_parent_mirrors(self):
        for count in (2, 18):
            with self.subTest(count=count):
                leaf = build_leaf(count, math.gcd(count, 16))
                end_ref = next(
                    r for r in leaf.references if r.cell_name == "dp_array_end_rows"
                )
                self.assertFalse(end_ref.rotation or end_ref.x_reflection)
                corners = {
                    edges.oriented_name("sram_cell_8t_corner", mx, my)
                    for mx, my in edges.ORIENTATIONS
                }
                refs = [r for r in end_ref.cell.references if r.cell_name in corners]
                self.assertEqual({r.cell_name for r in refs}, corners)
                self.assertTrue(
                    all(not r.rotation and not r.x_reflection for r in refs)
                )
                rows = [
                    r
                    for r in end_ref.cell.references
                    if r.cell_name.startswith("dummy_vertical_array_")
                ]
                self.assertEqual(len(rows), 4)
                self.assertTrue(
                    all(not r.rotation and not r.x_reflection for r in rows)
                )
                colgrp = next(
                    r.cell for r in leaf.references if r.cell_name.startswith("colgrp_")
                )
                caps = [
                    r for r in colgrp.references if r.cell_name.startswith("col_cap_")
                ]
                self.assertEqual(len(caps), 2)
                self.assertTrue(
                    all(not r.rotation and not r.x_reflection for r in caps)
                )
                for ref in caps:
                    self.assertEqual(
                        ref.cell.references[0].cell_name, "FILLER_cgedge_8t"
                    )
                    self.assertTrue(
                        all(
                            not r.rotation and not r.x_reflection
                            for r in ref.cell.references
                        )
                    )


if __name__ == "__main__":
    unittest.main()
