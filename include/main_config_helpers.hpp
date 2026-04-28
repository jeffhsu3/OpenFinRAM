#ifndef MAIN_CONFIG_HELPERS_HPP
#define MAIN_CONFIG_HELPERS_HPP

#include <cstdint>
#include <string>

struct MainCliOptions {
    uint64_t num_wls = 2;
    uint64_t num_data_bits = 2;
    uint64_t num_banks = 1;
    bool single_port = true;
    bool run_characterization = true;
    std::string output_sp_name = "sram_colgrp.sp";
    bool spice_only = false;
    int num_wl_buf = 5;
    int num_sae_buf = 10;
};

struct RuntimeDerivedParams {
    uint64_t num_wl = 0;
    uint64_t wl_addr_bits = 0;
    uint64_t mux_addr_bits = 0;
    uint64_t addr_width = 0;
};

MainCliOptions parse_main_cli_options(int argc, char** argv);

RuntimeDerivedParams derive_runtime_params(uint64_t num_wls, uint64_t num_banks);

void log_runtime_derived_params(const RuntimeDerivedParams& params);

#endif // MAIN_CONFIG_HELPERS_HPP
