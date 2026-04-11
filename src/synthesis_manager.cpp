#include "synthesis_manager.hpp"
#include "capacitance_parser.hpp"
#include "capacitance_predictor.hpp"
#include "utils.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <iomanip>

// Constructor
SynthesisManager::SynthesisManager(const SynthesisConfig& config)
    : config_(config) {
    // Ensure paths are set
    if (config_.verilog_path.empty()) {
        config_.verilog_path = "./verilog";
    }
    if (config_.syn_path.empty()) {
        config_.syn_path = "./verilog";
    }
    if (config_.output_path.empty()) {
        config_.output_path = "./verilog";
    }
}

std::string SynthesisManager::generate_parameter_string() const {
    std::ostringstream oss;
    oss << "ADDR_WIDTH=" << config_.addr_width
        << ",NUM_WL=" << config_.num_wl
        << ",MUX_RATIO=" << config_.mux_ratio
        << ",DLY_PRECH_CNT=" << config_.delay_prech_cnt
        << ",DLY_WL_CNT=" << config_.delay_wl_cnt
        << ",DLY_SENSE_CNT=" << config_.delay_sense_cnt
        << ",DLY_WRITE_CNT=" << config_.delay_write_cnt;
    return oss.str();
}

std::string SynthesisManager::get_config_string() const {
    std::ostringstream oss;
    oss << "Synthesis Configuration:\n"
        << "  Address Width: " << config_.addr_width << " bits\n"
        << "  Wordlines (NUM_WL): " << config_.num_wl << "\n"
        << "  MUX Ratio: " << config_.mux_ratio << "\n"
        << "  Delay Precharge Count: " << config_.delay_prech_cnt << "\n"
        << "  Delay Wordline Count: " << config_.delay_wl_cnt << "\n"
        << "  Delay Sense Count: " << config_.delay_sense_cnt << "\n"
        << "  Delay Write Count: " << config_.delay_write_cnt << "\n"
        << "  Verilog Path: " << config_.verilog_path << "\n"
        << "  Synthesis Path: " << config_.syn_path << "\n"
        << "  Output Path: " << config_.output_path;
    return oss.str();
}

