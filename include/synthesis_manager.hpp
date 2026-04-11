#ifndef SYNTHESIS_MANAGER_HPP
#define SYNTHESIS_MANAGER_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include "capacitance_parser.hpp"

/**
 * Configuration for parameterized Design Compiler synthesis
 */
struct SynthesisConfig {
    // Design parameters
    uint64_t addr_width;
    uint64_t num_wl;           // Number of wordlines
    uint64_t mux_ratio;        // MUX ratio (number of slices)
    
    // Timing parameters (delay chain counts)
    uint64_t delay_prech_cnt;
    uint64_t delay_wl_cnt;
    uint64_t delay_sense_cnt;
    uint64_t delay_write_cnt;
    
    // Paths
    std::string verilog_path;
    std::string syn_path;
    std::string output_path;
    
    // Capacitance values (optional)
    std::map<std::string, double> pin_capacitances;  // Pin name -> capacitance in pF
    
    // Constructor with defaults
    SynthesisConfig(
        uint64_t addr_w = 8,
        uint64_t wl = 32,
        uint64_t mux = 1,
        uint64_t dly_prech = 30,
        uint64_t dly_wl = 5,
        uint64_t dly_sense = 30,
        uint64_t dly_write = 10
    ) : addr_width(addr_w),
        num_wl(wl),
        mux_ratio(mux),
        delay_prech_cnt(dly_prech),
        delay_wl_cnt(dly_wl),
        delay_sense_cnt(dly_sense),
        delay_write_cnt(dly_write) {}
};

/**
 * Manages Design Compiler synthesis with parameterized designs
 */
class SynthesisManager {
public:
    /**
     * Constructor
     * @param config Synthesis configuration
     */
    explicit SynthesisManager(const SynthesisConfig& config);
    
    /**
     * Run the complete synthesis flow
     * @return true if successful, false otherwise
     */
    bool run_synthesis();
    
    /**
     * Generate syn.tcl script with current configuration
     * @return true if successful, false otherwise
     */
    bool generate_synthesis_script();
    
    /**
     * Execute Design Compiler with syn.tcl
     * @return true if successful, false otherwise
     */
    bool run_design_compiler();
    
    /**
     * Check if synthesis output is valid
     * @return true if netlist.v exists, false otherwise
     */
    bool verify_synthesis_output();
    
    /**
     * Fix assign statements in netlist by replacing with buffers
     * @return true if successful or no assigns found, false on error
     */
    bool fix_assign_statements();
    
    /**
     * Get configuration parameters as a string for logging
     */
    std::string get_config_string() const;
    
    /**
     * Load capacitance values from PEX report
     * @param rep_file_path Path to rep.txt file from PEX
     * @return true if successful, false otherwise
     */
    bool load_capacitance_from_pex(const std::string& rep_file_path);
    
    /**
     * Predict and load capacitance values using statistical model
     * Based on bit_num and stacked parameters (avoids running PEX)
     * @param bit_num Number of bits in configuration
     * @param stacked Stacked parameter value
     * @return true if successful, false otherwise
     */
    bool predict_capacitance(int bit_num, int stacked);
    
    /**
     * Set capacitance for a specific pin
     * @param pin_name Pin name (e.g., "wlt", "blprechtn")
     * @param cap_pf Capacitance in pF
     */
    void set_pin_capacitance(const std::string& pin_name, double cap_pf);
    
    /**
     * Check if a file exists
     * @param path Path to check
     * @return true if file exists, false otherwise
     */
    bool file_exists(const std::string& path) const;
    
private:
    SynthesisConfig config_;
    
    /**
     * Generate the parameter string for elaborate command
     * @return parameter string like "ADDR_WIDTH=8,NUM_WL=32,..."
     */
    std::string generate_parameter_string() const;
    
    /**
     * Generate complete syn.tcl content
     */
    std::string generate_tcl_content() const;
    
    /**
     * Create output directories if they don't exist
     */
    bool create_output_directories() const;
};

#endif // SYNTHESIS_MANAGER_HPP
