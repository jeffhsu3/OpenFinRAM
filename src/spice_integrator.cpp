#include "spice_integrator.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <algorithm>
#include <regex>

#include "plog/Log.h"

#include "utils.hpp"

// Constructor
SpiceIntegrator::SpiceIntegrator(const SramIntegrationConfig& config)
    : config_(config) {}

SpiceIntegrator::SpiceIntegrator(const MainCliOptions& cli_options_)
    : cli_options_(cli_options_) {}

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

std::string SpiceIntegrator::generate_header(const std::string& ctrl_netlist_path, const std::string& datapath_netlist_path) const {
    std::ostringstream oss;
    oss << "* HSPICE Netlist for Complete SRAM\n"
        << "* Configuration: " << cli_options_.num_data_bits << "-bit data, "
        << get_addr_width(cli_options_) << "-bit address\n"
        << "* Wordlines: " << cli_options_.num_wls << " (top) + "
        << cli_options_.num_wls << " (bottom)\n"
        << "* Generated automatically by OpenFinRAM\n"
        << "*\n"
        << "* ===================================================================\n"
        << "* Includes\n"
        << "* ===================================================================\n"
        << ".INCLUDE \"" << ctrl_netlist_path << "\"\n"
        << ".INCLUDE \"" << datapath_netlist_path << "\"\n"
        << "\n";
    return oss.str();
}