std::string SynthesisManager::generate_tcl_content() const {
    std::ostringstream oss;
    std::string cur_path = get_executable_directory();
    
    oss << "# Design Compiler synthesis script\n"
        << "# Generated automatically with parameterized design\n"
        << "# Configuration:\n"
        << "#   ADDR_WIDTH=" << config_.addr_width << "\n"
        << "#   NUM_WL=" << config_.num_wl << "\n"
        << "#   MUX_RATIO=" << config_.mux_ratio << "\n"
        << "#   DLY_PRECH_CNT=" << config_.delay_prech_cnt << "\n"
        << "#   DLY_WL_CNT=" << config_.delay_wl_cnt << "\n"
        << "#   DLY_SENSE_CNT=" << config_.delay_sense_cnt << "\n"
        << "#   DLY_WRITE_CNT=" << config_.delay_write_cnt << "\n\n"
        
        << "set_app_var search_path \"$search_path " << cur_path << "/tech/db " << config_.verilog_path << "\"\n"
        << "set_app_var target_library \"asap7sc7p5t_AO_RVT_TT.db asap7sc7p5t_INVBUF_RVT_TT.db asap7sc7p5t_OA_RVT_TT.db asap7sc7p5t_SEQ_RVT_TT.db asap7sc7p5t_SIMPLE_RVT_TT.db\"\n"
        << "set_app_var link_library \"* asap7sc7p5t_AO_RVT_TT.db asap7sc7p5t_INVBUF_RVT_TT.db asap7sc7p5t_OA_RVT_TT.db asap7sc7p5t_SEQ_RVT_TT.db asap7sc7p5t_SIMPLE_RVT_TT.db\"\n"
        << "\n"

        << "# Analyze and elaborate\n"
        << "analyze -f sverilog {sram_control.v delay_cell.v}\n"
        << "elaborate ctrl_decode -parameters \"" << generate_parameter_string() << "\"\n"
        << "rename_design [current_design] ctrl_decode\n"
        << "link\n\n"

        << "create_clock -name clk -period 1.0 [get_ports clk]\n"
        << "set_clock_uncertainty 0.1 [get_clocks clk]\n"
        << "set_input_transition 0.05 [all_inputs]\n\n"
        
        << "# Prevent delay cells from being optimized\n"
        << "set DELAY_CELLS [get_cells {*delay_*/*}]\n"
        << "set_dont_touch $DELAY_CELLS true\n\n"

        << "set_dont_use asap7sc7p5t_AO_RVT_TT/AOI211xp5_ASAP7_75t_R\n"
        << "set_dont_use asap7sc7p5t_OA_RVT_TT/OAI32xp33_ASAP7_75t_R\n"
        << "set_dont_use asap7sc7p5t_OA_RVT_TT/OAI221xp5_ASAP7_75t_R\n\n";
        // << "set_dont_touch u_vsswrite* true\n\n";
    
    // Add set_load commands based on capacitance values
    if (!config_.pin_capacitances.empty()) {
        oss << "# Output load settings (from PEX capacitance report)\n";
        
        // Map of signal names to their corresponding ports in the design
        // Note: The design uses lowercase port names
        std::map<std::string, std::string> signal_map;
        signal_map["WLT"] = "wlt";
        signal_map["WLB"] = "wlb";
        signal_map["BLPRECHTN"] = "blprechtn";
        signal_map["BLPRECHBN"] = "blprechbn";
        signal_map["YSELT"] = "yselt";
        signal_map["YSELB"] = "yselb";
        signal_map["YSELTN"] = "yseltn";
        signal_map["YSELBN"] = "yselbn";
        signal_map["SAE"] = "sae";
        signal_map["OEB_OUT"] = "oeb_out";
        signal_map["OE_OUT"] = "oe_out";
        signal_map["SAPRECHN"] = "saprechn";
        signal_map["WRENA"] = "wrena";
        signal_map["WRENAN"] = "wrenan";
        
        for (const auto& entry : config_.pin_capacitances) {
            const std::string& pin_name = entry.first;
            double cap_pf = entry.second;
            
            // Find the corresponding port name using exact match
            std::string port_name;
            for (const auto& mapping : signal_map) {
                if (pin_name == mapping.first) {
                    port_name = mapping.second;
                    break;
                }
            }
            
            if (!port_name.empty()) {
                // Give some margin (e.g., 50%) to the capacitance
                oss << "set_load " << std::fixed << std::setprecision(6) 
                    << (cap_pf * 1000) * 1.5 << " [get_ports " << port_name << "*]\n";
            }
        }
        oss << "\n";
    }
    
    oss << "set_fix_multiple_port_nets -all -buffer_constants [get_designs *] \n\n"
        << "# Compile and generate netlist\n"
        << "compile\n"
        << "report_qor > qor_report.txt\n"
        << "define_name_rules NO_CASE_CONFLICT -case_insensitive\n"
        << "change_names -rules NO_CASE_CONFLICT -hierarchy\n"
        << "change_names -rules verilog -hierarchy\n"
        << "write_file -hierarchy -format verilog -output ./netlist.v\n"
        << "exit\n";
    
    return oss.str();
}

bool SynthesisManager::file_exists(const std::string& path) const {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

bool SynthesisManager::create_output_directories() const {
    // Create syn_path directory
    std::string mkdir_cmd = "mkdir -p " + config_.syn_path;
    int result = std::system(mkdir_cmd.c_str());
    if (result != 0) {
        std::cerr << "  ✗ Error creating directory: " << config_.syn_path << std::endl;
        return false;
    }
    
    // Create output_path directory
    mkdir_cmd = "mkdir -p " + config_.output_path;
    result = std::system(mkdir_cmd.c_str());
    if (result != 0) {
        std::cerr << "  ✗ Error creating directory: " << config_.output_path << std::endl;
        return false;
    }
    
    return true;
}

bool SynthesisManager::generate_synthesis_script() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Generating Synthesis Script (syn.tcl)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    if (!create_output_directories()) {
        return false;
    }
    
    std::string syn_tcl_path = config_.syn_path + "/syn_auto.tcl";
    std::string tcl_content = generate_tcl_content();
    
    try {
        std::ofstream tcl_file(syn_tcl_path);
        if (!tcl_file.is_open()) {
            std::cerr << "  ✗ Error: Cannot open file for writing: " << syn_tcl_path << std::endl;
            return false;
        }
        
        tcl_file << tcl_content;
        tcl_file.close();
        
        std::cout << "  ✓ Generated synthesis script" << std::endl;
        std::cout << "  → " << syn_tcl_path << std::endl;
        std::cout << "\n  Parameters:" << std::endl;
        std::cout << "    ADDR_WIDTH=" << config_.addr_width << std::endl;
        std::cout << "    NUM_WL=" << config_.num_wl << std::endl;
        std::cout << "    MUX_RATIO=" << config_.mux_ratio << std::endl;
        std::cout << "    DLY_PRECH_CNT=" << config_.delay_prech_cnt << std::endl;
        std::cout << "    DLY_WL_CNT=" << config_.delay_wl_cnt << std::endl;
        std::cout << "    DLY_SENSE_CNT=" << config_.delay_sense_cnt << std::endl;
        std::cout << "    DLY_WRITE_CNT=" << config_.delay_write_cnt << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Error writing synthesis script: " << e.what() << std::endl;
        return false;
    }
}

