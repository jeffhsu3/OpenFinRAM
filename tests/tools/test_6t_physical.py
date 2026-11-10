"""Fast physical contract tests of the single-port 6T tile (--bitcell 6t); no router or simulator."""

from pathlib import Path
import re
import sys
import unittest

import gdstk

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from chipforge_asap7.verification.connectivity import MetalGraph
from compile_asap7_2rw import abstract, build_leaf_6t, build_wordline_strips, io_block_grids, leaf_net, strip_net, supply, wordline_tracks
import compile_asap7_2rw as compile_2rw
import generate_asap7_6t_iocolumn as columns6

ROWS = 8  # wordlines of the array (NUM_WL 4)


def boundary(cell):
    return columns6.boundary_box(cell)


class SixTTileTests(unittest.TestCase):
    def test_wordlines_sit_on_the_driver_slices_pitch(self):
        tile = build_leaf_6t(ROWS, bottom=True, top=True)
        tracks = wordline_tracks(tile)
        self.assertEqual(sorted(tracks), [("A", i) for i in range(ROWS)])
        xs = [tracks[("A", i)] for i in range(ROWS)]
        self.assertTrue(all(abs(b - a - 0.108) < 1e-6 for a, b in zip(xs, xs[1:])))
        # Over the bitcell's own M3 wordline stripe, not the label it carries.
        graph = MetalGraph(tile)
        for x in xs:
            self.assertEqual(len(graph.at(30, (x, 0.135))), 1)

    def test_tiles_abut_on_whole_nanometres(self):
        heights = {}
        for bottom in (False, True):
            for top in (False, True):
                _, y0, _, y1 = boundary(build_leaf_6t(ROWS, bottom=bottom, top=top))
                heights[(bottom, top)] = round(y1 - y0, 6)
                for edge in (y0, y1):
                    self.assertAlmostEqual(edge * 1000, round(edge * 1000), places=6)
        self.assertAlmostEqual(heights[(False, False)], 4 * 0.27)
        # A dummy row (283.5 nm) and three half fin pitches past it at each stack end.
        self.assertAlmostEqual(heights[(True, False)], 4 * 0.27 + 0.324)
        self.assertAlmostEqual(heights[(True, True)], 4 * 0.27 + 2 * 0.324)

    def test_the_tiles_pins_map_to_port_a_nets(self):
        tile = build_leaf_6t(ROWS, bottom=False, top=False)
        names = {label.text for label in tile.labels if not supply(label.text)}
        self.assertEqual(
            {n for n in names if not n.startswith("WLA[")},
            {"DA", "QA", "wrenaA", "wrenanA", "oe_outA", "oeb_outA", "blprechnA", "sae_A",
             *(f"ysel{s}A[{r}]" for s in ("", "n") for r in range(4))},
        )
        for name in names:
            net = leaf_net(name, bank=0, bit=1, wordlines=ROWS // 2, bits=4)
            self.assertNotRegex(net, r"_B\b|_B\[")
        _, pins, _, _ = abstract(tile, "dp_column_noend", [l for l in tile.labels if not supply(l.text)])
        self.assertTrue({"vdd", "vss"} <= {p["net"] for p in pins.values()})

    def test_one_strip_a_side_lines_up_with_the_tile(self):
        tile = build_leaf_6t(ROWS, bottom=True, top=True)
        lib = gdstk.Library(unit=1e-6, precision=1e-10)
        pairs, _ = build_wordline_strips(lib, tile, ROWS // 2, 4, ports=("A",))
        for half, pair in pairs.items():
            labels = [label.text for label in pair.labels]
            self.assertEqual(sum(bool(re.fullmatch(r"WL_A\[\d+\]", t)) for t in labels), ROWS, half)
            self.assertFalse(any(t.startswith(("WL_B", "SEL_B", "B_B")) for t in labels), half)

    def test_each_predecode_input_is_one_rail_across_the_strip(self):
        # Every slice's B<j> joined in the strip: one pin per sel_lo, not 2 x 16
        # interleaved runs the router could not all reach (x64x8x1 --segment-bits 2).
        tile = build_leaf_6t(4 * ROWS, bottom=True, top=True)
        for bits in (4, 32):  # the c4 slice and the taller c64 one
            lib = gdstk.Library(unit=1e-6, precision=1e-10)
            pairs, _ = build_wordline_strips(lib, tile, 2 * ROWS, bits, ports=("A",))
            pair = pairs["lo"]
            graph = MetalGraph(pair)
            roots = {}
            for label in pair.labels:
                if label.text.startswith("B_A["):
                    roots.setdefault(label.text, set()).add(graph.label_root(label))
            self.assertEqual(len(roots), 4, bits)
            self.assertTrue(all(len(r) == 1 for r in roots.values()), (bits, roots))
            self.assertEqual(len(set().union(*roots.values())), 4, bits)  # and never shorted
        # The rails are the router's M4: whole nanometres in every pair (the
        # strips themselves sit 13.5 nm in), or it lands 23.5 nm from them.
        lib = gdstk.Library(unit=1e-6, precision=1e-10)
        pairs, _ = build_wordline_strips(lib, tile, 2 * ROWS, 16, ports=("A",), segment_bits=4)
        for half, pair in pairs.items():
            edges = [v for p in pair.get_polygons(depth=None, layer=40, datatype=0) for v in p.points.flatten()]
            self.assertTrue(edges, half)
            self.assertTrue(all(abs(v * 1000 - round(v * 1000)) < 1e-3 for v in edges), half)

    def test_select_buffers_redrive_each_line_a_group_of_slices_at_a_time(self):
        # 68 wordlines: 17 slices, two groups (9 and 8), a BUFx8 a line in each.
        rows = 68
        tile = build_leaf_6t(rows, bottom=True, top=True)
        lib = gdstk.Library(unit=1e-6, precision=1e-10)
        plain, _ = build_wordline_strips(lib, tile, rows // 2, 8, ports=("A",), segment_bits=2, rail_on_grid=0.0)
        lib = gdstk.Library(unit=1e-6, precision=1e-10)
        pairs, load = build_wordline_strips(lib, tile, rows // 2, 8, ports=("A",), segment_bits=2,
                                            rail_on_grid=0.0, select_buffers=True)  # fmt: skip
        self.assertEqual(compile_2rw.select_buffer_groups(rows // 4), [9, 8])
        self.assertEqual(compile_2rw.select_buffer_cell(rows // 4), "BUFx8_ASAP7_75t_R")
        self.assertEqual(compile_2rw.select_buffer_cell(4), "BUFx4_ASAP7_75t_R")
        with self.assertRaises(RuntimeError):
            compile_2rw.select_buffer_cell(3)
        # A strip is the buffers' row (and its gap) taller; a mid pair holds two.
        for half, strips in (("lo", 1), ("hi", 1), ("mid", 2)):
            grown = boundary(pairs[half])[3] - boundary(plain[half])[3]
            self.assertAlmostEqual(grown, strips * compile_2rw.SELECT_ROW, places=6, msg=half)

        strip = next(cell for cell in lib.cells if cell.name == f"wl_strip_c{load}_x{rows // 4}")
        graph = MetalGraph(strip)

        def root(layer, x, y):
            (index,) = graph.at(layer, (x, y))
            return graph.root(index)

        lines = {int(label.text[2]): graph.label_root(label) for label in strip.labels if label.text.startswith("B[")}
        self.assertEqual(len(set(lines.values())), 4)  # four pins, four conductors
        slice_cell = next(cell for cell in lib.cells if cell.name == f"wl_slice_c{load}")
        pins = {label.text: label.origin for label in slice_cell.labels}
        starts = sorted(ref.origin[0] for ref in strip.references if ref.cell is slice_cell)
        self.assertEqual(len(starts), rows // 4)
        width, bars = compile_2rw._std_cell_pins("BUFx8_ASAP7_75t_R")

        def placed(ref):
            """A row cell's left edge and whether it is turned about its middle (A and Y change sides)."""
            turned = abs(ref.rotation) > 1
            return ref.origin[0] - (compile_2rw._std_cell_pins(ref.cell.name)[0] if turned else 0.0), turned

        buffers = sorted(placed(ref) for ref in strip.references if ref.cell.name == "BUFx8_ASAP7_75t_R")
        self.assertEqual(len(buffers), 4 * 2)
        y_bar = -compile_2rw.SELECT_ROW_GAP - compile_2rw.STD_ROW / 2
        driven, outputs = {}, []
        for x, turned in buffers:
            x_a, x_y = (x + width - bars[pin][0] if turned else x + bars[pin][0] for pin in "AY")
            (line,) = [j for j, r in lines.items() if r == root(19, x_a, y_bar)]
            out = root(19, x_y, y_bar)
            outputs.append(out)
            self.assertNotIn(out, lines.values())
            slices = tuple(k for k, x0 in enumerate(starts)
                           if root(20, x0 + pins[f"B{line}"][0], pins[f"B{line}"][1]) == out)  # fmt: skip
            driven.setdefault(line, []).append(slices)
        # Every line: one buffer on the first nine slices' inputs, one on the other eight's.
        for line in range(4):
            self.assertEqual(sorted(driven[line]), [tuple(range(9)), tuple(range(9, 17))], line)
        # Every slice's select pin comes through the row on its own track, to
        # the side its driver is on (the buffers' stacks keep off it).
        selects = {int(label.text[4:-1]): graph.label_root(label) for label in strip.labels if label.text.startswith("SEL[")}
        self.assertEqual(len(set(selects.values())), rows // 4)
        self.assertFalse(set(selects.values()) & (set(lines.values()) | set(outputs)))
        y_far = -compile_2rw.SELECT_ROW + compile_2rw.SELECT_PIN_END + 0.005
        for k, x0 in enumerate(starts):
            self.assertEqual(root(30, x0 + pins["SEL"][0], y_far), selects[k], k)
        # The row is whole (a cell on every site) and its rails are their own conductors.
        cells = sorted((placed(ref)[0], ref.cell.name) for ref in strip.references if "ASAP7" in ref.cell.name)
        x_left = starts[0] - 0.108
        cursor = x_left
        for x, name in cells:
            self.assertAlmostEqual(x, cursor, places=6, msg=name)
            cursor = x + compile_2rw._std_cell_pins(name)[0]
        self.assertAlmostEqual(cursor, starts[-1] + 0.432 + 0.108, places=6)
        self.assertEqual(sum(name.startswith("TAPCELL") for _, name in cells), 3)  # each end and each group
        vss, row_vss, row_vdd = (root(19, starts[0] + 0.05, y) for y in (0.0, -compile_2rw.SELECT_ROW_GAP, -compile_2rw.SELECT_ROW))
        self.assertEqual(len({vss, row_vss, row_vdd}), 3)
        # A pair's B pins are the row's rails, on the router's M4.
        for half, pair in pairs.items():
            pins_b = [label for label in pair.labels if label.text.startswith(("B_A[", "B_U[", "B_D["))]
            self.assertEqual(len(pins_b), 8 if half == "mid" else 4, half)
            self.assertTrue(all(label.layer == 40 for label in pins_b), half)
            _, terminals, lef, _ = abstract(pair, f"dp_wl_strips_{half}", [label for label in pair.labels if not supply(label.text)])
            nets = [t["net"] for t in terminals.values()]
            self.assertEqual(sum(net.startswith("B_") for net in nets), len(pins_b), half)
            self.assertTrue({"vdd", "vss"} <= set(nets), half)

    def test_a_precharge_buffer_redrives_the_line_for_the_strips_segment(self):
        # 64 wordlines in 2-bit segments at 4:1: 16 slices a strip, 5 fF of precharge gates a segment.
        rows = 64
        cell = compile_2rw.precharge_buffer_cell(2, 4)
        self.assertEqual(cell, "BUFx8_ASAP7_75t_R")
        self.assertEqual(compile_2rw.precharge_buffer_cell(16, 8), "BUFx12f_ASAP7_75t_R")  # 81 fF
        self.assertEqual(compile_2rw.precharge_buffer_cell(32, 8), "BUFx24_ASAP7_75t_R")  # 161 fF
        with self.assertRaises(RuntimeError):
            compile_2rw.precharge_buffer_cell(32, 16)  # 320 fF: more segments
        # The row has room past a group of nine slices' select buffers.
        self.assertTrue(compile_2rw.precharge_buffer_fits(16, cell))
        self.assertTrue(compile_2rw.precharge_buffer_fits(9, cell))
        self.assertFalse(compile_2rw.precharge_buffer_fits(8, cell))
        self.assertFalse(compile_2rw.precharge_buffer_fits(17, cell))  # groups of 9 and 8
        self.assertFalse(compile_2rw.precharge_buffer_fits(10, "BUFx24_ASAP7_75t_R"))

        tile = build_leaf_6t(rows, bottom=True, top=True)
        lib = gdstk.Library(unit=1e-6, precision=1e-10)
        pairs, load = build_wordline_strips(lib, tile, rows // 2, 8, ports=("A",), segment_bits=2, rail_on_grid=0.0,
                                            select_buffers=True, precharge_buffer=cell)  # fmt: skip
        strip = next(c for c in lib.cells if c.name == f"wl_strip_c{load}_x{rows // 4}")
        graph = MetalGraph(strip)
        roots = {label.text: graph.label_root(label) for label in strip.labels}
        self.assertEqual(len(set(roots.values())), len(roots))  # every pin its own conductor
        # The strip's fifth BUFx8, the one nearest the IO end: the line on its A, the segment's copy on its Y.
        width, bars = compile_2rw._std_cell_pins(cell)
        buffers = sorted((ref.origin[0] - (width if abs(ref.rotation) > 1 else 0.0), abs(ref.rotation) > 1)
                         for ref in strip.references if ref.cell.name == cell)  # fmt: skip
        self.assertEqual(len(buffers), 4 + 1)
        x, turned = buffers[-1]
        y_bar = -compile_2rw.SELECT_ROW_GAP - compile_2rw.STD_ROW / 2
        for pin, name in (("A", "PCH"), ("Y", "PCHL")):
            (index,) = graph.at(19, (x + width - bars[pin][0] if turned else x + bars[pin][0], y_bar))
            self.assertEqual(graph.root(index), roots[name], name)
        # Both pins are the router's M4, between the lines' rails and the slices.
        labels = {label.text: label for label in strip.labels}
        rails = sorted(labels[f"B[{j}]"].origin[1] for j in range(4))
        for name in ("PCH", "PCHL"):
            self.assertEqual(labels[name].layer, 40)
            self.assertGreater(labels[name].origin[1], rails[-1] + 0.047)
            self.assertLess(labels[name].origin[1], -0.040)
        for half, pair in pairs.items():
            names = sorted(label.text for label in pair.labels if label.text.startswith("PCH"))
            self.assertEqual(names, ["PCHL_D", "PCHL_U", "PCH_D", "PCH_U"] if half == "mid" else ["PCHL_A", "PCH_A"])
            _, terminals, _, _ = abstract(pair, f"dp_wl_strips_{half}", [label for label in pair.labels if not supply(label.text)])
            nets = [t["net"] for t in terminals.values()]
            for name in names:
                self.assertEqual(nets.count(name), 1, (half, name))
        # Every strip takes the bank's line; its copy is its segment's, as the wordlines are.
        self.assertEqual(strip_net("PCH_A", 0, "lo", rows // 2, segment=3), "blprechn_A[0]")
        self.assertEqual(strip_net("PCHL_A", 0, "lo", rows // 2, segment=3), "blprechn_A_lo_s3[0]")
        self.assertEqual(strip_net("PCHL_A", 1, "hi", rows // 2), "blprechn_A_hi[1]")
        self.assertEqual(strip_net("PCH_U", 1, "hi", rows // 2, below=0, above=1), "blprechn_A[1]")
        self.assertEqual(strip_net("PCHL_D", 0, "hi", rows // 2, below=0, above=1), "blprechn_A_hi_s0[0]")
        self.assertEqual(strip_net("PCHL_U", 0, "hi", rows // 2, below=0, above=1), "blprechn_A_hi_s1[0]")
        self.assertEqual(leaf_net("blprechnA", 0, 3, rows // 2, 16, segment=1), "blprechn_A[0]")
        self.assertEqual(leaf_net("blprechnA", 0, 3, rows // 2, 16, segment=1, precharge_buffers=True), "blprechn_A_lo_s1[0]")
        self.assertEqual(leaf_net("blprechnA", 1, 12, rows // 2, 16, precharge_buffers=True), "blprechn_A_hi[1]")
        self.assertEqual(leaf_net("sae_A", 0, 3, rows // 2, 16, segment=1, precharge_buffers=True), "sae_A[0]")
        # 68 wordlines: the second group's eight slices leave no room for it.
        with self.assertRaises(RuntimeError):
            build_wordline_strips(gdstk.Library(unit=1e-6, precision=1e-10), build_leaf_6t(68, bottom=True, top=True),
                                  34, 8, ports=("A",), segment_bits=2, rail_on_grid=0.0,
                                  select_buffers=True, precharge_buffer=cell)  # fmt: skip

    def test_a_mid_pair_drives_the_segments_either_side(self):
        tile = build_leaf_6t(ROWS, bottom=True, top=True)
        lib = gdstk.Library(unit=1e-6, precision=1e-10)
        pairs, load = build_wordline_strips(lib, tile, ROWS // 2, 16, ports=("A",),
                                            segment_bits=2)  # fmt: skip
        self.assertIn("mid", pairs)
        mid = pairs["mid"]
        _, y0, _, y1 = boundary(mid)
        down = {l.text: l.origin[1] for l in mid.labels if l.text.startswith("WL_D[")}
        up = {l.text: l.origin[1] for l in mid.labels if l.text.startswith("WL_U[")}
        self.assertEqual((len(down), len(up)), (ROWS, ROWS))
        self.assertTrue(all(abs(y - y0) < 0.01 for y in down.values()))
        self.assertTrue(all(abs(y - y1) < 0.01 for y in up.values()))
        # Sized for a segment (2 bits, 8 cells), not the stack (8 bits).
        self.assertEqual(load, 8)
        # D is the segment below, U the one above; selects are port A's.
        self.assertEqual(strip_net("WL_D[3]", 0, "lo", ROWS // 2, below=1, above=2), "wl_A_lo_s1[3]")
        self.assertEqual(strip_net("WL_U[3]", 1, "hi", ROWS // 2, below=0, above=1), f"wl_A_hi_s1[{ROWS + 3}]")
        self.assertEqual(strip_net("SEL_D[1]", 1, "lo", ROWS // 2), f"sel_hi_A[{ROWS // 4 + 1}]")
        self.assertEqual(strip_net("B_U[2]", 0, "lo", ROWS // 2), "sel_lo_A[2]")
        self.assertEqual(leaf_net("WLA[5]", 0, 3, ROWS // 2, 16, segment=1), "wl_A_lo_s1[5]")

    def test_deeper_muxes_stack_more_rows_and_selects(self):
        for mux in (8, 16):
            tile = build_leaf_6t(ROWS, bottom=False, top=False, mux=mux)
            _, y0, _, y1 = boundary(tile)
            self.assertAlmostEqual(y1 - y0, mux * 0.27)
            names = {label.text for label in tile.labels}
            if mux == 8:
                # Each leaf makes YSEL from YSELN: only the complements are pins.
                self.assertEqual({n for n in names if n.startswith("yselnA[")}, {f"yselnA[{r}]" for r in range(mux)})
                self.assertFalse({n for n in names if n.startswith("yselA[")})
                self.assertEqual(leaf_net(f"yselnA[{mux - 1}]", 1, 0, ROWS // 2, 4, mux=mux), f"yseln_A[{2 * mux - 1}]")
            else:
                # Two predecoded groups on the select bus: 4 + mux/4 pins, no complements.
                self.assertEqual({n for n in names if n.startswith("yselA[")}, {f"yselA[{r}]" for r in range(4 + mux // 4)})
                self.assertFalse({n for n in names if n.startswith("yselnA[")})
                # Bank 1's bus starts after bank 0's 4 + mux/4 lines.
                self.assertEqual(leaf_net("yselA[7]", 1, 0, ROWS // 2, 4, mux=mux), f"ysel_A[{4 + mux // 4 + 7}]")

    def test_m5_supply_stripes_run_the_tile_on_its_straps(self):
        # 0.12 um M5 stripes the tile's height (so abutted tiles make them the
        # stack's), one beside each of the IO block's straps, each one conductor
        # with that net's straps.
        for mux, (bottom, top) in ((4, (True, False)), (4, (False, False)), (8, (False, True)), (16, (False, False))):
            tile = build_leaf_6t(ROWS, bottom=bottom, top=top, mux=mux)
            _, y0, _, y1 = boundary(tile)
            graph = MetalGraph(tile)
            supply = {}
            for label in tile.labels:
                if label.text.upper() in ("VDD", "VSS"):
                    supply.setdefault(label.text.upper(), set()).add(graph.label_root(label))
            stripes = {"VDD": 0, "VSS": 0}
            for i, poly in enumerate(graph.polygons):
                if poly.layer != 50:
                    continue
                (a, b), (c, d) = poly.bounding_box()
                self.assertAlmostEqual(c - a, 0.12, places=6)
                self.assertAlmostEqual(b, y0, places=6)
                self.assertAlmostEqual(d, y1, places=6)
                net = next(n for n, roots in supply.items() if graph.root(i) in roots)
                stripes[net] += 1
            # One stripe a strap of the IO block, and at least one a net.
            spec = columns6.io_spec(columns6.load_source()[columns6.BITCELL], mux)
            io = columns6.build_io(gdstk.Library(), columns6.build_io_block(spec)[0], spec)
            straps = columns6.io_supply_straps(io)
            expected = {net: sum(1 for n, _ in straps if n == net) for net in ("VDD", "VSS")}
            self.assertEqual(stripes, expected, (mux, bottom, top))
            self.assertTrue(all(expected.values()), expected)

    def test_the_io_blocks_bitlines_meet_the_rows(self):
        cells = columns6.load_source()
        spec = columns6.io_spec(cells[columns6.BITCELL])
        # The 6T cell's M2 bitline bars, from each row's bottom.
        self.assertEqual(spec.bitline_entry, (186.5, 83.5))
        self.assertEqual(spec.row_pitch, 270)
        self.assertEqual(spec.height, 4 * 270)


class PhasedSixTTileTests(unittest.TestCase):
    """A tile drawn for its place on the DRM's M4 grid (`m4_grid`), as the assembler asks."""

    def test_every_wordline_count_accepts_the_blocks_grid(self):
        # 32 and 256 wordlines put the block's x a hair under a multiple of
        # 24 nm; a bare float modulo read that as 24, not 0, and refused the
        # block `io_block_grids` had drawn for phase 0.
        for wordlines in (16, 32, 256):
            for grid in ((0.0, 12.0), (12.0, 36.0)):
                plain = build_leaf_6t(wordlines, bottom=True, top=True)
                tile = build_leaf_6t(wordlines, bottom=True, top=True, m4_grid=grid,
                                     m4_grids=io_block_grids(plain, grid))  # fmt: skip
                pads = [p for p in tile.polygons if p.layer == columns6.M4]
                self.assertTrue(pads, f"{wordlines} wordlines: no M4 joining pads")
                for pad in pads:
                    (x0, _), (x1, _) = pad.bounding_box()
                    for x in (x0, x1):
                        off = (1000 * x - grid[0]) % 24
                        self.assertLess(min(off, 24 - off), 1e-3,
                                        f"{wordlines} wordlines: pad edge {x} off the grid")


if __name__ == "__main__":
    unittest.main(verbosity=2)
