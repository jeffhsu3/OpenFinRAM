#!/bin/bash

# Locate Xyce and verify that its runtime can initialize.  Some restricted
# environments allow the executable to be inspected but prevent the MPI
# runtime from opening its local socket.  CTest treats exit 77 as a skip, so
# do that here instead of letting every characterization measure look like an
# electrical sanity failure.
require_xyce() {
    local bundled_xyce="/home/jeff/iv4/local/xyce-14.4/bin/Xyce"

    if [ -n "${XYCE:-}" ] && [ -x "$XYCE" ]; then
        :
    elif [ -x "$bundled_xyce" ]; then
        XYCE="$bundled_xyce"
    else
        XYCE="$(command -v Xyce || true)"
    fi

    if [ -z "${XYCE:-}" ] || [ ! -x "$XYCE" ]; then
        echo "SKIP: Xyce not available"
        exit 77
    fi

    if ! "$XYCE" -v >/dev/null 2>&1; then
        echo "SKIP: Xyce is installed but cannot initialize in this environment"
        exit 77
    fi
}
