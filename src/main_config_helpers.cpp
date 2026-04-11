#include "main_config_helpers.hpp"

#include "plog/Log.h"

#include <string>

MainCliOptions parse_main_cli_options(int argc, char** argv) {
    MainCliOptions options;

    if (argc >= 2) {
        options.test_num_bits = std::stoull(argv[1]);
        LOGI << "Using test_num_bits from command line: " << options.test_num_bits;
    } else {
        LOGI << "Using default test_num_bits: " << options.test_num_bits;
    }

    if (argc >= 3) {
        options.num_stacked_rows = std::stoull(argv[2]);
        LOGI << "Using num_stacked_rows from command line: " << options.num_stacked_rows;
    } else {
        LOGI << "Using default num_stacked_rows: " << options.num_stacked_rows;
    }

    if (argc >= 4) {
        options.num_mux = std::stoull(argv[3]);
        LOGI << "Using num_mux from command line: " << options.num_mux;
    } else {
        LOGI << "Using default num_mux: " << options.num_mux;
    }

    if (argc >= 5) {
        uint64_t verify_arg = std::stoull(argv[4]);
        options.run_verification = (verify_arg != 0);
        LOGI << "Using run_verification from command line: "
             << (options.run_verification ? "1 (enabled)" : "0 (disabled)");
    } else {
        LOGI << "Using default run_verification: 1 (enabled)";
    }

    LOGI << "Configuration: test_num_bits=" << options.test_num_bits
         << ", num_stacked_rows=" << options.num_stacked_rows
         << ", num_mux=" << options.num_mux
         << ", run_verification=" << (options.run_verification ? 1 : 0);

    return options;
}

RuntimeDerivedParams derive_runtime_params(uint64_t test_num_bits, uint64_t num_mux) {
    RuntimeDerivedParams params;

    params.num_wl = test_num_bits;

    uint64_t temp = params.num_wl;
    while (temp > 1) {
        params.wl_addr_bits++;
        temp >>= 1;
    }

    temp = num_mux;
    while (temp > 1) {
        params.mux_addr_bits++;
        temp >>= 1;
    }

    params.addr_width = params.wl_addr_bits + 1 + 2 + params.mux_addr_bits;
    return params;
}

void log_runtime_derived_params(const RuntimeDerivedParams& params) {
    LOGI << "Address width calculation:";
    LOGI << "  WLT/WLB count: " << params.num_wl;
    LOGI << "  WL address bits: " << params.wl_addr_bits;
    LOGI << "  Top/Bottom bit: 1";
    LOGI << "  YSel bits: 2";
    LOGI << "  MUX bits: " << params.mux_addr_bits;
    LOGI << "  Total addr_width: " << params.addr_width;
}
