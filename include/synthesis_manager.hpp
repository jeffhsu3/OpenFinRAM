#ifndef SYNTHESIS_MANAGER_HPP
#define SYNTHESIS_MANAGER_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdint>

#include "capacitance_parser.hpp"
#include "main_config_helpers.hpp"

/**
 * Manages Design Compiler synthesis with parameterized designs
 */
class SynthesisManager {
public:
    /**
     * Constructor
     * @param cli_options CLI options for synthesis configuration
     */
    explicit SynthesisManager(const MainCliOptions& cli_options);
    
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
     * Predict and load capacitance values using statistical model
     * Based on bit_num and stacked parameters (avoids running PEX)
     * @return true if successful, false otherwise
     */
    bool predict_capacitance();
    
private:
    MainCliOptions cli_options_;

    std::string cur_path_;
    std::string rtl_path_;
    std::string db_path_;
    std::string syn_path_;
    std::string output_path_;
    std::map<std::string, double> pin_capacitances_;
    
    /**
     * Generate the parameter string for elaborate command
     * @return parameter string like "ADDR_WIDTH=8,NUM_WL=32,..."
     */
    std::string generate_parameter_string() const;
    
    /**
     * Generate complete syn.tcl content
     */
    std::string generate_tcl_content() const;
};

#endif // SYNTHESIS_MANAGER_HPP
