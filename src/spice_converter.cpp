#include "spice_converter.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <regex>

// Constructor
SpiceConverter::SpiceConverter(const SpiceConversionConfig& config)
    : config_(config) {}

std::string SpiceConverter::get_config_string() const {
    std::ostringstream oss;
    oss << "SPICE Conversion Configuration:\n"
        << "  Input Verilog: " << config_.netlist_v << "\n"
        << "  Output SPICE: " << config_.netlist_sp << "\n"
        << "  CDL Library: " << config_.cdl_file << "\n"
        << "  Synthesis Path: " << config_.syn_path;
    return oss.str();
}

bool SpiceConverter::file_exists(const std::string& path) const {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

std::vector<std::string> SpiceConverter::read_file(const std::string& filepath) const {
    std::vector<std::string> lines;
    std::ifstream file(filepath.c_str());
    
    if (!file.is_open()) {
        std::cerr << "  ✗ Error: Cannot open file " << filepath << std::endl;
        return lines;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    file.close();
    
    return lines;
}

bool SpiceConverter::write_file(const std::string& filepath,
                                const std::vector<std::string>& lines) const {
    std::ofstream file(filepath.c_str());
    
    if (!file.is_open()) {
        std::cerr << "  ✗ Error: Cannot open file for writing " << filepath << std::endl;
        return false;
    }
    
    for (const auto& line : lines) {
        file << line << "\n";
    }
    file.close();
    
    return true;
}

std::vector<std::string> SpiceConverter::merge_continuation_lines(
    const std::vector<std::string>& lines) const {
    
    std::vector<std::string> result;
    
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        
        if (!line.empty() && line[0] == '+') {
            // Continuation line - append to previous line
            if (!result.empty()) {
                result.back() += " " + line.substr(1);
            }
        } else {
            result.push_back(line);
        }
    }
    
    return result;
}

std::vector<std::string> SpiceConverter::add_power_to_subckt(
    const std::vector<std::string>& lines) const {
    
    std::vector<std::string> result;
    
    for (const auto& line : lines) {
        if (line.substr(0, 7) == ".SUBCKT" || line.substr(0, 7) == ".subckt") {
            // Add VDD VSS to the end of SUBCKT line
            std::string new_line = line;
            if (line.back() != '\n') {
                new_line += " VDD VSS";
            } else {
                new_line = line.substr(0, line.length() - 1) + " VDD VSS";
            }
            result.push_back(new_line);
        } else {
            result.push_back(line);
        }
    }
    
    return result;
}

bool SpiceConverter::parse_cdl_file() {
    // Expand home directory in path
    std::string cdl_path = config_.cdl_file;
    if (!cdl_path.empty() && cdl_path[0] == '~') {
        const char* home = getenv("HOME");
        if (home != nullptr) {
            cdl_path = std::string(home) + cdl_path.substr(1);
        }
    }
    
    std::cout << "  ▶ Parsing CDL file: " << cdl_path << std::endl;
    
    if (!file_exists(cdl_path)) {
        std::cerr << "  ✗ Warning: CDL file not found: " << cdl_path << std::endl;
        std::cerr << "    Continuing with netlist parsing only" << std::endl;
        return false;
    }
    
    std::vector<std::string> cdl_lines = read_file(cdl_path);
    
    for (const auto& line : cdl_lines) {
        if (line.substr(0, 7) == ".SUBCKT" || line.substr(0, 7) == ".subckt") {
            std::istringstream iss(line);
            std::string token;
            std::vector<std::string> tokens;
            
            while (iss >> token) {
                tokens.push_back(token);
            }
            
            if (tokens.size() >= 2) {
                std::string subckt_name = tokens[1];
                std::vector<std::string> pins(tokens.begin() + 2, tokens.end());
                subckt_dict_[subckt_name] = pins;
            }
        }
    }
    
    std::cout << "  ✓ Parsed " << subckt_dict_.size() << " subcircuits from CDL" << std::endl;
    return true;
}

void SpiceConverter::parse_subckt_from_lines(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (line.substr(0, 7) == ".SUBCKT" || line.substr(0, 7) == ".subckt") {
            std::istringstream iss(line);
            std::string token;
            std::vector<std::string> tokens;
            
            while (iss >> token) {
                tokens.push_back(token);
            }
            
            if (tokens.size() >= 2) {
                std::string subckt_name = tokens[1];
                std::vector<std::string> pins(tokens.begin() + 2, tokens.end());
                
                // Only add if not already in CDL dict
                if (subckt_dict_.find(subckt_name) == subckt_dict_.end()) {
                    subckt_dict_[subckt_name] = pins;
                }
            }
        }
    }
}

