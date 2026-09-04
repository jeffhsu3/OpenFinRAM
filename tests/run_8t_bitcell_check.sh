#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
generator="$repo_root/scripts/generate_asap7_8t_bitcell.py"
committed="$repo_root/tech/gds/sram_cell_8t.gds"
committed_edges="$repo_root/tech/gds/sram_cell_8t_edges.gds"
committed_tap="$repo_root/tech/gds/sram_cell_8t_tap.gds"

if [[ -x "$repo_root/.venv/bin/python" ]]; then
    python_bin="$repo_root/.venv/bin/python"
else
    python_bin="${PYTHON:-python3}"
fi

if ! "$python_bin" -c 'import gdstk' >/dev/null 2>&1; then
    echo "SKIP: gdstk Python module is unavailable" >&2
    exit 77
fi

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
generated="$scratch/sram_cell_8t.gds"
generated_edges="$scratch/sram_cell_8t_edges.gds"
generated_tap="$scratch/sram_cell_8t_tap.gds"

"$python_bin" "$generator" --output "$generated"
"$python_bin" "$generator" --verify "$committed"
"$python_bin" "$generator" --verify-edges "$committed_edges"
"$python_bin" "$generator" --verify-tap "$committed_tap" --output "$committed"

if ! cmp -s "$generated" "$committed"; then
    echo "FAIL: committed 8T GDS is stale; regenerate it with:" >&2
    echo "  $python_bin scripts/generate_asap7_8t_bitcell.py" >&2
    exit 1
fi

if ! cmp -s "$generated_edges" "$committed_edges"; then
    echo "FAIL: committed 8T edge-cell GDS is stale; regenerate it with:" >&2
    echo "  $python_bin scripts/generate_asap7_8t_bitcell.py" >&2
    exit 1
fi

if ! cmp -s "$generated_tap" "$committed_tap"; then
    echo "FAIL: committed 8T tap GDS is stale; regenerate it with:" >&2
    echo "  $python_bin scripts/generate_asap7_8t_bitcell.py" >&2
    exit 1
fi

echo "PASS: ASAP7 8T bitcell/edge/tap topology, tiling, rules, and artifacts"
