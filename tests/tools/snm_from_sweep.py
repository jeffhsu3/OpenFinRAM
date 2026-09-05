#!/usr/bin/env python3
"""Static noise margin from a Xyce DC butterfly sweep.

Reads the `.prn` written by `.print dc` for tests/spice/asap7_8t_snm.sp and
reports the SNM in millivolts.

Why the square, and where the geometry comes from
-------------------------------------------------
SNM is the side of the largest axis-aligned square that fits inside a lobe of
the butterfly.  The usual recipe is "rotate the curves 45 degrees and take the
maximum separation", which is correct but leaves a factor of sqrt(2) that is
easy to get backwards.  This implementation avoids the ambiguity by working
the geometry directly:

An axis-aligned square with corners (x0, y0) and (x0+s, y0+s) has its diagonal
along the direction (1, 1), i.e. along a line ``y = x + c``.  The largest such
square inscribed in a lobe has both of those corners on the lobe's boundary --
one on each curve.  So for each 45-degree line ``y = x + c``:

    find where each curve crosses that line,
    the two crossings are the square's opposite corners,
    and the side is simply the difference in x (equivalently in y),
    because both points satisfy y = x + c.

No sqrt(2) appears at all: the side is a coordinate difference, not a distance.
SNM is then the largest such side, and because a cell has two lobes and fails
at whichever is weaker, the reported value is the smaller of the two.

`_self_test` checks this against a case whose answer is known exactly: a pair
of infinite-gain inverters, whose lobe is the square [0, VDD/2] x [VDD/2, VDD]
and whose SNM is therefore exactly VDD/2.
"""
from __future__ import annotations

import argparse
import sys


def read_prn(path: str) -> tuple[list[float], list[float], list[float]]:
    """Return (sweep, qaout, qbout) from a Xyce .print dc output.

    The format is a header naming the columns, whitespace-separated rows, and a
    trailing "End of Xyce(TM) Simulation" line.  Columns are located by name so
    a reordered .print does not silently transpose the curves.
    """
    rows: list[list[float]] = []
    header: list[str] = []
    with open(path) as handle:
        for line in handle:
            fields = line.split()
            if not fields:
                continue
            if fields[0] == "Index":
                header = fields
                continue
            if not header:
                continue
            if fields[0].lower().startswith("end"):
                break
            try:
                rows.append([float(v) for v in fields])
            except ValueError:
                # Xyce occasionally emits a footer line; a row that will not
                # parse as numbers is not data.
                continue
    if not rows:
        raise ValueError(f"{path}: no data rows")

    def column(name: str) -> list[float]:
        try:
            index = header.index(name)
        except ValueError as exc:
            raise ValueError(
                f"{path}: no column {name!r} in {header}") from exc
        return [row[index] for row in rows]

    return column("V(SW)"), column("V(QAOUT)"), column("V(QBOUT)")


def _crossing_x(curve: list[tuple[float, float]], c: float) -> float | None:
    """Where the polyline crosses the 45-degree line y = x + c, as an x value.

    Returns None when the line misses the curve.  On the rare exact-tangent
    case the first crossing is taken, which is the conservative choice: it can
    only shrink the reported square, never inflate it.
    """
    best: float | None = None
    for (x0, y0), (x1, y1) in zip(curve, curve[1:]):
        d0 = (y0 - x0) - c
        d1 = (y1 - x1) - c
        if d0 == 0.0:
            candidate = x0
        elif d0 * d1 < 0.0:
            # Linear interpolation along the segment to the sign change.
            t = d0 / (d0 - d1)
            candidate = x0 + t * (x1 - x0)
        else:
            continue
        if best is None:
            best = candidate
    return best