std::vector<std::string> SpiceConverter::expand_pins(
    const std::vector<std::string>& lines) {
    
    std::vector<std::string> result;
    int pin_replacements = 0;
    int pin_errors = 0;
    
    for (const auto& line : lines) {
        if (line.find("$PINS") != std::string::npos) {
            // Parse the line with $PINS
            std::istringstream iss(line);
            std::string instance_name, subckt_name, dummy;
            std::vector<std::string> pin_mappings;
            
            iss >> instance_name >> subckt_name >> dummy; // Skip "$PINS"
            
            std::string mapping;
            while (iss >> mapping) {
                pin_mappings.push_back(mapping);
            }
            
            // Look up subcircuit definition
            if (subckt_dict_.find(subckt_name) != subckt_dict_.end()) {
                const auto& subckt_pins = subckt_dict_[subckt_name];
                
                // Parse pin mappings (format: PIN=NET)
                std::map<std::string, std::string> pin_map;
                for (const auto& pmapping : pin_mappings) {
                    size_t eq_pos = pmapping.find('=');
                    if (eq_pos != std::string::npos) {
                        std::string pin = pmapping.substr(0, eq_pos);
                        std::string net = pmapping.substr(eq_pos + 1);
                        
                        // Convert to uppercase for comparison
                        std::transform(pin.begin(), pin.end(), pin.begin(), ::toupper);
                        pin_map[pin] = net;
                    }
                }
                
                // Build connection list
                std::string new_line = instance_name;
                for (const auto& pin : subckt_pins) {
                    std::string pin_upper = pin;
                    std::transform(pin_upper.begin(), pin_upper.end(), pin_upper.begin(), ::toupper);
                    
                    if (pin_map.find(pin_upper) != pin_map.end()) {
                        new_line += " " + pin_map[pin_upper];
                    } else if (pin == "VDD" || pin == "VSS") {
                        new_line += " " + pin;
                    } else {
                        std::cerr << "    ⚠ Warning: Pin " << pin << " not found in mapping for " 
                                  << instance_name << std::endl;
                        new_line += " " + pin;
                        pin_errors++;
                    }
                }
                
                new_line += " " + subckt_name;
                result.push_back(new_line);
                pin_replacements++;
            } else {
                std::cerr << "    ⚠ Warning: Subcircuit " << subckt_name 
                          << " not found in definitions" << std::endl;
                result.push_back(line);
                pin_errors++;
            }
        } else {
            result.push_back(line);
        }
    }
    
    if (pin_replacements > 0) {
        std::cout << "    Expanded " << pin_replacements << " instances with $PINS" << std::endl;
    }
    if (pin_errors > 0) {
        std::cout << "    ⚠ Encountered " << pin_errors << " pin mapping warnings" << std::endl;
    }
    
    return result;
}

std::vector<std::string> SpiceConverter::process_connect_directives(
    const std::vector<std::string>& lines) const {
    
    std::vector<std::string> result;
    int connect_directives = 0;
    
    for (const auto& line : lines) {
        if (line.substr(0, 9) == "*.CONNECT" || line.substr(0, 9) == "*.connect") {
            // Parse *.CONNECT net1 net2
            std::istringstream iss(line);
            std::string token, net1, net2;
            iss >> token >> net1 >> net2;  // Skip "*.CONNECT"
            
            if (!net1.empty() && !net2.empty()) {
                // Create buffer instance
                std::string instance = "X" + net1 + net2;
                std::string new_line = instance + " " + net2 + " VDD VSS " + net1 + " BUFx2_ASAP7_75t_R";
                result.push_back(new_line);
                connect_directives++;
            } else {
                result.push_back(line);
            }
        } else {
            result.push_back(line);
        }
    }
    
    if (connect_directives > 0) {
        std::cout << "    Processed " << connect_directives << " .CONNECT directives" << std::endl;
    }
    
    return result;
}

