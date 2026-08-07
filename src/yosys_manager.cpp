#include "yosys_manager.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

#include "capacitance_parser.hpp"
#include "capacitance_predictor.hpp"
#include "plog/Log.h"
#include "utils.hpp"
#include "yosys_tcl_generator.hpp"

namespace {

// The four selectable physical delay stages must fit between opposite clock
// edges.  The longest TT routed chain is about 0.93 ns; a 2.5 ns cycle leaves
// 0.32 ns beyond that half-cycle delay for explicit uncertainty, interface
// budget, and route variation.  This replaces the impossible former 200 ps
// cycle and is a conservative implementation target, not a characterized
// macro frequency claim.
constexpr double kClockPeriodNs = 2.500;
constexpr double kClockUncertaintyNs = 0.050;
constexpr double kIoDelayNs = 0.100;
constexpr double kInputMinDelayNs = 0.020;
constexpr double kOutputMinDelayNs = -0.020;
constexpr double kInputSlewNs = 0.010;
constexpr double kMaxTransitionNs = 0.150;
constexpr double kMaxNetCapPf = 0.020;
constexpr double kArrayLoadMargin = 1.50;
constexpr double kDefaultAbcLoadFf = 3.898;
constexpr double kAbcDelayTargetPs = 140.0;

std::size_t count_occurrences(const std::string& text,
                              const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace

YosysManager::YosysManager(const MainCliOptions& cli_options)
    : cli_options_(cli_options) {
    cur_path_ = get_executable_directory();
    // Prefer CWD tech (repo root) if it exists; fallback to executable dir (supports `build` with `cp -r ../tech .`)
    std::string cwd_tech_sp = join_path(get_current_dir_name(), "tech/verilog_sp");
    if (directory_exists(cwd_tech_sp) || file_exists(join_path(cwd_tech_sp, "sram_control.v"))) {
        rtl_path_ = cwd_tech_sp;
        cur_path_ = get_current_dir_name();
    } else {
        rtl_path_ = join_path(cur_path_, "tech/verilog_sp");
    }
    // Syn path must match SpiceIntegrator's CWD-based tmp (repo/tmp) so downstream can find netlist
    syn_path_ = join_path(get_current_dir_name(), "tmp/syn_" + get_run_timestamp());

    if (!directory_exists(join_path(get_current_dir_name(), "tmp"))) {
        create_directory(join_path(get_current_dir_name(), "tmp"), nullptr);
    }
    if (!directory_exists(join_path(get_executable_directory(), "tmp"))) {
        create_directory(join_path(get_executable_directory(), "tmp"), nullptr);
    }
    if (!directory_exists(syn_path_)) {
        create_directory(syn_path_, nullptr);
    }
}

std::string YosysManager::generate_parameter_string() const {
    int addr_width = get_addr_width(cli_options_);
    std::ostringstream oss;
    oss << "ADDR_WIDTH=" << addr_width
        << ",NUM_WL=" << cli_options_.num_wls
        << ",NUM_BANK=" << cli_options_.num_banks;
    return oss.str();
}

std::string YosysManager::generate_yosys_script() const {
    OpenFinRAM::YosysTclGenerator gen;
    std::string platform = cli_options_.platform_path;
    std::string tech_lib = join_path(cur_path_, "tech/lib");
    return gen.generate_script(
        rtl_path_, syn_path_, generate_parameter_string(),
        get_addr_width(cli_options_),
        cli_options_.num_wls, cli_options_.num_banks,
        cli_options_.num_wl_buf, cli_options_.num_sae_buf,
        abc_output_load_ff(), kAbcDelayTargetPs,
        platform, tech_lib);
}

double YosysManager::abc_output_load_ff() const {
    double worst_load_ff = 0.0;
    for (const auto& entry : pin_capacitances_) {
        worst_load_ff = std::max(
            worst_load_ff, entry.second * 1000.0 * kArrayLoadMargin);
    }
    return std::max(worst_load_ff, kDefaultAbcLoadFf);
}

bool YosysManager::generate_timing_constraints() const {
    const std::string abc_path = syn_path_ + "/abc.constr";
    std::ofstream abc(abc_path);
    if (!abc.is_open()) {
        LOGE << "Cannot write ABC constraints: " << abc_path;
        return false;
    }
    abc << "set_driving_cell BUFx2_ASAP7_75t_R\n";
    abc << "set_load " << std::fixed << std::setprecision(3)
        << abc_output_load_ff() << "\n";
    abc.close();

    const std::string sdc_path = syn_path_ + "/timing.sdc";
    std::ofstream sdc(sdc_path);
    if (!sdc.is_open()) {
        LOGE << "Cannot write timing constraints: " << sdc_path;
        return false;
    }
    sdc << "# OpenFinRAM ctrl_decode constraints; time=ns capacitance=pF\n";
    sdc << "create_clock -name clk -period " << std::fixed
        << std::setprecision(3) << kClockPeriodNs << " [get_ports {clk}]\n";
    sdc << "set_clock_uncertainty " << kClockUncertaintyNs
        << " [get_clocks {clk}]\n";
    sdc << "set_input_transition " << kInputSlewNs << " [all_inputs]\n";
    sdc << "set_input_delay -clock clk -max " << kIoDelayNs
        << " [get_ports -quiet {ce_n we_n oe_n A* sdel*}]\n";
    sdc << "set_input_delay -clock clk -min " << kInputMinDelayNs
        << " [get_ports -quiet {ce_n we_n oe_n A* sdel*}]\n";
    sdc << "set_output_delay -clock clk -max " << kIoDelayNs
        << " [all_outputs]\n";
    sdc << "set_output_delay -clock clk -min " << kOutputMinDelayNs
        << " [all_outputs]\n";
    sdc << "set_max_transition " << kMaxTransitionNs
        << " [current_design]\n";
    sdc << "set_max_capacitance " << kMaxNetCapPf
        << " [current_design]\n";
    sdc << "set_max_fanout 10 [current_design]\n";
    sdc << "set_voltage 0.700\n";
    sdc << "\n# Predicted array loads with " << kArrayLoadMargin
        << "x routing/model margin.\n";

    const std::string load_report_path = syn_path_ + "/periphery_loads.rpt";
    std::ofstream loads(load_report_path);
    if (!loads.is_open()) {
        LOGE << "Cannot write periphery load report: " << load_report_path;
        return false;
    }
    loads << "# port_pattern predicted_pf constrained_pf\n";
    for (const auto& entry : pin_capacitances_) {
        const double constrained_pf = entry.second > 0.0
            ? entry.second * kArrayLoadMargin
            : kDefaultAbcLoadFf / 1000.0;
        sdc << "set_load " << std::fixed << std::setprecision(6)
            << constrained_pf << " [get_ports -quiet {"
            << entry.first << "*}]\n";
        loads << std::left << std::setw(16) << (entry.first + "*")
              << " " << std::right << std::fixed << std::setprecision(6)
              << entry.second << " " << constrained_pf << "\n";
    }
    loads << "abc_worst_load_ff " << std::fixed << std::setprecision(3)
          << abc_output_load_ff() << "\n";
    loads.close();
    sdc.close();

    LOGI << "Generated load-aware constraints: " << sdc_path
         << " (ABC load " << abc_output_load_ff() << " fF)";
    return true;
}

bool YosysManager::generate_synthesis_script() {
    LOGD << std::string(70, '=');
    LOGD << "Generating Yosys Synthesis Script (open-source ASAP7 single-port)";
    LOGD << std::string(70, '=');

    if (!directory_exists(syn_path_)) {
        if (!create_directory(syn_path_, nullptr)) {
            LOGE << "Failed to create synthesis directory: " << syn_path_;
            return false;
        }
    }
    std::string script_path = syn_path_ + "/synth.ys";
    std::string content = generate_yosys_script();
    std::ofstream out(script_path);
    if (!out.is_open()) {
        LOGE << "Cannot open file for writing: " << script_path;
        return false;
    }
    out << content;
    out.close();
    if (!generate_timing_constraints()) return false;
    LOGI << "Yosys script written to: " << script_path;
    return true;
}

bool YosysManager::run_yosys() {
    LOGD << std::string(70, '=');
    LOGD << "Running Yosys";
    LOGD << std::string(70, '=');
    std::string script_path = syn_path_ + "/synth.ys";
    if (!file_exists(script_path)) {
        LOGE << "synth.ys not found at " << script_path;
        return false;
    }
    // Use bash/sh - tcsh may not be installed; yosys is a plain shell command
    std::string cmd = "bash -c 'cd \"" + syn_path_ + "\" && yosys -s synth.ys > synth.log 2>&1' > /dev/null 2>&1";
    LOGD << "  ▶ Running: " << cmd;
    int result = std::system(cmd.c_str());
    if (result != 0) {
        LOGE << "Yosys failed with exit code: " << result << " (check "
             << syn_path_ << "/synth.log); refusing to generate a correctness stub";
        return false;
    }
    LOGD << "  ✓ Yosys completed successfully";
    return true;
}

bool YosysManager::verify_synthesis_output() {
    LOGD << std::string(70, '=');
    LOGD << "Verifying Yosys Output";
    LOGD << std::string(70, '=');
    std::string netlist_path = syn_path_ + "/netlist.v";
    if (!file_exists(netlist_path)) {
        LOGE << "  ✗ Error: netlist.v not generated at " << netlist_path;
        return false;
    }
    struct stat st;
    if (stat(netlist_path.c_str(), &st) == 0) {
        LOGD << "  ✓ Netlist generated: " << netlist_path << " (" << st.st_size << " bytes)";
        return true;
    }
    LOGE << "  ✗ Error checking netlist file";
    return false;
}

bool YosysManager::verify_periphery_structure() {
    const std::string netlist_path = syn_path_ + "/netlist.v";
    std::ifstream netlist(netlist_path);
    if (!netlist.is_open()) {
        LOGE << "Cannot inspect mapped periphery netlist: " << netlist_path;
        return false;
    }
    std::ostringstream contents;
    contents << netlist.rdbuf();
    const std::string text = contents.str();

    const std::size_t dff_count =
        count_occurrences(text, "DFFHQNx1_ASAP7_75t_R") +
        count_occurrences(text, "DFFHQNx2_ASAP7_75t_R") +
        count_occurrences(text, "DFFHQNx3_ASAP7_75t_R");
    const std::size_t invx1_count =
        count_occurrences(text, "INVx1_ASAP7_75t_R");
    const std::size_t bufx4_count =
        count_occurrences(text, "BUFx4_ASAP7_75t_R");
    std::size_t sdel_d_count = 0;
    for (unsigned bit = 0; bit < 4; ++bit) {
        sdel_d_count += count_occurrences(
            text, ".D(sdel[" + std::to_string(bit) + "])");
    }

    const std::size_t expected_dffs =
        static_cast<std::size_t>(get_addr_width(cli_options_) + 7);
    constexpr std::size_t kExpectedDelayInverters = 108;
    const std::size_t expected_wl_drivers =
        static_cast<std::size_t>(2 * cli_options_.num_wls * cli_options_.num_banks);
    const bool pass = dff_count == expected_dffs &&
                      invx1_count >= kExpectedDelayInverters &&
                      bufx4_count >= expected_wl_drivers &&
                      sdel_d_count == 4;

    const std::string report_path = syn_path_ + "/periphery_structure.rpt";
    std::ofstream report(report_path);
    if (report.is_open()) {
        report << "dff_count " << dff_count << " expected " << expected_dffs << "\n";
        report << "invx1_count " << invx1_count << " expected_min "
               << kExpectedDelayInverters << "\n";
        report << "bufx4_wordline_drivers " << bufx4_count << " expected_min "
               << expected_wl_drivers << "\n";
        report << "sdel_register_inputs " << sdel_d_count << " expected 4\n";
        report << "status " << (pass ? "PASS" : "FAIL") << "\n";
    }

    if (!pass) {
        LOGE << "Periphery structural signoff failed: DFF=" << dff_count
             << "/" << expected_dffs << ", INVx1=" << invx1_count
             << "/>=108, BUFx4=" << bufx4_count << "/>="
             << expected_wl_drivers << ", sdel D pins=" << sdel_d_count << "/4 (see "
             << report_path << ")";
        return false;
    }
    LOGI << "Periphery structural signoff PASS: " << dff_count
         << " state flops, " << invx1_count
         << " INVx1 cells, " << bufx4_count
         << " BUFx4 drivers, all sdel bits retained";
    return true;
}

bool YosysManager::fix_assign_statements() {
    // OpenROAD understands the direct wire aliases emitted by Yosys.  A prior
    // implementation replaced every assign with a scalar buffer, which
    // corrupted concatenated/bus aliases.  Keep legal Yosys assigns intact;
    // physical outputs already have explicit standard-cell drivers.
    LOGD << std::string(70, '=');
    LOGD << "Checking and Fixing Assign Statements (Yosys)";
    LOGD << std::string(70, '=');
    std::string netlist_path = syn_path_ + "/netlist.v";
    if (!file_exists(netlist_path)) return false;
    std::ifstream infile(netlist_path);
    if (!infile.is_open()) return false;
    std::string line;
    int assign_count = 0;
    while (std::getline(infile, line)) {
        size_t ap = line.find("assign");
        if (ap != std::string::npos) {
            size_t cp = line.find("//");
            if (cp == std::string::npos || ap < cp) assign_count++;
        }
    }
    infile.close();
    if (assign_count == 0) {
        LOGD << "  ✓ No assign statements found";
        return true;
    }
    LOGD << "  ✓ Retaining " << assign_count
         << " legal Yosys wire alias assign(s)";
    return true;
}

bool YosysManager::predict_capacitance() {
    LOGD << std::string(70, '=');
    LOGD << "Predicting Capacitance (Yosys path)";
    LOGD << std::string(70, '=');
    LOGD << "  Configuration: num_wls*2=" << cli_options_.num_wls * 2 << ", num_data_bits=" << cli_options_.num_data_bits;
    OpenFinRAM::CapacitancePredictor predictor;
    auto predictions = predictor.predict_all(cli_options_.num_wls * 2, cli_options_.num_data_bits);
    std::map<std::string, std::vector<std::string>> signal_map;
    // single-port focused
    signal_map["WLT"] = {"wlt", "wlb"};
    signal_map["YSELT"] = {"yselt", "yseltn", "yselb", "yselbn"};
    signal_map["BLPRECHTN"] = {"blprechtn", "blprechbn"};
    signal_map["WRENA"] = {"wrena", "wrenan"};
    signal_map["SAE"] = {"sae", "saprechn", "oeb_out", "oe_out"};
    for (auto &pred : predictions) {
        auto it = signal_map.find(pred.first);
        if (it != signal_map.end()) {
            for (auto &pin : it->second) pin_capacitances_[pin] = pred.second;
        } else {
            pin_capacitances_[pred.first] = pred.second;
        }
    }
    return true;
}

bool YosysManager::run_synthesis() {
    LOGD << std::string(70, '#');
    LOGD << "# Yosys Synthesis Flow (single-port ASAP7)";
    LOGD << std::string(70, '#');
    if (!predict_capacitance()) return false;
    if (!generate_synthesis_script()) return false;
    if (!run_yosys()) return false;
    if (!verify_synthesis_output()) return false;
    if (!fix_assign_statements()) return false;
    if (!verify_periphery_structure()) return false;
    LOGD << "✓ Yosys Synthesis Complete";
    return true;
}