bool SynthesisManager::run_design_compiler() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Running Design Compiler" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    std::string syn_tcl_path = config_.syn_path + "/syn_auto.tcl";
    
    // Check if script was generated
    if (!file_exists(syn_tcl_path)) {
        std::cerr << "  ✗ Error: syn_auto.tcl not found at " << syn_tcl_path << std::endl;
        std::cerr << "    Please run generate_synthesis_script() first" << std::endl;
        return false;
    }
    
    // Construct the Design Compiler command
    // Note: Adjust dc_shell path if needed for your environment
    std::string cmd = "tcsh -c 'cd " + config_.syn_path + " && dc_shell -f syn_auto.tcl -output_log_file syn_auto.log'";
    
    std::cout << "  ▶ Running: " << cmd << std::endl;
    
    int result = std::system(cmd.c_str());
    
    if (result != 0) {
        std::cerr << "  ✗ Design Compiler failed with exit code: " << result << std::endl;
        std::cerr << "    Check log file: " << config_.syn_path << "/syn_auto.log" << std::endl;
        return false;
    }
    
    std::cout << "  ✓ Design Compiler completed successfully" << std::endl;
    return true;
}

bool SynthesisManager::verify_synthesis_output() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Verifying Synthesis Output" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    std::string netlist_path = config_.syn_path + "/netlist.v";
    std::string log_path = config_.syn_path + "/syn_auto.log";
    
    if (!file_exists(netlist_path)) {
        std::cerr << "  ✗ Error: netlist.v not generated" << std::endl;
        
        if (file_exists(log_path)) {
            std::cerr << "\n  Checking log file for errors..." << std::endl;
            std::ifstream log_file(log_path.c_str());
            std::string line;
            int error_count = 0;
            while (std::getline(log_file, line) && error_count < 10) {
                if (line.find("Error") != std::string::npos ||
                    line.find("ERROR") != std::string::npos ||
                    line.find("error") != std::string::npos) {
                    std::cerr << "    " << line << std::endl;
                    error_count++;
                }
            }
        }
        return false;
    }
    
    // Get file size using stat
    struct stat stat_buf;
    if (stat(netlist_path.c_str(), &stat_buf) == 0) {
        std::cout << "  ✓ Netlist generated successfully" << std::endl;
        std::cout << "  → " << netlist_path << std::endl;
        std::cout << "  File size: " << stat_buf.st_size << " bytes" << std::endl;
        return true;
    } else {
        std::cerr << "  ✗ Error checking netlist file" << std::endl;
        return false;
    }
}

bool SynthesisManager::run_synthesis() {
    std::cout << "\n" << std::string(70, '#') << std::endl;
    std::cout << "# Parameterized Design Synthesis Flow" << std::endl;
    std::cout << std::string(70, '#') << std::endl;
    
    std::cout << "\n" << get_config_string() << std::endl;
    
    // Step 1: Generate synthesis script
    if (!generate_synthesis_script()) {
        std::cerr << "\n✗ Synthesis failed at script generation step" << std::endl;
        return false;
    }
    
    // Step 2: Run Design Compiler
    if (!run_design_compiler()) {
        std::cerr << "\n✗ Synthesis failed at Design Compiler execution step" << std::endl;
        return false;
    }
    
    // Step 3: Verify output
    if (!verify_synthesis_output()) {
        std::cerr << "\n✗ Synthesis failed at output verification step" << std::endl;
        return false;
    }
    
    // Step 4: Fix assign statements
    if (!fix_assign_statements()) {
        std::cerr << "\n✗ Synthesis failed at assign statement fixing step" << std::endl;
        return false;
    }
    
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "✓ Synthesis Complete" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    return true;
}

