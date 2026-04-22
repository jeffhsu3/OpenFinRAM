#ifndef SPICE_GENERATOR_HPP
#define SPICE_GENERATOR_HPP

#include <string>
#include <vector>
#include <fstream>

#include "main_config_helpers.hpp"

namespace OpenFinRAM {
class SpiceGenerator {
public:
    SpiceGenerator(const MainCliOptions& config);
    
    // Generate complete SPICE netlist
    bool generate();
    
private:
    MainCliOptions config_;
    
    // Helper functions
    std::string format_ports(const std::vector<std::string>& ports, int max_per_line = 10);
    std::string create_subckt(const std::string& name, 
                              const std::vector<std::string>& ports,
                              const std::string& instances);
    
    // Generate different hierarchy levels
    std::string generate_cell_row();
    std::string generate_sramcol();
    std::string generate_array();
    std::string generate_colgrp();
    std::string generate_stacked_colgrp();
    std::string generate_stacked_colgrp_mux();

    // Generate different hierarchy levels with dual-port SRAM
    std::string generate_cell_row_8t();
    std::string generate_array_8t();
    std::string generate_colgrp_8t();
    std::string generate_stacked_colgrp_8t();
    
    // Full content generation
    std::string generate_spice_content(const bool& single_port);
};

} // namespace OpenFinRAM

#endif // SPICE_GENERATOR_HPP
