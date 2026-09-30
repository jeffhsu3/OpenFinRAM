#!/usr/bin/env bash
# Install the tested upstream revision as a standalone tool, local to this build.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${1:-$repo_root/build/tools/gdscheck}"
cargo install --git https://github.com/jeffhsu3/gdscheck.git \
    --rev 24a836ab27277574f8a5d7b6a096850f53843132 \
    --bin gdscheck --root "$install_root"
echo "Installed $install_root/bin/gdscheck"
