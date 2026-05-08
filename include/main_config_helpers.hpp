#ifndef MAIN_CONFIG_HELPERS_HPP
#define MAIN_CONFIG_HELPERS_HPP

#include <cstdint>
#include <string>

struct MainCliOptions {
    unsigned num_wls = 2;
    unsigned num_data_bits = 2;
    unsigned num_banks = 1;
    bool single_port = false;
    bool skip_characterization = true;
    std::string output_sp_name = "sram.sp";
    std::string output_gds_name = "sram.gds";
    bool spice_only = false;
    unsigned num_wl_buf = 5;
    unsigned num_sae_buf = 10;
};

MainCliOptions parseMainCliOptions(int argc, char** argv);

#endif // MAIN_CONFIG_HELPERS_HPP