bool SpiceConverter::run_v2lvs() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Running v2lvs Conversion" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    // Check if input netlist exists
    if (!file_exists(config_.netlist_v)) {
        std::cerr << "  ✗ Error: Input netlist not found: " << config_.netlist_v << std::endl;
        return false;
    }
    
    // Expand home directory in CDL path
    std::string cdl_path = config_.cdl_file;
    if (!cdl_path.empty() && cdl_path[0] == '~') {
        const char* home = getenv("HOME");
        if (home != nullptr) {
            cdl_path = std::string(home) + cdl_path.substr(1);
        }
    }
    
    // Construct v2lvs command
    std::string cmd = "tcsh -c 'cd " + config_.syn_path + 
                      " && v2lvs -v netlist.v -s " + cdl_path + 
                      " -o netlist.sp'";
    
    std::cout << "  ▶ Running v2lvs..." << std::endl;
    std::cout << "    Input: " << config_.netlist_v << std::endl;
    std::cout << "    Output: " << config_.netlist_sp << std::endl;
    
    int result = std::system(cmd.c_str());
    
    if (result != 0) {
        std::cerr << "  ✗ v2lvs failed with exit code: " << result << std::endl;
        return false;
    }
    
    if (!file_exists(config_.netlist_sp)) {
        std::cerr << "  ✗ Error: Output netlist not generated" << std::endl;
        return false;
    }
    
    std::cout << "  ✓ v2lvs conversion completed" << std::endl;
    return true;
}

bool SpiceConverter::post_process_netlist() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Post-processing SPICE Netlist" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    if (!file_exists(config_.netlist_sp)) {
        std::cerr << "  ✗ Error: Netlist file not found: " << config_.netlist_sp << std::endl;
        return false;
    }
    
    // Step 1: Read file
    std::cout << "  ▶ Step 1: Reading netlist..." << std::endl;
    std::vector<std::string> lines = read_file(config_.netlist_sp);
    std::cout << "    Read " << lines.size() << " lines" << std::endl;
    
    // Step 2: Merge continuation lines
    std::cout << "  ▶ Step 2: Merging continuation lines..." << std::endl;
    lines = merge_continuation_lines(lines);
    std::cout << "    Result: " << lines.size() << " lines after merge" << std::endl;
    
    // Step 3: Add VDD VSS to SUBCKT definitions
    std::cout << "  ▶ Step 3: Adding power to SUBCKT definitions..." << std::endl;
    lines = add_power_to_subckt(lines);
    
    // Step 4: Parse CDL file
    std::cout << "  ▶ Step 4: Parsing CDL definitions..." << std::endl;
    parse_cdl_file();
    
    // Step 5: Parse subcircuits from netlist
    std::cout << "  ▶ Step 5: Parsing subcircuits from netlist..." << std::endl;
    parse_subckt_from_lines(lines);
    std::cout << "    Total " << subckt_dict_.size() << " subcircuits available" << std::endl;
    
    // Step 6: Expand $PINS format
    std::cout << "  ▶ Step 6: Expanding $PINS format..." << std::endl;
    lines = expand_pins(lines);
    
    // Step 7: Process .CONNECT directives
    std::cout << "  ▶ Step 7: Processing .CONNECT directives..." << std::endl;
    lines = process_connect_directives(lines);
    
    // Step 8: Write back to file
    std::cout << "  ▶ Step 8: Writing processed netlist..." << std::endl;
    if (!write_file(config_.netlist_sp, lines)) {
        std::cerr << "  ✗ Error: Failed to write netlist" << std::endl;
        return false;
    }
    
    std::cout << "  ✓ Post-processing completed" << std::endl;
    std::cout << "    Output: " << config_.netlist_sp << std::endl;
    
    return true;
}

bool SpiceConverter::convert_to_spice() {
    std::cout << "\n" << std::string(70, '#') << std::endl;
    std::cout << "# Verilog to SPICE Conversion Flow" << std::endl;
    std::cout << std::string(70, '#') << std::endl;
    
    std::cout << "\n" << get_config_string() << std::endl;
    
    // Step 1: Run v2lvs
    if (!run_v2lvs()) {
        std::cerr << "\n✗ Conversion failed at v2lvs execution step" << std::endl;
        return false;
    }
    
    // Step 2: Post-process netlist
    if (!post_process_netlist()) {
        std::cerr << "\n✗ Conversion failed at post-processing step" << std::endl;
        return false;
    }
    
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "✓ Verilog to SPICE Conversion Complete" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    return true;
}