std::string SpiceIntegrator::generate_subckt_header() const {
    std::ostringstream oss;
    
    oss << "* ===================================================================\n"
        << "* SRAM Top Module (" << cli_options_.num_data_bits << "-bit data)\n"
        << "* ===================================================================\n";
    
    if (cli_options_.single_port) {
        oss << "* Single-Port Configuration\n";

        // Build port list (updated to ce_n and we_n)
        oss << ".SUBCKT sram_x" << cli_options_.num_wls * 2
            << "x" << cli_options_.num_data_bits
            << "x" << cli_options_.num_banks
            << " vdd vss clk rst_n ce_n we_n oe_n";
        
        // Address ports
        for (uint64_t i = 0; i < get_addr_width(cli_options_); ++i) {
            oss << " A[" << i << "]";
        }
        
        // Data input ports
        for (uint64_t i = 0; i < cli_options_.num_data_bits; ++i) {
            oss << " D[" << i << "]";
        }
        
        // Data output ports
        for (uint64_t i = 0; i < cli_options_.num_data_bits; ++i) {
            oss << " Q[" << i << "]";
        }
    } else {
        oss << "* Dual-Port Configuration\n";

        oss << ".SUBCKT sram_x" << cli_options_.num_wls * 2
            << "x" << cli_options_.num_data_bits
            << "x" << cli_options_.num_banks
            << " vdd vss clk rst_n ce_n_A we_n_A oe_n_A"
            << "\n+";

        for (uint64_t i = 0; i < get_addr_width(cli_options_); ++i) {
            oss << " A_A[" << i << "]";
        }
        oss << "\n+";
        for (uint64_t i = 0; i < cli_options_.num_data_bits; ++i) {
            oss << " D_A[" << i << "]";
        }
        oss << "\n+";
        for (uint64_t i = 0; i < cli_options_.num_data_bits; ++i) {
            oss << " Q_A[" << i << "]";
        }
        oss << "\n+";

        oss << " ce_n_B oe_n_B"
            << "\n+";
        for (uint64_t i = 0; i < get_addr_width(cli_options_); ++i) {
            oss << " A_B[" << i << "]";
        }
        oss << "\n+";
        for (uint64_t i = 0; i < cli_options_.num_data_bits; ++i) {
            oss << " Q_B[" << i << "]";
        }
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
        
        for (int bank = 0; bank < cli_options_.num_banks; ++bank) {
            if (cli_options_.single_port) {
                for (uint64_t i = 0; i < cli_options_.num_wls; ++i) {
                    oss << " wlt[" << i + bank * cli_options_.num_wls << "]";
                }
                oss << "\n+";
                
                for (uint64_t i = 0; i < cli_options_.num_wls; ++i) {
                    oss << " wlb[" << i + bank * cli_options_.num_wls << "]";
                }
                oss << "\n+";
            } else {
                // Wordlines (top)
                for (uint64_t i = 0; i < cli_options_.num_wls; ++i) {
                    oss << " wlt_a[" << i + bank * cli_options_.num_wls << "]";
                }
                oss << "\n+";
                
                for (uint64_t i = 0; i < cli_options_.num_wls; ++i) {
                    oss << " wlt_b[" << i + bank * cli_options_.num_wls << "]";
                }
                oss << "\n+";

                // Wordlines (bottom)
                for (uint64_t i = 0; i < cli_options_.num_wls; ++i) {
                    oss << " wlb_a[" << i + bank * cli_options_.num_wls << "]";
                }
                oss << "\n+";

                for (uint64_t i = 0; i < cli_options_.num_wls; ++i) {
                    oss << " wlb_b[" << i + bank * cli_options_.num_wls << "]";
                }
                oss << "\n+";
            }
        }

        // Data IO
        if (cli_options_.single_port) {
            for (uint64_t i = 0; i < cli_options_.num_data_bits / 2; ++i) {
                oss << " D[" << i + top_bottom * (cli_options_.num_data_bits / 2) << "]";
            }
            oss << "\n+";

            for (uint64_t i = 0; i < cli_options_.num_data_bits / 2; ++i) {
                oss << " Q[" << i + top_bottom * (cli_options_.num_data_bits / 2) << "]";
            }
            oss << "\n+";
        } else {
            for (uint64_t i = 0; i < cli_options_.num_data_bits / 2; ++i) {
                oss << " D_A[" << i + top_bottom * (cli_options_.num_data_bits / 2) << "]";
            }
            oss << "\n+";

            for (uint64_t i = 0; i < cli_options_.num_data_bits / 2; ++i) {
                oss << " Q_A[" << i + top_bottom * (cli_options_.num_data_bits / 2) << "]";
            }
            oss << "\n+";

            for (uint64_t i = 0; i < cli_options_.num_data_bits / 2; ++i) {
                oss << " Q_B[" << i + top_bottom * (cli_options_.num_data_bits / 2) << "]";
            }
            oss << "\n+";
        }

        std::vector<std::string> ctrl_sigs;
        if (cli_options_.single_port) {
            ctrl_sigs = {
                "wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
                "blprechtn", "blprechbn"
            };
        } else {
            ctrl_sigs = {
                "wrena_A", "wrenan_A",
                "oeb_out_A", "oe_out_A", "oeb_out_B", "oe_out_B",
                "blprechtn_A", "blprechbn_A", "blprechtn_B", "blprechbn_B",
                "blprechn_rbl_A", "blprechn_rbl_B"
            };
        }
        for (const auto& sig : ctrl_sigs) {
            for (int bank = 0; bank < cli_options_.num_banks; ++bank) {
                oss << " " << sig << "[" << bank << "]";
            }

            oss << "\n+";
        }

        if (cli_options_.single_port) {
            // yseltn
            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yseltn[" << bank << "]";
            }
            oss << "\n+";

            // yselt
            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselt[" << bank << "]";
            }
            oss << "\n+";

            // yselbn
            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselbn[" << bank << "]";
            }
            oss << "\n+";

            // yselb
            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselb[" << bank << "]";
            }
            oss << "\n+";
        } else {
            // yseltn
            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yseltn_A[" << bank << "]";
            }
            oss << "\n+";

            // yselt
            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselt_A[" << bank << "]";
            }
            oss << "\n+";

            // yselbn
            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselbn_A[" << bank << "]";
            }
            oss << "\n+";

            // yselb
            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselb_A[" << bank << "]";
            }
            oss << "\n+";


            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yseltn_B[" << bank << "]";
            }
            oss << "\n+";

            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselt_B[" << bank << "]";
            }
            oss << "\n+";

            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselbn_B[" << bank << "]";
            }
            oss << "\n+";


            for (int bank = 0; bank < cli_options_.num_banks * 4; ++bank) {
                oss << " yselb_B[" << bank << "]";
            }
            oss << "\n+";
        }
        

        for (int bank = 0; bank < cli_options_.num_banks; ++bank) {
            oss << " RWLT_A[" << bank << "] RWLB_A[" << bank << "] RWLT_B[" << bank << "] RWLB_B[" << bank << "]";
            oss << "\n+";
        }
        
        // Control signals and power
        oss << " VDD VSS\n"
            << "+ stacked_colgrp_x"<< cli_options_.num_wls * 2 
            << "x" << cli_options_.num_data_bits / 2 << "x" << cli_options_.num_banks << "\n\n";
    }

    return oss.str();
}

