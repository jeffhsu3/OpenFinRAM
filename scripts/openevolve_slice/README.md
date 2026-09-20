# OpenEvolve: routing the handcrafted driver slice

Logical effort (`chipforge_asap7.devices.sizing.size_decoder`) settles the
decoder's devices. What it cannot settle is the routing inside the
handcrafted four-wordline slice: which tracks the wires take, where the vias
go, and whether the result leaves its pins reachable and its M2/M3 tracks
free for the predecode bus. That is combinatorial, and only DRC finally
judges it. This directory searches it with OpenEvolve and a locally served
model.

```bash
# score the hand-written router; no LLM involved (about 15 s)
.venv/bin/python scripts/run_openevolve_slice.py --check

# evolve against the model served at localhost:8000
.venv/bin/python scripts/run_openevolve_slice.py --iterations 200
.venv/bin/python scripts/run_openevolve_slice.py --iterations 60 --thinking   # reasoning on

# resume, inspect every candidate, draw the winner
.venv/bin/python scripts/run_openevolve_slice.py --resume results/openevolve_slice/<run>/checkpoints/checkpoint_50
.venv/bin/python scripts/run_openevolve_slice.py --iterations 5 --checkpoint-interval 1
.venv/bin/python scripts/run_openevolve_slice.py --apply results/openevolve_slice/<run>/best/best_program.py
```

OpenEvolve is installed editable from `~/iv4/repos/openevolve`
(`uv pip install --python .venv/bin/python -e ~/iv4/repos/openevolve`).

## Files

| file | role |
|---|---|
| `initial_program.py` | the evolved program: `plan(frame)` (the routing choices) and `route_slice(frame)` |
| `router_kit.py` | fixed helpers the program imports; they carry the via and end-cap rules so a rewrite cannot drop them |
| `slice_frame.py` | the frame a candidate is given, the static rule and connectivity checker, the objectives |
| `evaluator.py` | the three-stage ladder OpenEvolve calls |
| `local_llm.py` | client that switches the model's reasoning on or off and keeps its final code block |
| `config.yaml` | OpenEvolve configuration for `http://localhost:8000/v1` |
| `../run_openevolve_slice.py` | runner: server preflight, baseline, evolution, resume, apply |

## The problem

`route_slice(frame)` returns M2, M3, V1 and V2 rectangles and the pin
positions for `chipforge_asap7`'s `DriverSliceSpec`, whose leaf cells (four
`NandSpec` tiles under four interleaved `InverterSpec` drivers) are already
placed. It must make net SEL across the four NAND A bars, nets N0..N3 from
each NAND's output bar to its driver's input strap, and WL0..WL3 from each
driver's output pad to the top edge on M3. The frame is plain JSON-safe data
in slice coordinates, so a candidate needs no layout library.
`build_driver_slice(spec, router=...)` draws the result.

## The ladder

| stage | what runs | time | score band |
|---|---|---|---|
| 1 | candidate in a subprocess on all three sizes; widths, via sizes, keep-outs, landings, shorts and opens by union-find | ms | 0 to 0.25 |
| 2 | KLayout LVS and the public ASAP7 DRC runset on the small slice between its filler and tap columns | ~5 s | 0.3 to 0.5 |
| 3 | LVS and DRC on the one-row LVT and the released-size slices, DRC on two butted slices, then the objectives | ~10 s | 0.5 to 1.0 |

A candidate that gets further always outranks one that stopped earlier.
Failures come back to the model as artifacts with coordinates: the traceback,
`short: one metal group touches nets ['N2', 'N3']`, or the DRC category and
location.

Objectives, averaged over the three sizes:

- `pin_escape` (0.35): per input pin, M3 columns on a 36 nm grid that can drop
  from the pin's M2 straight to the bottom edge, where the predecode bus and
  the slice's small gates are; capped at four per pin.
- `m2_feedthrough` (0.25): horizontal M2 tracks over the NAND rows a bus wire
  could cross the whole slice on.
- `m3_feedthrough` (0.20): M3 columns clear from the bottom edge to the drivers.
- `wire` (0.20): M2 + M3 length plus 100 nm per via, against the baseline.

`pin_escape` and `m3_feedthrough` are also the MAP-Elites feature axes, so
routers that trade one for the other are kept apart.

The hand-written router scores 0.848: the upper-row B pins have two escape
columns each and only 17 % of M3 columns run clear, because the lower NANDs'
outputs climb on M3 straight under their drivers. Moving two of those columns
toward the slice edges (`n_column_offset = [-54, 0, 108, 0]`) is legal on
every size and scores 0.866, so the headroom is real and reachable by a
one-line edit.

## The local model

The server at localhost:8000 is vLLM serving a 3B reasoning model. Two
things follow, both handled in `local_llm.py`:

- With reasoning on it writes its deliberation into `content` and gives its
  answer last, while OpenEvolve's parser takes the first fenced block. The
  client keeps only the last block.
- `chat_template_kwargs={"enable_thinking": false}` in `extra_body` turns the
  reasoning off: a one-line function goes from over a thousand tokens to 18.

`diff_based_evolution` is off because a model this size cannot reproduce
source text exactly enough for SEARCH/REPLACE blocks. It also tends to rewrite
everything and break things, which is why the rules live in `router_kit.py`
and the choices in a small `plan(frame)`. A larger served model will need none
of that; point `--api-base` and `--model` at it.

## Retargeting

The harness is the reusable part. For another handcrafted block (the SAE
cell's `pair` tiles, the 8T I/O column adapters) keep `evaluator.py`'s ladder
and `local_llm.py`, and replace `slice_frame.py`'s `SPECS`, `make_frame`,
`static_check` landings and `objectives`.
