#include "spice_integrator.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <algorithm>
#include <regex>

// Constructor
SpiceIntegrator::SpiceIntegrator(const SramIntegrationConfig& config)
    : config_(config) {}

std::string SpiceIntegrator::get_config_string() const {
    std::ostringstream oss;
    oss << "SRAM Integration Configuration:\n"
        << "  Control Netlist: " << config_.ctrl_netlist << "\n"
        << "  Datapath Netlist: " << config_.datapath_netlist << "\n"
        << "  Output Netlist: " << config_.output_netlist << "\n"
        << "  Address Width: " << config_.addr_width << " bits\n"
        << "  Data Width: " << config_.data_width << " bits\n"
        << "  Wordlines: " << config_.num_wordlines << " (top and bottom)";
    return oss.str();
}

bool SpiceIntegrator::file_exists(const std::string& path) const {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

std::vector<std::string> SpiceIntegrator::parse_ctrl_ports(
    const std::string& netlist_sp) {
    
    std::vector<std::string> ports;
    
    std::ifstream file(netlist_sp.c_str());
    if (!file.is_open()) {
        std::cerr << "  ✗ Error: Cannot open netlist " << netlist_sp << std::endl;
        return ports;
    }
    
    std::string line;
    bool in_subckt = false;
    std::string subckt_section;
    
    // Read file and find ctrl_decode subcircuit
    while (std::getline(file, line)) {
        // Check if line starts .SUBCKT ctrl_decode
        if (line.substr(0, 7) == ".SUBCKT" || line.substr(0, 7) == ".subckt") {
            if (line.find("ctrl_decode") != std::string::npos ||
                line.find("CTRL_DECODE") != std::string::npos) {
                in_subckt = true;
                
                // Extract ports from this line
                std::istringstream iss(line);
                std::string token;
                int token_count = 0;
                
                while (iss >> token) {
                    token_count++;
                    if (token_count > 2) { // Skip ".SUBCKT ctrl_decode"
                        // Remove any trailing characters
                        if (token.back() == '\n' || token.back() == '\r') {
                            token.pop_back();
                        }
                        if (!token.empty() && token[0] != '*') {
                            ports.push_back(token);
                        }
                    }
                }
            }
        } else if (in_subckt && line.find(".ENDS") != std::string::npos) {
            // End of subcircuit found
            break;
        } else if (in_subckt && !line.empty()) {
            // Continuation line with more ports (usually starting with +)
            if (line[0] == '+' && line.find("=") == std::string::npos) {
                // Remove the continuation character
                std::string port_line = line.substr(1);
                std::istringstream iss(port_line);
                std::string token;
                
                while (iss >> token) {
                    // Skip comments and empty tokens
                    if (!token.empty() && token[0] != '*' && token[0] != '$') {
                        ports.push_back(token);
                    }
                }
            }
        }
    }
    
    file.close();
    
    std::cout << "  ✓ Parsed " << ports.size() << " ports from ctrl_decode" << std::endl;
    if (ports.size() > 5) {
        std::cout << "    First few ports: " << ports[0];
        for (size_t i = 1; i < std::min(size_t(5), ports.size()); ++i) {
            std::cout << ", " << ports[i];
        }
        std::cout << ", ..." << std::endl;
    }
    
    return ports;
}

std::map<std::string, std::string> SpiceIntegrator::build_port_mapping() const {
    std::map<std::string, std::string> port_map;
    
    // Basic control signals (updated to ce_n and we_n)
    port_map["clk"] = "clk";
    port_map["rst_n"] = "rst_n";
    port_map["ce_n"] = "ce_n";
    port_map["we_n"] = "we_n";
    port_map["saprechn"] = "saprechn";
    port_map["sae"] = "sae";
    port_map["blprechtn"] = "blprechtn";
    port_map["blprechbn"] = "blprechbn";
    port_map["wrena"] = "wrena";
    port_map["wrenan"] = "wrenan";
    
    // Address ports
    for (uint64_t i = 0; i < config_.addr_width; ++i) {
        std::ostringstream key, val;
        key << "A[" << i << "]";
        val << "A[" << i << "]";
        port_map[key.str()] = val.str();
    }
    
    // Wordline ports
    for (uint64_t i = 0; i < config_.num_wordlines; ++i) {
        std::ostringstream key;
        key << "wlt[" << i << "]";
        port_map[key.str()] = key.str();
        
        key.str("");
        key << "wlb[" << i << "]";
        port_map[key.str()] = key.str();
    }
    
    // Y-select ports
    for (int i = 0; i < 4; ++i) {
        std::ostringstream key;
        key << "yselt[" << i << "]";
        port_map[key.str()] = key.str();
        
        key.str("");
        key << "yseltn[" << i << "]";
        port_map[key.str()] = key.str();
        
        key.str("");
        key << "yselb[" << i << "]";
        port_map[key.str()] = key.str();
        
        key.str("");
        key << "yselbn[" << i << "]";
        port_map[key.str()] = key.str();
    }
    
    return port_map;
}

std::string SpiceIntegrator::generate_header() const {
    std::ostringstream oss;
    oss << "* HSPICE Netlist for Complete SRAM\n"
        << "* Configuration: " << config_.data_width << "-bit data, "
        << config_.addr_width << "-bit address\n"
        << "* Wordlines: " << config_.num_wordlines << " (top) + "
        << config_.num_wordlines << " (bottom)\n"
        << "* Generated automatically by OpenFinRAM\n"
        << "*\n"
        << "* ===================================================================\n"
        << "* Includes\n"
        << "* ===================================================================\n"
        << ".INCLUDE \"" << config_.ctrl_netlist << "\"\n"
        << ".INCLUDE \"" << config_.datapath_netlist << "\"\n"
        << "\n";
    return oss.str();
}

std::string SpiceIntegrator::generate_subckt_header() const {
    std::ostringstream oss;
    
    oss << "* ===================================================================\n"
        << "* SRAM Top Module (" << config_.data_width << "-bit data)\n"
        << "* ===================================================================\n";
    
    // Build port list (updated to ce_n and we_n)
    oss << ".SUBCKT sram_x" << config_.num_wordlines * 2
        << "x" << config_.data_width << " vdd vss clk rst_n ce_n we_n oe_n";
    
    // Address ports
    for (uint64_t i = 0; i < config_.addr_width; ++i) {
        oss << " A[" << i << "]";
    }
    
    // Data input ports
    for (uint64_t i = 0; i < config_.data_width; ++i) {
        oss << " D[" << i << "]";
    }
    
    // Data output ports
    for (uint64_t i = 0; i < config_.data_width; ++i) {
        oss << " Q[" << i << "]";
    }
    
    oss << "\n\n";
    return oss.str();
}

std::string SpiceIntegrator::generate_ctrl_instance(
    const std::vector<std::string>& ctrl_ports) const {
    
    std::ostringstream oss;
    
    oss << "** Instantiate Control Decoder\n"
        << "** Port order matched from synthesized netlist.sp\n"
        << "Xctrl";
    
    auto port_map = build_port_mapping();
    
    int col_count = 0;
    for (const auto& port : ctrl_ports) {
        // Try to find mapping
        std::string connection = port;
        
        // Check lowercase version
        std::string port_lower = port;
        std::transform(port_lower.begin(), port_lower.end(), 
                      port_lower.begin(), ::tolower);
        
        if (port_map.find(port_lower) != port_map.end()) {
            connection = port_map[port_lower];
        } else if (port_map.find(port) != port_map.end()) {
            connection = port_map[port];
        }
        
        // Line wrapping every 8 ports
        if (col_count > 0 && col_count % 8 == 0) {
            oss << "\n+";
        }
        oss << " " << connection;
        col_count++;
    }
    
    oss << " ctrl_decode\n\n";
    return oss.str();
}

std::string SpiceIntegrator::generate_datapath_instance() const {
    std::ostringstream oss;
    
    for (int top_bottom = 0; top_bottom < 2; ++top_bottom) {

        oss << "** Instantiate SRAM Datapath\n"
            << "Xdata_" << (top_bottom == 0 ? "top " : "bottom ");
        
        for (int mux = 0; mux < config_.num_mux; ++mux) {
            // Wordlines (top)
            int line_count = 0;
            for (uint64_t i = 0; i < config_.num_wordlines; ++i) {
                // if (line_count > 0 && line_count % 8 == 0) {
                //     oss << "\n+";
                // }
                oss << " wlt[" << i + mux * config_.num_wordlines << "]";
                // line_count++;
            }
            oss << "\n+";
            
            // Wordlines (bottom)
            for (uint64_t i = 0; i < config_.num_wordlines; ++i) {
                // if (line_count > 0 && line_count % 8 == 0) {
                //     oss << "\n+";
                // }
                oss << " wlb[" << i + mux * config_.num_wordlines << "]";
                line_count++;
            }
            oss << "\n+";
        }

        for (int mux = 0; mux < config_.num_mux; ++mux) {
            // yseltn
            for (int i = 0; i < 4; ++i) {
                oss << " yseltn[" << i + mux * 4 << "]";
                // line_count++;
            }
            oss << "\n+";

            // yselt
            for (int i = 0; i < 4; ++i) {
                oss << " yselt[" << i + mux * 4 << "]";
            }
            oss << "\n+";

            // yselbn
            for (int i = 0; i < 4; ++i) {
                oss << " yselbn[" << i + mux * 4 << "]";
            }
            oss << "\n+";

            // yselb
            for (int i = 0; i < 4; ++i) {
                oss << " yselb[" << i + mux * 4 << "]";
            }
            oss << "\n+";
        }
        
        // Data input
        for (uint64_t i = 0; i < config_.data_width / 2; ++i) {
            if (i > 0 && i % 8 == 0) {
                oss << "\n+";
            }
            oss << " D[" << i + top_bottom * (config_.data_width / 2) << "]";
        }
        oss << "\n+";
        
        // Data output
        for (uint64_t i = 0; i < config_.data_width / 2; ++i) {
            if (i > 0 && i % 8 == 0) {
                oss << "\n+";
            }
            oss << " Q[" << i + top_bottom * (config_.data_width / 2) << "]";
        }
        oss << "\n+";

        std::vector<std::string> ctrl_sigs = {
            "wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
            "blprechtn", "blprechbn"
        };
        for (const auto& sig : ctrl_sigs) {
            for (int mux = 0; mux < config_.num_mux; ++mux) {
                oss << " " << sig << "[" << mux << "]";
            }

            oss << "\n+";
        }
        
        // Control signals and power
        oss << " VDD VSS\n"
            << "+ stacked_colgrp_x"<< config_.num_wordlines * 2 
            << "x" << config_.data_width / 2 << "x" << config_.num_mux << "\n\n";
    }

    return oss.str();
}

std::string SpiceIntegrator::generate_footer() const {
    std::ostringstream oss;
    
    // oss << "Vvsswrite vsswrite vss DC 0\n";

    oss << "\n.ENDS sram_x" << config_.num_wordlines * 2
        << "x" << config_.data_width << "\n";
    
    return oss.str();
}

std::string SpiceIntegrator::integrate_sram(const std::string& output_file) {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Integrating Control and Datapath" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    // Verify input files exist
    if (!file_exists(config_.ctrl_netlist)) {
        std::cerr << "  ✗ Error: Control netlist not found: " 
                  << config_.ctrl_netlist << std::endl;
        return "";
    }
    
    if (!file_exists(config_.datapath_netlist)) {
        std::cerr << "  ✗ Error: Datapath netlist not found: " 
                  << config_.datapath_netlist << std::endl;
        return "";
    }
    
    std::cout << "  ▶ Merging netlists..." << std::endl;
    
    // Parse control circuit ports
    std::cout << "\n  ▶ Parsing control circuit ports..." << std::endl;
    std::vector<std::string> ctrl_ports = parse_ctrl_ports(config_.ctrl_netlist);
    
    if (ctrl_ports.empty()) {
        std::cerr << "  ✗ Error: Could not parse control ports" << std::endl;
        return "";
    }
    
    // Generate output file path
    std::string output_path = output_file;
    // If output_file is just a filename, prepend the directory path
    if (output_file.find('/') == std::string::npos &&
        output_file.find('\\') == std::string::npos) {
        // Extract directory from datapath_netlist
        size_t last_slash = config_.datapath_netlist.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            output_path = config_.datapath_netlist.substr(0, last_slash + 1) + output_file;
        }
    }
    
    // Generate SPICE netlist
    std::cout << "\n  ▶ Generating integrated SPICE netlist..." << std::endl;
    
    std::ofstream outfile(output_path.c_str());
    if (!outfile.is_open()) {
        std::cerr << "  ✗ Error: Cannot open output file " << output_path << std::endl;
        return "";
    }
    
    // Write sections
    outfile << generate_header();
    outfile << generate_subckt_header();
    outfile << generate_ctrl_instance(ctrl_ports);
    outfile << generate_datapath_instance();
    outfile << generate_footer();
    
    outfile.close();
    
    // Verify output
    if (!file_exists(output_path)) {
        std::cerr << "  ✗ Error: Output file not created" << std::endl;
        return "";
    }
    
    std::cout << "\n  ✓ Integration completed" << std::endl;
    std::cout << "  → " << output_path << std::endl;
    std::cout << "\n  SRAM Configuration:" << std::endl;
    std::cout << "    Address bits: " << config_.addr_width << std::endl;
    std::cout << "    Data bits: " << config_.data_width << std::endl;
    std::cout << "    Total addresses: " << (1UL << config_.addr_width) << std::endl;
    
    return output_path;
}
