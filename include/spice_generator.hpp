#ifndef SPICE_GENERATOR_HPP
#define SPICE_GENERATOR_HPP

#include <string>
#include <vector>
#include <fstream>

#include "main_config_helpers.hpp"

namespace OpenFinRAM {

// ============================================================================
// SPICE Generator Configuration
// ============================================================================
struct SpiceConfig {
    int num_wordlines;      // Number of wordlines (WL)
    int num_colgrp;         // Number of column groups to stack
    int data_bits;          // Number of data bits (usually same as num_colgrp)
    int num_mux;            // Number of mux slices (horizontal stacks)
    std::string output_dir; // Output directory for SPICE files
    
    SpiceConfig() 
        : num_wordlines(16)
        , num_colgrp(8)
        , data_bits(8)
        , num_mux(1)
        , output_dir(".")
    {}
};

// ============================================================================
// SPICE Templates - Cell definitions
// ============================================================================
class SpiceTemplates {
public:
    static std::string get_cell_6t();
    static std::string get_cell_8t();
    static std::string get_replica_cell_8t();
    static std::string get_dummy_cell();
    static std::string get_dummy_cell_8t();
    static std::string get_dummy_topbot_v1();
    static std::string get_dummy_topbot_v2();
    static std::string get_prech_v1();
    static std::string get_prech_8t_v1();
    static std::string get_prech_v2();
    static std::string get_prech_8t_v2();
    static std::string get_prech_ymux();
    static std::string get_wrasst_prech_ymux_x8_sram_8t();
    static std::string get_write_driver();
    static std::string get_sense_amp();
    static std::string get_skewed_inv();
    static std::string get_io_nand();
    static std::string get_tbuf();
    static std::string get_iocolgrp();
};

// ============================================================================
// SPICE Generator - Main class
// ============================================================================
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
    
    // Full content generation
    std::string generate_spice_content();
};

} // namespace OpenFinRAM

#endif // SPICE_GENERATOR_HPP
