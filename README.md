# ASAP7 SRAM Compiler

This is an SRAM compiler designed for the ASAP7 PDK, leveraging the GDSII Tool Kit (GDSTK) to create SRAM layouts.

# Usage

```
git clone --recursive https://github.com/shao-chien-lu/OpenFinRAM.git
cd OpenFinRAM
mkdir build
cd build
cp -r ../tech .
cmake ..
make

./OpenFinRAM --num-wls 2 --num-data-bits 4 --num-banks 1 --single-port
```

## Open-Source Flow (Yosys + OpenROAD + OpenSTA, single-port ASAP7)

Pinned to local OpenROAD checkout at `~/iv3/repos/OpenROAD` and ASAP7 platform `~/iv3/repos/OpenROAD/platform/asap7`.

```
# single-port open-source flow (Yosys synthesis + OpenROAD P&R + OpenSTA)
./OpenFinRAM --num-wls 2 --num-data-bits 4 --num-banks 1 --single-port --openroad
# or explicitly:
./OpenFinRAM --num-wls 2 --num-data-bits 4 --num-banks 1 --single-port --use-yosys --use-openroad

# custom pinned paths (default to ~/iv3/repos/OpenROAD):
./OpenFinRAM --openroad --openroad-path ~/iv3/repos/OpenROAD --platform-path ~/iv3/repos/OpenROAD/platform/asap7
```

Notes:
- Commercial flow (`Design Compiler` + `Innovus` + `Calibre`) is still default when `--openroad` is omitted.
- Yosys script is emitted to `tmp/syn_<ts>/synth.ys` → `netlist.v`/`timing.sdc`/`qor_report.txt` (same contract as DC).
- OpenROAD TCL is emitted to `tmp/openroad_<ts>/run.tcl` and reads `platform/asap7` LEF/lib (fallback to `tech/lef|lib`).
- KLayout's Python bindings are required to stream the routed OpenROAD DEF to `ctrl_decode.gds`, merging the ASAP7 standard-cell GDS before final SRAM assembly.
- `OpenSTA` is invoked via OpenROAD `report_checks`/`report_tns` during P&R.
- Macro LEF is extracted natively from the final merged GDS with GDSTK. Cadence `strmin`/`abstract` and `tcsh` are not required for LEF export.
- `--skip-characterization` still writes `<cell>.lib`: it is an explicitly marked FakeRAM-style early-PPA estimate (actual LEF area, compact timing tables, no power model). If SiliconSmart is requested but fails, the same estimate is used as a fallback; a successful characterized model is never overwritten.
- Only single-port is validated for the open-source path; dual-port falls back to commercial flow.

## Commercial Flow (Cadence / Synopsys, default)

```
./OpenFinRAM --num-wls 2 --num-data-bits 4 --num-banks 1 --single-port
# uses dc_shell + innovus and optional calibre/siliconsmart as before;
# LEF export itself is native and no longer needs Cadence Abstract
```

## References

- **ASAP7 PDK**: https://github.com/The-OpenROAD-Project/asap7
- **GDSTK**: https://github.com/heitzmann/gdstk
- **BSG FakeRAM**: https://github.com/bespoke-silicon-group/bsg_fakeram
- **PLOG**: https://github.com/SergiusTheBest/plog

## License

This project is licensed under the BSD 3-Clause License - see the [LICENSE](LICENSE) file for details.
