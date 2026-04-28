#include "innovus_manager.hpp"

#include <cmath>

#include "plog/Log.h"

#include "innovus_tcl_generator.hpp"
#include "utils.hpp"

InnovusManager::InnovusManager(const MainCliOptions& cli_options)
    : cli_options_(cli_options) {}

bool InnovusManager::run_innovus_flow() {
    OpenFinRAM::InnovusTclGenerator tcl_generator;

    tcl_generator.set_design_name("ctrl_decode");
    tcl_generator.set_site_name("asap7sc7p5t");
    tcl_generator.set_site_height(0.27);
    tcl_generator.set_cpu_count(8, 0);

    std::string qor_file_path = join_path(get_executable_directory(), "tmp/syn_" + get_run_timestamp() + "/qor_report.txt");
    bool qor_parsed = tcl_generator.parse_qor_report(qor_file_path);

    if (!qor_parsed) {
        LOGE << "Failed to parse QoR report, cannot proceed with Innovus flow.";
        return false;
    }

    std::string output_tcl_path = join_path(get_executable_directory(), "tmp/innovus_" + get_run_timestamp() + "/run.tcl");
    if (!directory_exists(join_path(get_executable_directory(), "tmp/innovus_" + get_run_timestamp()))) {
        LOGD << "Creating Innovus output directory: " << join_path(get_executable_directory(), "tmp/innovus_" + get_run_timestamp());
        if (!create_directory(join_path(get_executable_directory(), "tmp/innovus_" + get_run_timestamp()), nullptr)) {
            LOGE << "Failed to create Innovus output directory: " << join_path(get_executable_directory(), "tmp/innovus_" + get_run_timestamp());
            return false;
        }
    }

    int num_ysel = 4;
    int addr_width = std::ceil(std::log2(cli_options_.num_wls)) 
                   + std::ceil(std::log2(cli_options_.num_banks))
                   + 1 + 2; // +1 for top/bottom, +2 for ysel
    double sram_width = 10.0; // Tmp value
    
    if (tcl_generator.generate_run_tcl(sram_width, 
        0.0, 
        output_tcl_path, 
        cli_options_.num_wls, 
        cli_options_.num_wls, 
        num_ysel, 
        addr_width, 
        cli_options_.num_banks,
        cli_options_.spice_only)) {
        LOGI << "run.tcl generated successfully at: " << output_tcl_path;
    } else {
        LOGE << "Failed to generate run.tcl";
        return false;
    }

    std::string innovus_work_dir = join_path(get_executable_directory(), "tmp/innovus_" + get_run_timestamp());
    std::string log_file = join_path(innovus_work_dir, "innovus.log");
    if (tcl_generator.run_innovus(output_tcl_path, innovus_work_dir, log_file)) {
        LOGI << "Innovus flow completed successfully.";
    } else {
        LOGE << "Innovus flow failed.";
        return false;
    }

    std::string verilog_file_name = "netlist_for_lvs.v";
    std::string spice_file_name = "netlist_for_lvs.sp";
    std::string cdl_file_path = join_path(get_executable_directory(), "tech/cdl/asap7sc7p5t_28_R.cdl");

    if (tcl_generator.run_v2lvs(innovus_work_dir)) {
        LOGI << "v2lvs completed successfully.";
    } else {
        LOGE << "v2lvs failed.";
        return false;
    }

    if (tcl_generator.post_process_netlist(innovus_work_dir, spice_file_name, cdl_file_path)) {
        LOGI << "Post-processing of SPICE netlist completed successfully.";
    } else {
        LOGE << "Post-processing of SPICE netlist failed.";
        return false;
    }
    

    return true;
}