def snm(curve1: list[tuple[float, float]],
        curve2: list[tuple[float, float]],
        steps: int = 2001) -> tuple[float, float, float]:
    """Return (snm, lobe_a, lobe_b) in volts for two butterfly curves."""
    cs = [y - x for x, y in curve1] + [y - x for x, y in curve2]
    lo, hi = min(cs), max(cs)
    lobe_a = 0.0
    lobe_b = 0.0
    for i in range(steps):
        c = lo + (hi - lo) * i / (steps - 1)
        x1 = _crossing_x(curve1, c)
        x2 = _crossing_x(curve2, c)
        if x1 is None or x2 is None:
            continue
        # Both crossings sit on y = x + c, so the side of the square whose
        # diagonal joins them is exactly this coordinate difference.
        side = x1 - x2
        if side > lobe_a:
            lobe_a = side
        if -side > lobe_b:
            lobe_b = -side
    return min(lobe_a, lobe_b), lobe_a, lobe_b


def _self_test() -> None:
    """Infinite-gain inverters: the lobe is a square of side VDD/2.

    curve1 is the ideal VTC (output high until the input passes VDD/2, then
    low); curve2 is the same inverter measured the other way, i.e. curve1
    mirrored about y = x.  The upper-left lobe is exactly
    [0, VDD/2] x [VDD/2, VDD], so the answer must be VDD/2 to the resolution of
    the c grid.
    """
    vdd = 0.7
    half = vdd / 2.0
    curve1 = [(0.0, vdd), (half, vdd), (half, 0.0), (vdd, 0.0)]
    curve2 = [(y, x) for x, y in curve1]
    value, lobe_a, lobe_b = snm(curve1, curve2)
    if abs(value - half) > 1e-3:
        raise AssertionError(
            f"ideal butterfly should give SNM = VDD/2 = {half:.4f} V, "
            f"got {value:.4f} V (lobes {lobe_a:.4f}, {lobe_b:.4f})")

    # A degenerate butterfly -- both inverters identical and linear, so the
    # curves coincide -- has no lobe at all and must report zero rather than
    # some artifact of the grid.
    line = [(0.0, vdd), (vdd, 0.0)]
    value, _, _ = snm(line, line)
    if abs(value) > 1e-6:
        raise AssertionError(
            f"coincident curves should give SNM = 0, got {value:.6f} V")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prn", nargs="?", help="Xyce .print dc output")
    parser.add_argument("--self-test", action="store_true",
                        help="check the square fit against known geometry")
    parser.add_argument("--symmetry-tol", type=float, default=1e-6,
                        help="max |QAOUT - QBOUT| before the cell is called "
                             "asymmetric (volts)")
    parser.add_argument("--prefix", default="",
                        help="prepended to every emitted name, so several "
                             "configurations stay distinguishable in one log")
    args = parser.parse_args(argv)

    if args.self_test:
        _self_test()
        print("PASS snm_from_sweep self-test")
        return 0

    if not args.prn:
        parser.error("a .prn path is required unless --self-test is given")

    sweep, qaout, qbout = read_prn(args.prn)

    # Forcing Q and forcing QB must give the same response on a symmetric
    # cell.  If they do not, the butterfly below is being built from two
    # different inverters and the SNM is not the cell's.
    worst = max(abs(a - b) for a, b in zip(qaout, qbout))
    if worst > args.symmetry_tol:
        print(f"FAIL: cell is asymmetric, max |QAOUT - QBOUT| = {worst:.6f} V",
              file=sys.stderr)
        return 1

    # curve1 forces Q and observes QB; curve2 is the same inverter measured in
    # the other direction, so its coordinates are swapped.
    curve1 = list(zip(sweep, qaout))
    curve2 = [(y, x) for x, y in zip(sweep, qbout)]

    value, lobe_a, lobe_b = snm(curve1, curve2)
    p = args.prefix
    print(f"{p}SNM_MV = {value * 1000:.1f}")
    print(f"{p}SNM_LOBE_A_MV = {lobe_a * 1000:.1f}")
    print(f"{p}SNM_LOBE_B_MV = {lobe_b * 1000:.1f}")
    print(f"{p}SYMMETRY_UV = {worst * 1e6:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
