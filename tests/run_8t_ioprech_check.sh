#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
generator="$repo_root/scripts/generate_asap7_8t_ioprech.py"
committed="$repo_root/tech/gds/sram_8t_ioprech.gds"

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
generated="$scratch/sram_8t_ioprech.gds"

"$python_bin" "$generator" --output "$generated"
"$python_bin" "$generator" --verify "$committed"

if ! cmp -s "$generated" "$committed"; then
    echo "FAIL: committed 8T IO/precharge GDS is stale; regenerate it with:" >&2
    echo "  $python_bin scripts/generate_asap7_8t_ioprech.py" >&2
    exit 1
fi

echo "PASS: ASAP7 8T port-A/port-B IO/precharge wrappers and artifacts"
