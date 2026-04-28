#include "main_config_helpers.hpp"

#include "argparse/argparse.hpp"
#include "plog/Log.h"

MainCliOptions parse_main_cli_options(int argc, char** argv) {
    argparse::ArgumentParser program("OpenFinRAM");

    program.add_argument("--num-wls")
        .help("Number of wordline top/bottom (i.e., |WLT/WLB|). Must be an even number.")
        .default_value(uint64_t{2})
        .scan<'u', uint64_t>();

    program.add_argument("--num-data-bits")
        .help("Number of D/Q bits (i.e., |D/Q|). Must be an even number.")
        .default_value(uint64_t{2})
        .scan<'u', uint64_t>();

    program.add_argument("--num-banks")
        .help("Number of banks. Must be a power of 2.")
        .default_value(uint64_t{1})
        .scan<'u', uint64_t>();

    program.add_argument("--single-port")
        .help("Generate single-port SRAM (default: dual-port).")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--skip-characterization")
        .help("Disable SiliconSmart characterization and verification steps.")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--output-sp-name")
        .help("Output name for generated SPICE netlist.")
        .default_value(std::string("sram.sp"));

    program.add_argument("--spice-only")
        .help("Only generate SPICE netlist but also run Innovus.")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--num-wl-buf")
        .help("Number of buffers for wl delay.")
        .default_value(uint64_t{5})
        .scan<'u', uint64_t>();

    program.add_argument("--num-sae-buf")
        .help("Number of buffers for sae delay.")
        .default_value(uint64_t{10})
        .scan<'u', uint64_t>();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        LOGE << "CLI parse error: " << e.what();
        std::cerr << program;
        std::exit(1);
    }

    MainCliOptions options;
    options.num_wls    = program.get<uint64_t>("--num-wls");
    options.num_data_bits = program.get<uint64_t>("--num-data-bits");
    options.num_banks          = program.get<uint64_t>("--num-banks");
    options.single_port        = program.get<bool>("--single-port");
    options.run_characterization = !program.get<bool>("--skip-characterization");
    options.num_wl_buf = program.get<uint64_t>("--num-wl-buf");
    options.num_sae_buf = program.get<uint64_t>("--num-sae-buf");
    options.output_sp_name = program.get<std::string>("--output-sp-name");
    options.spice_only = program.get<bool>("--spice-only");

    LOGI << "Configuration: num_wls=" << options.num_wls
         << ", num_data_bits=" << options.num_data_bits
         << ", num_banks=" << options.num_banks
         << ", single_port=" << (options.single_port ? 1 : 0)
         << ", run_characterization=" << (options.run_characterization ? 1 : 0)
         << ", output_sp_name=" << options.output_sp_name
         << ", spice_only=" << (options.spice_only ? 1 : 0);

    return options;
}

RuntimeDerivedParams derive_runtime_params(uint64_t num_wls, uint64_t num_banks) {
    RuntimeDerivedParams params;

    params.num_wl = num_wls;

    uint64_t temp = params.num_wl;
    while (temp > 1) {
        params.wl_addr_bits++;
        temp >>= 1;
    }

    temp = num_banks;
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