std::string SpiceIntegrator::generate_footer() const {
    std::ostringstream oss;
    
    // oss << "Vvsswrite vsswrite vss DC 0\n";

    oss << "\n.ENDS sram_x" << cli_options_.num_wls * 2
        << "x" << cli_options_.num_data_bits
        << "x" << cli_options_.num_banks << "\n";
    
    return oss.str();
}

bool SpiceIntegrator::flatten_netlist(const std::string& input_path, const std::string& output_path) const {
    std::ifstream infile(input_path.c_str());
    if (!infile.is_open()) {
        LOGE << "  ✗ Error: Cannot open input netlist for flattening: " << input_path;
        return false;
    }

    std::ofstream outfile(output_path.c_str());
    if (!outfile.is_open()) {
        LOGE << "  ✗ Error: Cannot open output netlist for flattening: " << output_path;
        return false;
    }

    std::string cdl_path = join_path(get_current_dir_name(), "tech/cdl/asap7sc7p5t_28_R.cdl");
    std::ifstream cdl_file(cdl_path.c_str());
    if (!cdl_file.is_open()) {
        LOGE << "  ✗ Error: Cannot open CDL file for flattening: " << cdl_path;
        return false;
    }

    // First write CDL content
    std::string cdl_line;
    while (std::getline(cdl_file, cdl_line)) {
        outfile << cdl_line << "\n";
    }
    outfile << "\n";

    std::string line;
    while (std::getline(infile, line)) {
        // Check for .INCLUDE statements
        if (line.substr(0, 4) == ".inc" || line.substr(0, 4) == ".INC" ||
            line.substr(0, 8) == ".INCLUDE" || line.substr(0, 8) == ".include") {
                // Parse include path between quotes
                std::string include_path = line.substr(line.find_first_of("\"") + 1, line.find_last_of("\"") - line.find_first_of("\"") - 1);
                
                
                // Read all content
                std::ifstream inc_file(include_path.c_str());
                if (!inc_file.is_open()) {
                    LOGE << "  ✗ Error: Cannot open included file: " << include_path;
                    return false;
                }

                std::string inc_line;
                while (std::getline(inc_file, inc_line)) {
                    if (inc_line.substr(0, 8) == ".INCLUDE") {
                        continue; // Skip nested includes
                    }
                    if (inc_line == "*.BUSDELIMITER [ ") {
                        continue; // Skip bus delimiter lines
                    }
                    if (inc_line.find(".SUBCKT") != std::string::npos && inc_line.find("ctrl_decode") != std::string::npos) {
                        LOGD << "Add power ports to ctrl_decode instance";
                        outfile << inc_line << " VDD VSS\n";
                        continue;
                    }
                    outfile << inc_line << "\n";
                }
        } else {
            // Write line as is
            outfile << line << "\n";
        }
    }

    return true;
}

