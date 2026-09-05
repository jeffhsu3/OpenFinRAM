"""Polygon connectivity for ASAP7 interconnect (GDS coordinates in microns).

This checks actual conductors and adjacent via layers, not pin-label proximity.
It intentionally excludes device channels: two nets separated by a transistor
must remain separate in the interconnect graph.
"""

from __future__ import annotations

from collections import defaultdict
import math
import gdstk

METALS = (19, 20, 30, 40, 50, 60, 70, 80, 90)
VIAS = (21, 25, 35, 45, 55, 65, 75, 85)
ADJACENT = {via: (METALS[i], METALS[i + 1]) for i, via in enumerate(VIAS)}


class MetalGraph:
    def __init__(self, cell: gdstk.Cell):
        self.polygons = [
            p
            for p in cell.get_polygons()
            if p.datatype == 0 and p.layer in (*METALS, *VIAS)
        ]
        self.parent = list(range(len(self.polygons)))
        self.buckets = defaultdict(list)
        self.boxes = []
        for i, p in enumerate(self.polygons):
            box = p.bounding_box()
            self.boxes.append(box)
            candidates = set()
            layers = [p.layer]
            layers += list(ADJACENT.get(p.layer, ()))
            layers += [v for v, pair in ADJACENT.items() if p.layer in pair]
            for x, y in self.tiles(box):
                for layer in layers:
                    candidates.update(self.buckets[layer, x, y])
            for j in candidates:
                q = self.polygons[j]
                a, b = box, self.boxes[j]
                if (
                    a[1][0] + 1e-7 < b[0][0]
                    or b[1][0] + 1e-7 < a[0][0]
                    or a[1][1] + 1e-7 < b[0][1]
                    or b[1][1] + 1e-7 < a[0][1]
                ):
                    continue
                # Tiny expansion includes legal edge abutment in the same
                # layer. Vias must overlap both adjacent conductor layers.
                test = (
                    gdstk.offset([p], 1e-6, precision=1e-7)
                    if p.layer == q.layer
                    else [p]
                )
                if gdstk.boolean(test, [q], "and", precision=1e-7):
                    self.parent[self.root(i)] = self.root(j)
            for x, y in self.tiles(box):
                self.buckets[p.layer, x, y].append(i)

    @staticmethod
    def tiles(box):
        for x in range(
            math.floor((box[0][0] - 1e-7) / 0.25),
            math.floor((box[1][0] + 1e-7) / 0.25) + 1,
        ):
            for y in range(
                math.floor((box[0][1] - 1e-7) / 0.25),
                math.floor((box[1][1] + 1e-7) / 0.25) + 1,
            ):
                yield x, y

    def root(self, i):
        while self.parent[i] != i:
            self.parent[i] = self.parent[self.parent[i]]
            i = self.parent[i]
        return i

    def at(self, layer, point):
        x, y = (math.floor(v / 0.25) for v in point)
        return {
            self.root(i)
            for i in self.buckets[layer, x, y]
            if gdstk.inside([point], [self.polygons[i]])[0]
        }

    def label_root(self, label):
        roots = self.at(label.layer, label.origin)
        if len(roots) != 1:
            raise RuntimeError(
                f"{label.text}: expected one conductor at {label.origin} "
                f"on layer {label.layer}, found {len(roots)}"
            )
        return next(iter(roots))
