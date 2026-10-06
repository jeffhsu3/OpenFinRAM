#!/usr/bin/env bash
# Install the tested upstream revision as a standalone tool, local to this build.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${1:-$repo_root/build/tools/gdscheck}"
# GDSCHECK_SRC=<checkout> installs a local gdscheck (the full ASAP7 process
# verify_macro.py needs: --suite main, ACTIVE.W.2/LUP.1 read as the DRM
# writes them); otherwise the pinned revision.
if [ -n "${GDSCHECK_SRC:-}" ]; then
    cargo install --path "$GDSCHECK_SRC" --bin gdscheck --root "$install_root" --force
else
    cargo install --git https://github.com/jeffhsu3/gdscheck.git \
        --rev 24a836ab27277574f8a5d7b6a096850f53843132 \
        --bin gdscheck --root "$install_root"
fi
echo "Installed $install_root/bin/gdscheck"
