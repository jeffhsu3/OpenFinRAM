#ifndef SPICE_CONVERTER_HPP
#define SPICE_CONVERTER_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdint>

/**
 * Configuration for Verilog to SPICE conversion
 */
struct SpiceConversionConfig {
    std::string verilog_path;
    std::string syn_path;
    std::string netlist_v;      // Input Verilog netlist
    std::string netlist_sp;     // Output SPICE netlist
    std::string cdl_file;       // CDL library path
    
    // Constructor with defaults
    SpiceConversionConfig(
        const std::string& verilog_p = "./verilog",
        const std::string& syn_p = "./verilog"
    ) : verilog_path(verilog_p),
        syn_path(syn_p),
        netlist_v(syn_p + "/netlist.v"),
        netlist_sp(syn_p + "/netlist.sp"),
        cdl_file("~/asap7/asap7sc7p5t_28/CDL/LVS/asap7sc7p5t_28_R.cdl") {}
};

/**
 * Converts Verilog netlist to SPICE using v2lvs and post-processes
 */
class SpiceConverter {
public:
    /**
     * Constructor
     * @param config Conversion configuration
     */
    explicit SpiceConverter(const SpiceConversionConfig& config);
    
    /**
     * Run the complete conversion flow
     * @return true if successful, false otherwise
     */
    bool convert_to_spice();
    
    /**
     * Run v2lvs tool to convert Verilog to SPICE
     * @return true if successful, false otherwise
     */
    bool run_v2lvs();
    
    /**
     * Post-process the generated SPICE netlist
     * @return true if successful, false otherwise
     */
    bool post_process_netlist();
    
    /**
     * Get configuration parameters as a string for logging
     */
    std::string get_config_string() const;
    
private:
    SpiceConversionConfig config_;
    std::map<std::string, std::vector<std::string>> subckt_dict_;
    
    /**
     * Check if file exists
     */
    bool file_exists(const std::string& path) const;
    
    /**
     * Merge continuation lines (lines starting with '+')
     * @param lines Input lines
     * @return Lines with continuations merged
     */
    std::vector<std::string> merge_continuation_lines(
        const std::vector<std::string>& lines) const;
    
    /**
     * Add VDD VSS to .SUBCKT definitions
     * @param lines Input lines
     * @return Lines with power added to SUBCKTs
     */
    std::vector<std::string> add_power_to_subckt(
        const std::vector<std::string>& lines) const;
    
    /**
     * Parse CDL file to extract subcircuit definitions
     * @return true if successful, false otherwise
     */
    bool parse_cdl_file();
    
    /**
     * Parse subcircuit definitions from SPICE netlist
     * @param lines Input lines
     */
    void parse_subckt_from_lines(const std::vector<std::string>& lines);
    
    /**
     * Expand $PINS format to explicit pin connections
     * @param lines Input lines with $PINS
     * @return Lines with $PINS expanded
     */
    std::vector<std::string> expand_pins(
        const std::vector<std::string>& lines);
    
    /**
     * Handle .CONNECT directives (commented connections)
     * @param lines Input lines
     * @return Lines with .CONNECT directives processed
     */
    std::vector<std::string> process_connect_directives(
        const std::vector<std::string>& lines) const;
    
    /**
     * Read file into vector of lines
     * @param filepath Path to file
     * @return Vector of lines
     */
    std::vector<std::string> read_file(const std::string& filepath) const;
    
    /**
     * Write lines to file
     * @param filepath Path to file
     * @param lines Lines to write
     * @return true if successful, false otherwise
     */
    bool write_file(const std::string& filepath,
                    const std::vector<std::string>& lines) const;
};

#endif // SPICE_CONVERTER_HPP