bool SpiceIntegrator::replace_chars_for_sis(const std::string& input_path, const std::string& output_path) const {
    std::ifstream infile(input_path.c_str());
    if (!infile.is_open()) {
        LOGE << "  ✗ Error: Cannot open input netlist for character replacement: " << input_path;
        return false;
    }

    std::ofstream outfile(output_path.c_str());
    if (!outfile.is_open()) {
        LOGE << "  ✗ Error: Cannot open output netlist for character replacement: " << output_path;
        return false;
    }

    std::string line;
    while (std::getline(infile, line)) {
        // Replace [ and ] with _ for SIS compatibility and to lowercase
        std::string modified_line = line;
        // std::replace(modified_line.begin(), modified_line.end(), '[', '_');
        // std::replace(modified_line.begin(), modified_line.end(), ']', ' ');
        // std::transform(modified_line.begin(), modified_line.end(), modified_line.begin(), ::tolower);

        // Replace state_A[0] and state_A[1] with state_A_0 and state_A_1
        if (line.find("state_A[0]") != std::string::npos) {
            modified_line = std::regex_replace(modified_line, std::regex("state_A\\[0\\]"), "state_A_0");
        }
        if (line.find("state_A[1]") != std::string::npos) {
            modified_line = std::regex_replace(modified_line, std::regex("state_A\\[1\\]"), "state_A_1");
        }

        outfile << modified_line << "\n";
    }

    return true;
}

bool SpiceIntegrator::integrate_sram() {
    std::string output_file_path = join_path(get_current_dir_name(), cli_options_.output_sp_name);

    LOGD << "\n" << std::string(70, '=');
    LOGD << "Integrating Control and Datapath";
    LOGD << std::string(70, '=');
    
    // Verify input files exist
    std::string ctrl_netlist_path = join_path(get_current_dir_name(), "tmp/innovus/netlist_for_lvs.sp");
    if (!file_exists(ctrl_netlist_path)) {
        LOGE << "  ✗ Error: Control netlist not found: " 
             << ctrl_netlist_path;
        return false;
    }
    
    std::string datapath_netlist_path = join_path(get_current_dir_name(), "sram_colgrp.sp");
    if (!file_exists(datapath_netlist_path)) {
        LOGE << "  ✗ Error: Datapath netlist not found: " 
             << datapath_netlist_path;
        return false;
    }
    
    LOGD << "  ▶ Merging netlists...";
    
    // Parse control circuit ports
    LOGD << "  ▶ Parsing control circuit ports...";
    std::vector<std::string> ctrl_ports = parse_ctrl_ports(ctrl_netlist_path);
    ctrl_ports.push_back("VDD");
    ctrl_ports.push_back("VSS");
    
    if (ctrl_ports.empty()) {
        LOGE << "  ✗ Error: Could not parse control ports";
        return false;
    }

    // Generate SPICE netlist
    LOGD << "\n  ▶ Generating integrated SPICE netlist...";
    
    std::ofstream outfile(output_file_path.c_str());
    if (!outfile.is_open()) {
        LOGE << "  ✗ Error: Cannot open output file " << output_file_path;
        return false;
    }
    
    // Write sections
    outfile << generate_header(ctrl_netlist_path, datapath_netlist_path);
    outfile << generate_subckt_header();
    outfile << generate_ctrl_instance(ctrl_ports);
    outfile << generate_datapath_instance();
    outfile << generate_footer();
    
    outfile.close();
    
    // Verify output
    if (!file_exists(output_file_path)) {
        LOGE << "  ✗ Error: Output file not created";
        return false;
    }
    
    LOGD << "✓ Integration completed → " << output_file_path;
    
    // Flatten the netlist to resolve includes and subcircuit definitions
    LOGD << "\n  ▶ Flattening netlist for LVS compatibility...";
    std::string flattened_output_path = join_path(get_current_dir_name(), "sram_flat.sp");
    if (!flatten_netlist(output_file_path, flattened_output_path)) {
        LOGE << "  ✗ Error: Netlist flattening failed";
        return false;
    }

    // Replace [] with _ for SIS compatibility
    std::string sis_ready_output_path = join_path(get_current_dir_name(), "sram_flat_sis.sp");
    if (!replace_chars_for_sis(flattened_output_path, sis_ready_output_path)) {
        LOGE << "  ✗ Error: Character replacement for SIS failed";
        return false;
    }
    
    return true;
}
