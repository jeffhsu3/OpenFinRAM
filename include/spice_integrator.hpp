#ifndef SPICE_INTEGRATOR_HPP
#define SPICE_INTEGRATOR_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdint>

/**
 * Configuration for SRAM integration
 */
struct SramIntegrationConfig {
    // Netlist paths
    std::string ctrl_netlist;       // Control circuit SPICE netlist
    std::string datapath_netlist;   // Datapath SPICE netlist
    std::string output_netlist;     // Output integrated SPICE netlist
    
    // SRAM parameters
    uint64_t addr_width;            // Address bus width
    uint64_t data_width;            // Data bus width
    uint64_t num_wordlines;         // Number of wordlines (top and bottom)
    uint64_t num_mux;              // Number of MUX slices (horizontal stacks)
    
    // Constructor with defaults
    SramIntegrationConfig(
        uint64_t addr_w = 8,
        uint64_t data_w = 8,
        uint64_t num_wl = 32,
        uint64_t num_mux = 1
    ) : addr_width(addr_w),
        data_width(data_w),
        num_wordlines(num_wl),
        num_mux(num_mux),
        ctrl_netlist("./verilog/netlist.sp"),
        datapath_netlist("./verilog/sram_colgrp.sp"),
        output_netlist("./verilog/sram.sp") {}
};

/**
 * Integrates control circuit and datapath SPICE netlists into complete SRAM
 */
class SpiceIntegrator {
public:
    /**
     * Constructor
     * @param config Integration configuration
     */
    explicit SpiceIntegrator(const SramIntegrationConfig& config);
    
    /**
     * Run the complete integration flow
     * @return Path to integrated SPICE file, empty string if failed
     */
    std::string integrate_sram(const std::string& output_file = "sram.sp");
    
    /**
     * Get configuration parameters as a string for logging
     */
    std::string get_config_string() const;
    
private:
    SramIntegrationConfig config_;
    
    /**
     * Check if file exists
     */
    bool file_exists(const std::string& path) const;
    
    /**
     * Parse control circuit ports from netlist
     * Extracts port list from .SUBCKT ctrl_decode definition
     * @param netlist_sp Path to control netlist
     * @return Vector of port names in order
     */
    std::vector<std::string> parse_ctrl_ports(const std::string& netlist_sp);
    
    /**
     * Generate header section of SRAM netlist
     * @return Header string with includes
     */
    std::string generate_header() const;
    
    /**
     * Generate SRAM subcircuit definition header
     * @return SUBCKT definition line
     */
    std::string generate_subckt_header() const;
    
    /**
     * Build port mapping for controller instance
     * @return Port mapping map
     */
    std::map<std::string, std::string> build_port_mapping() const;
    
    /**
     * Generate controller instantiation
     * @param ctrl_ports Port list from ctrl_decode subcircuit
     * @return Controller instantiation string
     */
    std::string generate_ctrl_instance(const std::vector<std::string>& ctrl_ports) const;
    
    /**
     * Generate datapath instantiation
     * @return Datapath instantiation string
     */
    std::string generate_datapath_instance() const;
    
    /**
     * Generate footer section (power supply, .ENDS)
     * @return Footer string
     */
    std::string generate_footer() const;
};

#endif // SPICE_INTEGRATOR_HPP
