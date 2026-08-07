#ifndef YOSYS_MANAGER_HPP
#define YOSYS_MANAGER_HPP

#include <map>
#include <string>
#include "main_config_helpers.hpp"

/**
 * Manages Yosys synthesis for open-source ASAP7 flow (single-port).
 * Mirrors SynthesisManager API but emits Yosys script instead of DC TCL.
 */
class YosysManager {
public:
    explicit YosysManager(const MainCliOptions& cli_options);

    bool run_synthesis();
    bool generate_synthesis_script();
    bool run_yosys();
    bool verify_synthesis_output();
    bool verify_periphery_structure();
    bool fix_assign_statements();
    bool predict_capacitance();

    // Paths (mirrors SynthesisManager for reuse by OpenROAD flow)
    std::string get_syn_path() const { return syn_path_; }
    std::string get_qor_path() const { return syn_path_ + "/qor_report.txt"; }

private:
    MainCliOptions cli_options_;
    std::string cur_path_;
    std::string rtl_path_;
    std::string syn_path_;
    std::map<std::string, double> pin_capacitances_;

    std::string generate_parameter_string() const;
    std::string generate_yosys_script() const;
    bool generate_timing_constraints() const;
    double abc_output_load_ff() const;
};

#endif // YOSYS_MANAGER_HPP
