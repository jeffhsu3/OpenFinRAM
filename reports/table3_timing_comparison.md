# Table III reproduction: read-timing comparison

All values are ns. Stimulus point: input slew 0.04 ns, output load
0.04608 pF, TT corner.

## 128-word x 64-bit (8 Kb class)

| Metric | Vendor .lib (Cadence char.) | Report PDK | Report Ours | OS .lib estimate | OS Xyce transient |
|---|---|---|---|---|---|
| Cell rise | 0.189 | -- | -- | 0.218 | -- |
| Rise transition | 0.116 | -- | -- | 0.227 | -- |
| Cell fall | 0.182 | -- | -- | 0.218 | -- |
| Fall transition | 0.084 | -- | -- | 0.227 | -- |

## 256-word x 64-bit (16 Kb class)

| Metric | Vendor .lib (Cadence char.) | Report PDK | Report Ours | OS .lib estimate | OS Xyce transient |
|---|---|---|---|---|---|
| Cell rise | 0.198 | 0.250 | 0.216 | 0.218 | 0.217 |
| Rise transition | 0.116 | 0.072 | 0.064 | 0.227 | 0.063 |
| Cell fall | 0.191 | 0.252 | 0.216 | 0.218 | 0.221 |
| Fall transition | 0.084 | 0.057 | 0.048 | 0.227 | 0.048 |

## 512-word x 64-bit (16 Kb class)

| Metric | Vendor .lib (Cadence char.) | Report PDK | Report Ours | OS .lib estimate | OS Xyce transient |
|---|---|---|---|---|---|
| Cell rise | -- | 0.270 | 0.245 | -- | 0.272 |
| Rise transition | -- | 0.070 | 0.063 | -- | 0.063 |
| Cell fall | -- | 0.264 | 0.246 | -- | 0.274 |
| Fall transition | -- | 0.053 | 0.048 | -- | 0.048 |

## 1024-word x 64-bit (16 Kb class)

| Metric | Vendor .lib (Cadence char.) | Report PDK | Report Ours | OS .lib estimate | OS Xyce transient |
|---|---|---|---|---|---|
| Cell rise | -- | 0.297 | 0.369 | -- | 0.383 |
| Rise transition | -- | 0.068 | 0.061 | -- | 0.063 |
| Cell fall | -- | 0.287 | 0.367 | -- | 0.385 |
| Fall transition | -- | 0.058 | 0.047 | -- | 0.048 |

## Notes

- **Vendor .lib**: `srambank_<rows>x4mux<bits>_6t122.lib` from the
  ASAP7 `asap7_sram_0p0` PDK add-on (Cadence-characterized). Its
  headers declare ns/pF but the delay tables are numerically ps/fF;
  interpreted as ps here (their grid contains exactly the 40 ps /
  46.08 fF stimulus point). No 512/1024-word vendor libs exist.
- **Report PDK / Ours**: transcribed from `MOST_report.pdf`, Table III.
- **OS .lib estimate**: `--skip-characterization` output; deliberately
  flat FakeRAM-style early-PPA tables (constant across slew/load).
- **OS Xyce transient**: `characterize_read.py --mode table3`, full
  read path through the real sense amp/latch/tristate with
  replica-timed SAE; transitions measured 10%-90% (Liberty slew
  conventions differ, typically 30%-70% derated).
- Config caveat: the vendor parts are mux-4 banks while the OpenFinRAM
  transient sweep varies bitline depth directly; capacities shown are
  the 16 Kb class unless stated otherwise.
