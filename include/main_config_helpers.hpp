#pragma once

#include <cstdint>

struct MainCliOptions {
    uint64_t test_num_bits = 2;
    uint64_t num_stacked_rows = 2;
    uint64_t num_mux = 1;
    bool run_verification = true;
};

struct RuntimeDerivedParams {
    uint64_t num_wl = 0;
    uint64_t wl_addr_bits = 0;
    uint64_t mux_addr_bits = 0;
    uint64_t addr_width = 0;
};

MainCliOptions parse_main_cli_options(int argc, char** argv);

RuntimeDerivedParams derive_runtime_params(uint64_t test_num_bits, uint64_t num_mux);

void log_runtime_derived_params(const RuntimeDerivedParams& params);