bool SynthesisManager::load_capacitance_from_pex(const std::string& rep_file_path) {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Loading Capacitance from PEX Report" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    if (!file_exists(rep_file_path)) {
        std::cerr << "  ✗ Error: PEX report file not found: " << rep_file_path << std::endl;
        return false;
    }
    
    OpenFinRAM::CapacitanceParser parser;
    if (!parser.parse(rep_file_path)) {
        std::cerr << "  ✗ Error: Failed to parse capacitance report" << std::endl;
        return false;
    }
    
    // Extract capacitance for key signals
    std::vector<std::string> signals = {
        "WLT", "WLB", "BLPRECHTN", "BLPRECHBN",
        "YSELT", "YSELB", "YSELTN", "YSELBN",
        "SAE", "SAPRECHN", "WRENA", "WRENAN"
    };
    
    config_.pin_capacitances.clear();
    
    std::cout << "\n  Extracted Capacitances:" << std::endl;
    
    for (const auto& signal : signals) {
        // Try to get average capacitance for bus signals (WLT, WLB, etc.)
        double avg_cap = parser.get_average_capacitance_pf(signal);
        double max_cap = parser.get_max_capacitance_pf(signal);
        
        if (avg_cap > 0.0) {
            // Use maximum capacitance for conservative design
            config_.pin_capacitances[signal] = max_cap;
            
            std::cout << "    " << std::left << std::setw(15) << signal << ": "
                     << std::scientific << std::setprecision(3) 
                     << "avg=" << avg_cap << " pF, "
                     << "max=" << max_cap << " pF "
                     << "(using max)" << std::endl;
        }
    }
    
    if (config_.pin_capacitances.empty()) {
        std::cerr << "  ✗ Warning: No matching capacitances found" << std::endl;
        return false;
    }
    
    std::cout << "\n  ✓ Loaded " << config_.pin_capacitances.size() 
              << " capacitance values" << std::endl;
    
    return true;
}

bool SynthesisManager::predict_capacitance(int bit_num, int stacked) {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Predicting Capacitance using Statistical Model" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    std::cout << "  Configuration: bit_num=" << bit_num 
              << ", stacked=" << stacked << std::endl;
    
    // 使用統計模型預測
    OpenFinRAM::CapacitancePredictor predictor;
    auto predictions = predictor.predict_all(bit_num, stacked);
    
    config_.pin_capacitances.clear();
    
    // 映射預測結果到對應的pin names
    // 預測器返回的key: WLT, YSELT, BLPRECHTN, WRENA, SAE
    std::map<std::string, std::vector<std::string>> signal_map = {
        {"WLT", {"WLT", "WLB"}},
        {"YSELT", {"YSELT", "YSELB", "YSELTN", "YSELBN"}},
        {"BLPRECHTN", {"BLPRECHTN", "BLPRECHBN"}},
        {"WRENA", {"WRENA", "WRENAN"}},
        {"SAE", {"SAE", "SAPRECHN", "OEB_OUT", "OE_OUT"}}
    };
    
    std::cout << "\n  Predicted Capacitances:" << std::endl;
    
    for (const auto& pred : predictions) {
        const std::string& metric = pred.first;
        double cap = pred.second;
        
        std::cout << "    " << std::left << std::setw(15) << metric << ": "
                  << std::fixed << std::setprecision(6) 
                  << cap << " pF" << std::endl;
        
        // 將預測值應用到相關的所有pin
        auto it = signal_map.find(metric);
        if (it != signal_map.end()) {
            for (const auto& pin : it->second) {
                config_.pin_capacitances[pin] = cap;
            }
        } else {
            config_.pin_capacitances[metric] = cap;
        }
    }
    
    std::cout << "\n  ✓ Predicted " << predictions.size() 
              << " capacitance values for " << config_.pin_capacitances.size()
              << " pins" << std::endl;
    std::cout << "  ℹ Using statistical model (no PEX required)" << std::endl;
    
    return true;
}

void SynthesisManager::set_pin_capacitance(const std::string& pin_name, double cap_pf) {
    config_.pin_capacitances[pin_name] = cap_pf;
}

