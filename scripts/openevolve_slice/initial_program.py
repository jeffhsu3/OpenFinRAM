"""Router for an ASAP7 four-wordline driver slice.  Units are nanometres.

frame: width, height, driver_y0 (drivers sit above this y), and
  nands[i]   a_bar {x, y0, y1, via_y}  vertical M1 bar of input A      (net SEL)
             y_bar {x, y0, y1}         vertical M1 bar of the output   (net N<i>)
             b_tie {x0, x1, y}         M2 bar of input B<i>, already drawn
  drivers[i] input_strap {x, y0, y1}   vertical M1 bar of the input    (net N<i>)
             wl_pad {x, y}             M1 pad of the output            (net WL<i>)
  blocked_m2, blocked_m3               lists [x0, y0, x1, y1] of leaf metal; stay 18 away
Nets: SEL joins all four a_bars.  N<i> joins nands[i].y_bar to
drivers[i].input_strap.  WL<i> runs from drivers[i].wl_pad to the top edge on M3.
Router kit (fixed, do not redefine): r.v1(x, y, pad=True)  M1-M2 via, with M2 pad
  r.v2(x, y)  M2-M3 via      r.m2(x0, x1, y, cap=17)  horizontal wire on track y
  r.m3(x, y0, y1)  vertical wire on column x      r.m3_to_top(x, y)
  r.pin(name, metal, x, y)   r.result()           constants HALF=9 CAP=14 TRACK=36
A V1 must sit on a bar at least 14 inside its ends (y0 + 14 .. y1 - 14), or
exactly on a wl_pad.  Wires on the same layer stay 18 apart, 31 end to end,
and every shape keeps x within [9, width - 9].
"""
from router_kit import CAP, HALF, TRACK, Router


# EVOLVE-BLOCK-START
# The routing choices.  Editing one number in plan() is the safest useful change:
#   sel_column       x (nm) of the M3 joining the two SEL rows
#   n_column_offset  for net N<i>, how far (nm) its M3 column sits from the
#                    driver's input strap; 0 climbs straight under the strap.  A
#                    long M3 low in the slice blocks the escape columns of the B
#                    pins above it, so moving it toward a slice edge can help.
#   tap_tracks       how many 36 nm tracks below the top of the output bar N<i> taps
#   landing_track    which track above the bottom of the input strap N<i> lands on
# Every column must stay inside the slice: 18 <= x <= frame["width"] - 18.
def plan(frame):
    return {
        "sel_column": frame["width"] / 2,
        "n_column_offset": [0, 0, 0, 0],
        "tap_tracks": [1, 1, 1, 1],
        "landing_track": [0, 1, 0, 1],
    }


def route_slice(frame):
    r = Router(frame)
    PLAN = plan(frame)
    nands, drivers = frame["nands"], frame["drivers"]

    # SEL: one M2 wire per NAND row through the A-bar vias, joined by one M3.
    sel_x = PLAN["sel_column"]
    rows = sorted({n["a_bar"]["via_y"] for n in nands})
    for y in rows:
        xs = [n["a_bar"]["x"] for n in nands if n["a_bar"]["via_y"] == y]
        for x in xs:
            r.v1(x, y, pad=False)
        r.m2(min(xs), max(xs), y, cap=HALF)
        r.v2(sel_x, y)
    r.m3(sel_x, rows[0], rows[-1])
    r.pin("SEL", "M3", sel_x, (rows[0] + rows[-1]) / 2 - TRACK)

    # N<i>: tap the NAND output bar, run M2 to the chosen column, climb on M3
    # past the VSS rail, run M2 to the driver's strap, land on it.
    for i, (n, d) in enumerate(zip(nands, drivers)):
        tap_x = n["y_bar"]["x"]
        tap_y = n["y_bar"]["y1"] - TRACK * PLAN["tap_tracks"][i]
        in_x = d["input_strap"]["x"]
        land_y = d["input_strap"]["y0"] + CAP + TRACK * PLAN["landing_track"][i]
        col_x = in_x + PLAN["n_column_offset"][i]
        r.v1(tap_x, tap_y, pad=False)
        r.m2(tap_x, col_x, tap_y)
        r.v2(col_x, tap_y)
        r.m3(col_x, tap_y, land_y)
        r.v2(col_x, land_y)
        r.m2(col_x, in_x, land_y)
        r.v1(in_x, land_y, pad=False)

    # WL<i>: straight up from the driver's output pad to the top edge.
    for d in drivers:
        x, y = d["wl_pad"]["x"], d["wl_pad"]["y"]
        r.v1(x, y)
        r.v2(x, y)
        r.m3_to_top(x, y)
        r.pin(f"WL{d['wordline']}", "M3", x, frame["height"] - TRACK)

    for n in nands:
        tie = n["b_tie"]
        r.pin(f"B{n['wordline']}", "M2", (tie["x0"] + tie["x1"]) / 2, tie["y"])
    return r.result()
# EVOLVE-BLOCK-END
