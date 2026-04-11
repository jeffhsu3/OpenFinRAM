#include "pex_runner.hpp"
#include "plog/Log.h"

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace OpenFinRAM {

PEXRunner::PEXRunner(const Config& config) 
    : config_(config) 
{
    run_dir_ = config_.output_dir + "/pex";
    svdb_dir_ = run_dir_ + "/svdb";
}

bool PEXRunner::create_directories() {
    // 創建 run_{cell_name} 目錄
    if (mkdir(run_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
        LOGE << "Failed to create directory: " << run_dir_;
        return false;
    }
    
    // 創建 svdb 目錄
    if (mkdir(svdb_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
        LOGE << "Failed to create svdb directory: " << svdb_dir_;
        return false;
    }
    
    LOGI << "Created directories: " << run_dir_;
    return true;
}

bool PEXRunner::generate_svrf_file() {
    std::string svrf_path = run_dir_ + "/_run_control.svrf";
    std::ofstream svrf_file(svrf_path);
    
    if (!svrf_file.is_open()) {
        LOGE << "Failed to create SVRF file: " << svrf_path;
        return false;
    }
    
    // 生成 SVRF 內容
    svrf_file << "// [Input Settings]\n";
    svrf_file << "LAYOUT PATH \"" << config_.gds_path << "\"\n";
    svrf_file << "LAYOUT SYSTEM GDSII\n";
    svrf_file << "LAYOUT PRIMARY \"" << config_.cell_name << "\"\n";
    svrf_file << "\n";
    
    svrf_file << "// [Source Settings]\n";
    svrf_file << "SOURCE PATH \"" << config_.spice_path << "\"\n";
    svrf_file << "SOURCE SYSTEM SPICE\n";
    svrf_file << "SOURCE PRIMARY \"" << config_.cell_name << "\"\n";
    svrf_file << "\n";
    
    svrf_file << "// [Output Settings]\n";
    svrf_file << "LVS REPORT \"" << config_.cell_name << ".lvs.report\"\n";
    svrf_file << "PEX NETLIST \"" << config_.cell_name << ".pex.netlist\" ELDO SOURCENAMES\n";
    svrf_file << "\n";
    
    svrf_file << "// [Database Settings]\n";
    svrf_file << "MASK SVDB DIRECTORY \"svdb\" QUERY XRC\n";
    svrf_file << "\n";
    
    svrf_file << "// [LVS Options]\n";
    svrf_file << "LVS POWER NAME \"VDD!\" \"vdd!\"\n";
    svrf_file << "LVS GROUND NAME \"VSS!\" \"vss!\"\n";
    svrf_file << "LVS RECOGNIZE GATES ALL\n";
    svrf_file << "DRC ICSTATION YES\n";
    svrf_file << "\n";
    
    svrf_file << "// [Include PDK Rules]\n";
    svrf_file << "INCLUDE \"" << config_.pdk_rule_file << "\"\n";
    
    svrf_file.close();
    
    LOGI << "Generated SVRF file: " << svrf_path;
    return true;
}

bool PEXRunner::generate_tcl_script() {
    std::string tcl_path = run_dir_ + "/run_pex.tcl";
    std::ofstream tcl_file(tcl_path);
    
    if (!tcl_file.is_open()) {
        LOGE << "Failed to create TCL script: " << tcl_path;
        return false;
    }
    
    tcl_file << "#!/bin/tcsh\n\n";
    tcl_file << "# Auto-generated PEX TCL script\n";
    tcl_file << "# Cell: " << config_.cell_name << "\n\n";
    
    tcl_file << "set CELL = " << config_.cell_name << "\n";
    tcl_file << "set TURBO_COUNT = " << config_.turbo_count << "\n\n";
    
    tcl_file << "echo \"==========================================\"\n";
    tcl_file << "echo \"Starting PEX for: $CELL\"\n";
    tcl_file << "echo \"==========================================\"\n\n";
    
    tcl_file << "# Step 1: LVS\n";
    tcl_file << "echo \"[Step 1/3] Running LVS...\"\n";
    tcl_file << "calibre -lvs -hier -spice svdb/$CELL.sp -nowait _run_control.svrf > log_lvs.txt\n\n";
    
    tcl_file << "if ( ! -e \"svdb/$CELL.phdb\" ) then\n";
    tcl_file << "    echo \"Error: LVS PHDB creation failed. Check log_lvs.txt\"\n";
    tcl_file << "    exit 1\n";
    tcl_file << "endif\n\n";
    
    tcl_file << "# Step 2: PDB Extraction\n";
    tcl_file << "echo \"[Step 2/3] Running PEX PDB extraction...\"\n";
    tcl_file << "calibre -xrc -pdb -rcc -turbo $TURBO_COUNT -nowait _run_control.svrf > log_pdb.txt\n\n";
    
    tcl_file << "# Step 3: FMT (Formatter)\n";
    tcl_file << "echo \"[Step 3/3] Running PEX Formatting...\"\n";
    tcl_file << "calibre -xrc -fmt -all -nowait _run_control.svrf > log_fmt.txt\n\n";
    
    tcl_file << "if ( -e \"$CELL.pex.netlist\" ) then\n";
    tcl_file << "    echo \"SUCCESS: Output Netlist created at $CELL.pex.netlist\"\n";
    tcl_file << "else\n";
    tcl_file << "    echo \"ERROR: FMT failed. Check log_fmt.txt\"\n";
    tcl_file << "    exit 1\n";
    tcl_file << "endif\n\n";
    
    tcl_file << "echo \"PEX Completed.\"\n";
    
    tcl_file.close();
    
    // 設置可執行權限
    chmod(tcl_path.c_str(), 0755);
    
    LOGI << "Generated TCL script: " << tcl_path;
    return true;
}

bool PEXRunner::execute_command(const std::string& command, const std::string& log_file) {
    std::string full_command = command;
    if (!log_file.empty()) {
        full_command += " > " + log_file + " 2>&1";
    }
    
    LOGI << "Executing: " << command;
    int ret = system(full_command.c_str());
    
    if (ret != 0) {
        LOGE << "Command failed with return code: " << ret;
        if (!log_file.empty()) {
            LOGE << "Check log file: " << log_file;
        }
        return false;
    }
    
    return true;
}

bool PEXRunner::execute_lvs() {
    LOGI << "[Step 1/3] Running LVS...";
    
    std::string command = "tcsh -c 'cd " + run_dir_ + " && calibre -lvs -hier -spice svdb/" 
                         + config_.cell_name + ".sp -nowait _run_control.svrf'";
    std::string log_file = run_dir_ + "/log_lvs.txt";
    
    if (!execute_command(command, log_file)) {
        return false;
    }
    
    // 檢查 PHDB 是否創建成功
    std::string phdb_path = svdb_dir_ + "/" + config_.cell_name + ".phdb";
    if (access(phdb_path.c_str(), F_OK) != 0) {
        LOGE << "LVS PHDB creation failed. Check " << log_file;
        return false;
    }
    
    LOGI << "LVS completed successfully";
    return true;
}

bool PEXRunner::execute_pdb() {
    LOGI << "[Step 2/3] Running PEX PDB extraction...";
    
    std::stringstream cmd;
    cmd << "tcsh -c 'cd " << run_dir_ << " && calibre -xrc -pdb -rcc -turbo " 
        << config_.turbo_count << " -nowait _run_control.svrf'";
    
    std::string log_file = run_dir_ + "/log_pdb.txt";
    
    if (!execute_command(cmd.str(), log_file)) {
        return false;
    }
    
    LOGI << "PDB extraction completed successfully";
    return true;
}

bool PEXRunner::execute_fmt() {
    LOGI << "[Step 3/3] Running PEX Formatting...";
    
    std::string command = "tcsh -c 'cd " + run_dir_ + " && calibre -xrc -fmt -all -nowait _run_control.svrf'";
    std::string log_file = run_dir_ + "/log_fmt.txt";
    
    if (!execute_command(command, log_file)) {
        return false;
    }
    
    // 檢查輸出 netlist 是否創建
    if (!check_output_exists()) {
        LOGE << "FMT failed: output netlist not created. Check " << log_file;
        return false;
    }
    
    LOGI << "PEX Formatting completed successfully";
    LOGI << "Output netlist: " << get_pex_netlist_path();
    return true;
}

bool PEXRunner::run() {
    LOGI << "==========================================";
    LOGI << "Starting PEX for cell: " << config_.cell_name;
    LOGI << "==========================================";
    LOGI << "Configuration:";
    LOGI << "  GDS Path: " << config_.gds_path;
    LOGI << "  SPICE Path: " << config_.spice_path;
    LOGI << "  Output Dir: " << config_.output_dir;
    LOGI << "  PDK Rule: " << config_.pdk_rule_file;
    LOGI << "  Turbo Count: " << config_.turbo_count;
    
    // 1. 創建目錄
    if (!create_directories()) {
        LOGE << "Failed to create directories";
        return false;
    }
    
    // 2. 生成 SVRF 文件
    if (!generate_svrf_file()) {
        LOGE << "Failed to generate SVRF file";
        return false;
    }
    
    // 3. 生成 TCL 腳本（可選，主要用於手動執行）
    if (!generate_tcl_script()) {
        LOGW << "Failed to generate TCL script (non-critical)";
    }
    
    // 4. 執行 LVS
    if (!execute_lvs()) {
        LOGE << "LVS failed";
        return false;
    }
    
    // 5. 執行 PDB 抽取
    if (!execute_pdb()) {
        LOGE << "PDB extraction failed";
        return false;
    }
    
    // 6. 執行 FMT 生成 netlist
    if (!execute_fmt()) {
        LOGE << "FMT failed";
        return false;
    }
    
    LOGI << "==========================================";
    LOGI << "PEX completed successfully!";
    LOGI << "Output: " << get_pex_netlist_path();
    LOGI << "==========================================";
    
    return true;
}

std::string PEXRunner::get_pex_netlist_path() const {
    return run_dir_ + "/" + config_.cell_name + ".pex.netlist";
}

bool PEXRunner::check_output_exists() const {
    return access(get_pex_netlist_path().c_str(), F_OK) == 0;
}

} // namespace OpenFinRAM
