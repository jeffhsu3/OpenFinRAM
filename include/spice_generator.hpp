#ifndef SPICE_GENERATOR_HPP
#define SPICE_GENERATOR_HPP

#include <string>
#include <vector>
#include <fstream>

#include "main_config_helpers.hpp"

namespace OpenFinRAM {
class SpiceGenerator {
public:
    explicit SpiceGenerator(const MainCliOptions& config);

    bool generate();

private:
    MainCliOptions config_;

    std::string format_ports(const std::vector<std::string>& ports, int max_per_line = 10);
    std::string create_subckt(const std::string& name,
                              const std::vector<std::string>& ports,
                              const std::string& instances);

    std::string generate_cell_row();
    std::string generate_sramcol();
    std::string generate_array();
    std::string generate_colgrp();
    std::string generate_stacked_colgrp();
    std::string generate_stacked_colgrp_mux();

    std::string generate_cell_row_8t();
    std::string generate_array_8t();
    std::string generate_colgrp_8t();
    std::string generate_stacked_colgrp_8t();

    std::string generate_spice_content(bool single_port);
};

} // namespace OpenFinRAM

#endif // SPICE_GENERATOR_HPP
