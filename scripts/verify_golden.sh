#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OPENFINRAM_BIN="${BUILD_DIR}/OpenFinRAM"
GDS_COMPARATOR_BIN="${BUILD_DIR}/gds_comparator"
GOLDEN_DIR="${BUILD_DIR}/golden"
RESULTS_DIR="${BUILD_DIR}/results"

log_info() {
    echo "[INFO] $*"
}

log_error() {
    echo "[ERROR] $*" >&2
}

die() {
    log_error "$*"
    exit 1
}

require_file() {
    local file_path="$1"
    [[ -f "${file_path}" ]] || die "Missing required file: ${file_path}"
}

ensure_binaries() {
    if [[ ! -x "${OPENFINRAM_BIN}" || ! -x "${GDS_COMPARATOR_BIN}" ]]; then
        log_info "Building missing binaries (OpenFinRAM / gds_comparator)..."
        cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
        cmake --build "${BUILD_DIR}" -j
    fi

    [[ -x "${OPENFINRAM_BIN}" ]] || die "OpenFinRAM binary not found: ${OPENFINRAM_BIN}"
    [[ -x "${GDS_COMPARATOR_BIN}" ]] || die "gds_comparator binary not found: ${GDS_COMPARATOR_BIN}"
}

latest_artifact_for_cell() {
    local extension="$1"
    local cell_name="$2"

    if [[ ! -d "${RESULTS_DIR}" ]]; then
        return 1
    fi

    find "${RESULTS_DIR}" -type f -name "${cell_name}.${extension}" -printf '%T@ %p\n' 2>/dev/null \
        | sort -nr \
        | head -n 1 \
        | cut -d' ' -f2-
}

run_case_and_verify() {
    local bits="$1"
    local rows="$2"
    local mux="$3"
    local verify_flag="$4"
    local cell_name="$5"

    local golden_sp="${GOLDEN_DIR}/${cell_name}/${cell_name}.sp"
    local golden_gds="${GOLDEN_DIR}/${cell_name}/${cell_name}.gds"

    require_file "${golden_sp}"
    require_file "${golden_gds}"

    log_info "Running case: ${bits} ${rows} ${mux} ${verify_flag} -> ${cell_name}"
    (
        cd "${BUILD_DIR}"
        ./OpenFinRAM "${bits}" "${rows}" "${mux}" "${verify_flag}"
    )

    local generated_sp
    local generated_gds

    generated_sp="$(latest_artifact_for_cell sp "${cell_name}")" || die "Cannot locate generated SP for ${cell_name} in ${RESULTS_DIR}"
    generated_gds="$(latest_artifact_for_cell gds "${cell_name}")" || die "Cannot locate generated GDS for ${cell_name} in ${RESULTS_DIR}"

    [[ -n "${generated_sp}" ]] || die "Generated SP path is empty for ${cell_name}"
    [[ -n "${generated_gds}" ]] || die "Generated GDS path is empty for ${cell_name}"

    log_info "SPICE diff: ${generated_sp} <-> ${golden_sp}"
    diff -u "${golden_sp}" "${generated_sp}"

    log_info "GDS compare: ${generated_gds} <-> ${golden_gds}"
    "${GDS_COMPARATOR_BIN}" "${generated_gds}" "${golden_gds}"

    log_info "Case passed: ${cell_name}"
}

main() {
    ensure_binaries

    # Two mandatory golden cases agreed in workflow.
    run_case_and_verify 2 2 1 0 sram_x4x2x1
    run_case_and_verify 2 2 2 0 sram_x4x2x2

    log_info "All golden checks passed."
}

main
