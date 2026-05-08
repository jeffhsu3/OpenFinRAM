#include "main_config_helpers.hpp"

#include "argparse/argparse.hpp"
#include "plog/Log.h"

MainCliOptions parseMainCliOptions(int argc, char** argv) {
    argparse::ArgumentParser program("OpenFinRAM");

    program.add_argument("--num-wls")
        .help("Number of wordlines. Must be an even number.")
        .default_value(unsigned{2})
        .scan<'u', unsigned>();

    program.add_argument("--num-data-bits")
        .help("Number of data bits (D/Q port width). Must be an even number.")
        .default_value(unsigned{2})
        .scan<'u', unsigned>();

    program.add_argument("--num-banks")
        .help("Number of banks. Must be a power of 2.")
        .default_value(unsigned{1})
        .scan<'u', unsigned>();

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

    program.add_argument("--output-gds-name")
        .help("Output name for generated GDS file.")
        .default_value(std::string("sram.gds"));

    program.add_argument("--spice-only")
        .help("Only generate SPICE netlist but also run Innovus.")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--num-wl-buf")
        .help("Number of buffers in the wordline (WL) chain for delay estimation.")
        .default_value(unsigned{5})
        .scan<'u', unsigned>();

    program.add_argument("--num-sae-buf")
        .help("Number of buffers in the sense amplifier enable (SAE) chain for delay estimation.")
        .default_value(unsigned{10})
        .scan<'u', unsigned>();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        LOGE << "CLI parse error: " << e.what();
        LOGE << program;
        std::exit(1);
    }

    MainCliOptions options;
    options.num_wls               = program.get<unsigned>("--num-wls");
    options.num_data_bits         = program.get<unsigned>("--num-data-bits");
    options.num_banks             = program.get<unsigned>("--num-banks");
    options.single_port           = program.get<bool>("--single-port");
    options.skip_characterization = program.get<bool>("--skip-characterization");
    options.num_wl_buf            = program.get<unsigned>("--num-wl-buf");
    options.num_sae_buf           = program.get<unsigned>("--num-sae-buf");
    options.output_sp_name        = program.get<std::string>("--output-sp-name");
    options.output_gds_name       = program.get<std::string>("--output-gds-name");
    options.spice_only            = program.get<bool>("--spice-only");

    if (options.num_wls % 2 != 0) {
        LOGE << "Error: --num-wls must be an even number.";
        std::exit(1);
    }
    if (options.num_data_bits % 2 != 0) {
        LOGE << "Error: --num-data-bits must be an even number.";
        std::exit(1);
    }
    if ((options.num_banks & (options.num_banks - 1)) != 0) {
        LOGE << "Error: --num-banks must be a power of 2.";
        std::exit(1);
    }

    LOGD << "Configuration: num_wls=" << options.num_wls
         << ", num_data_bits=" << options.num_data_bits
         << ", num_banks=" << options.num_banks
         << ", single_port=" << (options.single_port ? 1 : 0)
         << ", skip_characterization=" << (options.skip_characterization ? 1 : 0)
         << ", output_sp_name=" << options.output_sp_name
         << ", output_gds_name=" << options.output_gds_name
         << ", spice_only=" << (options.spice_only ? 1 : 0);

    return options;
}
