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

    program.add_argument("--bitcell")
        .help("8t (default): the two-port 8T macro. 6t: a single-port macro of the released "
              "6T cell, built like the two-port one with port A alone.")
        .default_value(std::string("8t"));

    program.add_argument("--segment-bits")
        .help("Divided wordlines (--bitcell 6t): cut each stack of data bits into segments of "
              "this many bits, each with its own wordlines and a strip pair between two. "
              "0 (default): whole stacks.")
        .default_value(unsigned{0})
        .scan<'u', unsigned>();

    program.add_argument("--share-port-b")
        .help("Two-port: banks in pairs share port B's sense amplifier, write driver "
              "and output latch, the pair's arrays mirrored about one two-sided IO block. "
              "Needs an even number of banks.")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--strips-in-controller")
        .help("Two-port: place and route the controller around the wordline driver strips, "
              "its band filling the space between the two stacks of column tiles.")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--skip-characterization")
        .help("Skip SiliconSmart and emit an explicitly estimated early-PPA Liberty model.")
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

    program.add_argument("--use-yosys")
        .help("Use Yosys for synthesis (open-source, instead of Design Compiler).")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--use-openroad")
        .help("Use OpenROAD for P&R (open-source, instead of Innovus). Pairs with --use-yosys.")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--openroad")
        .help("Alias for --use-yosys --use-openroad (ASAP7 open-source flow).")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--openroad-path")
        .help("OpenROAD binary name or path (default: 'openroad' resolved from $PATH)")
        .default_value(std::string(""))
        ;

    program.add_argument("--platform-path")
        .help("Path to ASAP7 platform (default: <openroad>/platform/asap7)")
        .default_value(std::string(""));

    program.add_argument("--liberty-from")
        .help("Characterization JSON with measured values overriding the "
              "estimated Liberty constants (timing tables, leakage, caps).")
        .default_value(std::string(""));

    program.add_argument("--bitcell-width")
        .help("Custom push-rule bitcell width (um). Default: 0.108 for logic-rule ASAP7.")
        .default_value(double{0.108})
        .scan<'f', double>();

    program.add_argument("--bitcell-height")
        .help("Custom push-rule bitcell height (um). Default: 0.27 for logic-rule ASAP7.")
        .default_value(double{0.27})
        .scan<'f', double>();

    program.add_argument("--num-rows-per-mux")
        .help("Number of bitcell rows per IO column group (multiplexing factor). Default: 4.")
        .default_value(unsigned{4})
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
    options.share_port_b          = program.get<bool>("--share-port-b");
    const std::string bitcell     = program.get<std::string>("--bitcell");
    if (bitcell != "8t" && bitcell != "6t") {
        LOGE << "Error: --bitcell is 8t or 6t, got " << bitcell;
        std::exit(1);
    }
    options.bitcell_6t            = bitcell == "6t";
    options.segment_bits          = program.get<unsigned>("--segment-bits");
    options.strips_in_controller  = program.get<bool>("--strips-in-controller");
    options.skip_characterization = program.get<bool>("--skip-characterization");
    options.num_wl_buf            = program.get<unsigned>("--num-wl-buf");
    options.num_sae_buf           = program.get<unsigned>("--num-sae-buf");
    options.output_sp_name        = program.get<std::string>("--output-sp-name");
    options.output_gds_name       = program.get<std::string>("--output-gds-name");
    options.spice_only            = program.get<bool>("--spice-only");
    options.use_yosys             = program.get<bool>("--use-yosys");
    options.use_openroad          = program.get<bool>("--use-openroad");
    options.openroad_only         = program.get<bool>("--openroad");
    options.openroad_path         = program.get<std::string>("--openroad-path");
    options.platform_path         = program.get<std::string>("--platform-path");
    options.liberty_from_json     = program.get<std::string>("--liberty-from");
    options.bitcell_width         = program.get<double>("--bitcell-width");
    options.bitcell_height        = program.get<double>("--bitcell-height");
    options.num_rows_per_mux      = program.get<unsigned>("--num-rows-per-mux");

    if (options.openroad_only) {
        options.use_yosys = true;
        options.use_openroad = true;
    }
    // Default: resolve 'openroad' from $PATH at run time. A directory path
    // pointing at an OpenROAD checkout is still accepted and searched for
    // build/src/openroad by OpenRoadTclGenerator::run_openroad.
    if (options.openroad_path.empty()) {
        options.openroad_path = "openroad";
    }
    if (options.platform_path.empty()) {
        options.platform_path = options.openroad_path + "/platform/asap7";
    }
    if (options.bitcell_6t && options.single_port) {
        LOGE << "Error: --bitcell 6t is the generated single-port flow; --single-port is the legacy one.";
        std::exit(1);
    }
    if (options.bitcell_6t && (options.share_port_b || options.strips_in_controller)) {
        LOGE << "Error: --bitcell 6t has one port and takes no --share-port-b or --strips-in-controller yet.";
        std::exit(1);
    }
    if (options.segment_bits && options.segment_bits < options.num_data_bits / 2) {
        if (!options.bitcell_6t) {
            LOGE << "Error: --segment-bits is for the single-port macro (--bitcell 6t) so far.";
            std::exit(1);
        }
        if ((options.num_data_bits / 2) % options.segment_bits != 0) {
            LOGE << "Error: --segment-bits " << options.segment_bits << " does not divide a stack of "
                 << options.num_data_bits / 2 << " bits.";
            std::exit(1);
        }
    }
    if (options.segment_bits >= options.num_data_bits / 2) options.segment_bits = 0;
    if (options.bitcell_6t && options.num_rows_per_mux != 4 && options.num_rows_per_mux != 8 &&
        options.num_rows_per_mux != 16) {
        LOGE << "Error: --bitcell 6t's IO block is 4:1, 8:1 or 16:1 (--num-rows-per-mux).";
        std::exit(1);
    }
    if ((options.use_yosys || options.use_openroad) && options.bitcell_6t) {
        LOGI << "Open-source flow in generated single-port (6T) mode";
    } else if ((options.use_yosys || options.use_openroad) && !options.single_port) {
        LOGI << "Open-source flow in dual-port (8T) mode";
    } else if ((options.use_yosys || options.use_openroad) && options.single_port) {
        LOGI << "Open-source flow in single-port (6T) mode";
    }

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
    if (options.strips_in_controller && options.single_port) {
        LOGE << "Error: --strips-in-controller is for the two-port macro.";
        std::exit(1);
    }
    if (options.share_port_b && (options.single_port || options.num_banks < 2)) {
        LOGE << "Error: --share-port-b needs the two-port macro and at least two banks.";
        std::exit(1);
    }

    LOGD << "Configuration: num_wls=" << options.num_wls
         << ", num_data_bits=" << options.num_data_bits
         << ", num_banks=" << options.num_banks
         << ", single_port=" << (options.single_port ? 1 : 0)
         << ", skip_characterization=" << (options.skip_characterization ? 1 : 0)
         << ", output_sp_name=" << options.output_sp_name
         << ", output_gds_name=" << options.output_gds_name
         << ", spice_only=" << (options.spice_only ? 1 : 0)
         << ", use_yosys=" << (options.use_yosys ? 1 : 0)
         << ", use_openroad=" << (options.use_openroad ? 1 : 0)
         << ", openroad_path=" << options.openroad_path
         << ", platform_path=" << options.platform_path;

    return options;
}
