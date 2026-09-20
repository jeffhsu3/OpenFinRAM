"""Fixed helpers for slice routers.  Not evolved; candidates import it.

A small model asked to rewrite a router "simplifies" its helpers and quietly
drops the rules they encode -- the M2 that must run past a V1, the M3 cap past
a V2.  Keeping those in a module the candidate merely calls leaves the evolved
program as routing *decisions*: which track, which column, where to tap.
All values are nanometres.
"""

from __future__ import annotations

HALF = 9  # half a via, half an 18 nm wire
CAP = 14  # M3 past the centre of a V2 along the wire (9 + 5)
PAD = 17  # M2 past the centre of a V1 along the wire (9 + 8)
TRACK = 36  # wire pitch: 18 wide + 18 apart
SPACE = 18


class Router:
    """Collects shapes and pins; `result()` is what route_slice returns."""

    def __init__(self, frame: dict) -> None:
        self.frame = frame
        self.shapes: list[list] = []
        self.pins: dict[str, list] = {}

    def v1(self, x: float, y: float, pad: bool = True) -> None:
        """M1-M2 via at (x, y).  With `pad` it brings its own legal M2 landing;
        without, an `m2()` wire must cover it and run 8 nm past it on one side."""
        self.shapes.append(["V1", x - HALF, y - HALF, x + HALF, y + HALF])
        if pad:
            self.shapes.append(["M2", x - PAD, y - HALF, x + PAD, y + HALF])

    def v2(self, x: float, y: float) -> None:
        """M2-M3 via at (x, y); needs an M2 and an M3 rectangle around it."""
        self.shapes.append(["V2", x - HALF, y - HALF, x + HALF, y + HALF])

    def m2(self, x0: float, x1: float, y: float, cap: float = PAD) -> None:
        """Horizontal wire on track y between via centres x0 and x1, `cap` past each."""
        lo, hi = min(x0, x1), max(x0, x1)
        self.shapes.append(["M2", lo - cap, y - HALF, hi + cap, y + HALF])

    def m3(self, x: float, y0: float, y1: float, cap: float = CAP) -> None:
        """Vertical wire on column x between via centres y0 and y1, `cap` past each."""
        lo, hi = min(y0, y1), max(y0, y1)
        self.shapes.append(["M3", x - HALF, lo - cap, x + HALF, hi + cap])

    def m3_to_top(self, x: float, y: float) -> None:
        """Vertical wire from a via at (x, y) to the top edge of the slice."""
        self.shapes.append(["M3", x - HALF, y - CAP, x + HALF, self.frame["height"]])

    def pin(self, name: str, metal: str, x: float, y: float) -> None:
        self.pins[name] = [metal, x, y]

    def result(self) -> dict:
        return {"shapes": self.shapes, "pins": self.pins}