bool SynthesisManager::fix_assign_statements() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "Checking and Fixing Assign Statements" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    std::string netlist_path = config_.syn_path + "/netlist.v";
    
    // Check if netlist exists
    if (!file_exists(netlist_path)) {
        std::cerr << "  ✗ Error: Netlist not found at " << netlist_path << std::endl;
        return false;
    }
    
    // Read the netlist file
    std::ifstream infile(netlist_path);
    if (!infile.is_open()) {
        std::cerr << "  ✗ Error: Cannot open netlist file: " << netlist_path << std::endl;
        return false;
    }
    
    std::vector<std::string> lines;
    std::string line;
    int assign_count = 0;
    int line_num = 0;
    std::vector<int> assign_line_numbers;
    
    // Read all lines and count assigns
    while (std::getline(infile, line)) {
        line_num++;
        lines.push_back(line);
        
        // Check if line contains "assign"
        size_t assign_pos = line.find("assign");
        if (assign_pos != std::string::npos) {
            // Make sure it's not in a comment
            size_t comment_pos = line.find("//");
            if (comment_pos == std::string::npos || assign_pos < comment_pos) {
                assign_count++;
                assign_line_numbers.push_back(line_num);
            }
        }
    }
    infile.close();
    
    if (assign_count == 0) {
        std::cout << "  ✓ No assign statements found - netlist is clean" << std::endl;
        return true;
    }
    
    std::cout << "  ⚠ Found " << assign_count << " assign statement(s)" << std::endl;
    std::cout << "  Converting to HB1xp67_ASAP7_75t_R buffers..." << std::endl;
    
    // Process each line and replace assigns with buffers
    int buffer_counter = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string& current_line = lines[i];
        size_t assign_pos = current_line.find("assign");
        
        if (assign_pos != std::string::npos) {
            // Make sure it's not in a comment
            size_t comment_pos = current_line.find("//");
            if (comment_pos != std::string::npos && assign_pos >= comment_pos) {
                continue;
            }
            
            // Parse the assign statement: "assign output = input;"
            // Pattern: assign <spaces> output <spaces> = <spaces> input <spaces> ;
            
            // Find the '=' position
            size_t equal_pos = current_line.find('=', assign_pos);
            if (equal_pos == std::string::npos) {
                std::cerr << "  ✗ Warning: Invalid assign at line " << (i+1) << std::endl;
                continue;
            }
            
            // Find the ';' position
            size_t semicolon_pos = current_line.find(';', equal_pos);
            if (semicolon_pos == std::string::npos) {
                std::cerr << "  ✗ Warning: No semicolon found at line " << (i+1) << std::endl;
                continue;
            }
            
            // Extract output signal (between "assign" and "=")
            std::string output = current_line.substr(assign_pos + 6, equal_pos - assign_pos - 6);
            // Trim whitespace
            size_t start = output.find_first_not_of(" \t");
            size_t end = output.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                output = output.substr(start, end - start + 1);
            }
            
            // Extract input signal (between "=" and ";")
            std::string input = current_line.substr(equal_pos + 1, semicolon_pos - equal_pos - 1);
            // Trim whitespace
            start = input.find_first_not_of(" \t");
            end = input.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                input = input.substr(start, end - start + 1);
            }
            
            // Get indentation from original line
            std::string indent = "";
            for (size_t j = 0; j < current_line.length(); j++) {
                if (current_line[j] == ' ' || current_line[j] == '\t') {
                    indent += current_line[j];
                } else {
                    break;
                }
            }
            
            // Generate unique buffer instance name
            std::ostringstream buf_name;
            buf_name << "u_buf_assign_" << buffer_counter++;
            
            // Create buffer instantiation
            std::ostringstream new_line;
            new_line << indent << "HB1xp67_ASAP7_75t_R " << buf_name.str() 
                     << " ( .A(" << input << "), .Y(" << output << ") );";
            
            // Replace the line
            current_line = new_line.str();
            
            std::cout << "  • Line " << (i+1) << ": " << output << " = " << input 
                      << " → " << buf_name.str() << std::endl;
        }
    }
    
    // Write the modified netlist back
    std::string backup_path = netlist_path + ".backup";
    
    // Create backup
    std::string cp_cmd = "cp " + netlist_path + " " + backup_path;
    int result = std::system(cp_cmd.c_str());
    if (result != 0) {
        std::cerr << "  ✗ Warning: Could not create backup file" << std::endl;
    } else {
        std::cout << "  ✓ Created backup: " << backup_path << std::endl;
    }
    
    // Write modified content
    std::ofstream outfile(netlist_path);
    if (!outfile.is_open()) {
        std::cerr << "  ✗ Error: Cannot write to netlist file: " << netlist_path << std::endl;
        return false;
    }
    
    for (const auto& l : lines) {
        outfile << l << "\n";
    }
    outfile.close();
    
    std::cout << "  ✓ Fixed " << assign_count << " assign statement(s)" << std::endl;
    std::cout << "  → Updated netlist: " << netlist_path << std::endl;
    
    return true;
}
