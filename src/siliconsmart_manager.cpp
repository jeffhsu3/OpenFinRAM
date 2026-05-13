#include "siliconsmart_manager.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plog/Log.h"

#include "utils.hpp"

SiliconSmartManager::SiliconSmartManager(const MainCliOptions& options)
    : cli_option_(options) {
}

bool SiliconSmartManager::gen_run() {
    std::string filePath = join_path(get_current_dir_name(), "tmp/sis_" + get_run_timestamp() + "/run.tcl");

    std::string cellName = "sram_x" + std::to_string(cli_option_.num_wls * 2) 
                           + "x" + std::to_string(cli_option_.num_data_bits) + 
                           "x" + std::to_string(cli_option_.num_banks);

    std::string content = "exec rm -rf testcase\n\n";
    content += "set cells {" + cellName + "}\n";
    content += R"(set charpoint testcase
create $charpoint
set_log_file $charpoint/sis.log

exec cp configure.tcl ${charpoint}/config/configure.tcl
set_location $charpoint

flatten_all_netlists -netlist_dir netlist -ext .sp -out_dir netlist_flat $cells

import -template template.tcl -netlist_dir netlist_flat -extension .sp -instance_dir control -flatten $cells

configure -fast -timing -power $cells
characterize $cells

model -timing -power -output nldm $cells
)";

    std::ofstream out(filePath);
    if (!out) {
        LOGE << "Failed to write run.tcl: " << filePath;
        return false;
    }
    out << content;
    out.close();

    return true;
}

bool SiliconSmartManager::gen_config() {
    int addr_width = get_addr_width(cli_option_);
    int bus_width = cli_option_.num_data_bits;

    std::string content = R"(# See SiliconSmart User Guide Appendix B for a complete list of parameters and definitions

############################
# DEFAULT PINTYPE PARAMETERS
############################
pintype default {
	set logic_high_name vdd
    set logic_low_name vss
    set logic_high_threshold 0.9
    set logic_low_threshold 0.1
    set prop_delay_level 0.5
    set explicit_points_slew { 0.005e-9 0.04e-9 0.32e-9  }
    set explicit_points_load { 0.00576e-12 0.04608e-12 0.36864e-12 }
    set initial_delay 5e-9 
}

pintype sis_address->default {
set bus_width )" + std::to_string(addr_width) + R"(}

pintype sis_data->default {
set bus_width )" + std::to_string(bus_width) + R"(}

#################################
# OPERATING CONDITIONS DEFINITION
#################################
create_operating_condition PVT_0P7V_25C
add_opc_supplies PVT_0P7V_25C vdd 0.7
add_opc_grounds PVT_0P7V_25C vss 0
set_opc_temperature PVT_0P7V_25C 25
set_opc_process PVT_0P7V_25C {
	{ .inc '/home/s1111534/asap7/asap7_pdk_r1p7/models/hspice/7nm_TT.pm' }
}


#################################
# GLOBAL CONFIGURATION PARAMETERS
#################################
define_parameters default {

    set active_pvts { PVT_0P7V_25C }

    set simulator finesim
    set simulator_cmd {finesim -w <input_deck> -o <listing_file> >&/dev/null}
    set simulation_tmpdir /tmp

    set simulator_options {
        "common,finesim: post=1 finesim_output=fsdb finesim_mode=promd finesim_spred=3"
    }

    # Simulation resolution
    set time_res_high 1e-12
    set time_res_low 100e-12
    
    # Controls which supplies are measured for power consumption
    set power_meas_supplies { vdd }

    # list of ground supplies used (required for Functional Recognition)
    set power_meas_grounds { vss }

    set slew_derate_upper_threshold 0.9
    set slew_derate_lower_threshold 0.1

    set liberty_max_transition 1 
    set liberty_max_capacitance 1

	set archive_condition_on_success compress
	set archive_condition_on_failure yes

    set nmos_model_names { nmos_rvt nmos_lvt nmos_hvt nmos_sram }
    set pmos_model_names { pmos_rvt pmos_lvt pmos_hvt pmos_sram }

    set constraint_mode independent
    set smc_constraint_style relative-degradation
    set smc_degrade 0.1
    set path_constraint_mode off	

    set job_scheduler standalone
    set run_list_maxsize 64
    set normal_queue {bnormal -R rusage[mem=16000]} 
}


#####################################
# LIBERTY MODEL GENERATION PARAMETERS
#####################################
define_parameters liberty_model {
    # Add Liberty header attributes here for use with "model -create_new_model"
    set delay_model "table_lookup"
    set default_fanout_load 1.0
    set default_inout_pin_cap 1.0
    set default_input_pin_cap 1.0
    set default_output_pin_cap 0.0
    set default_cell_leakage_power 0.0
    set default_leakage_power_density  0.0
    set in_place_swap_mode match_footprint
    
    set slew_lower_threshold_pct_fall 0.1
    set slew_upper_threshold_pct_fall 0.9
    set slew_lower_threshold_pct_rise 0.1
    set slew_upper_threshold_pct_rise 0.9
}


#######################
# VALIDATION PARAMETERS
#######################
define_parameters validation {
    # Add validation parameters here
}

)";

    std::string filePath = join_path(get_current_dir_name(), "tmp/sis_" + get_run_timestamp() + "/configure.tcl");
    std::ofstream out(filePath);
    if (!out) {
        LOGE << "Failed to write configure.tcl: " << filePath;
        return false;
    }
    out << content;
    out.close();

    return true;
}

bool SiliconSmartManager::gen_template() {
    int addr_width = get_addr_width(cli_option_);
    int data_width = cli_option_.num_data_bits;
    int num_bank = cli_option_.num_banks;

    std::string cell_name = "sram_x" + std::to_string(cli_option_.num_wls * 2) 
                            + "x" + std::to_string(cli_option_.num_data_bits) + 
                            "x" + std::to_string(cli_option_.num_banks);

    std::string content;

    if (cli_option_.single_port) {
        content = R"(set_memory_type single_port_ram
set_memory_name )" + cell_name + R"(

create_readwrite_port A
set_clock clk -active r -port A
set_address_bus A -width )" + std::to_string(addr_width) + R"( -port A
set_data_bus D -width )" + std::to_string(data_width) + R"( -port A
set_write_enable we_n -active L -port A
set_read_enable oe_n -active L -port A
set_chip_enable ce_n -active L -port A
set_data_output Q -width )" + std::to_string(data_width) + R"( -port A)";
    } else {
        content = R"(set_memory_type multi_port_ram
set_memory_name )" + cell_name + R"(
set_separate_statetable_mode ON

create_readwrite_port A
set_clock clk -active r -port A
set_address_bus a_a -width )" + std::to_string(addr_width) + R"( -port A
set_data_bus d_a -width )" + std::to_string(data_width) + R"( -port A
set_read_enable oe_n_a -active L -port A
set_write_enable we_n_a -active L -port A
set_chip_enable ce_n_a -active L -port A
set_data_output q_a -width )" + std::to_string(data_width) + R"( -port A

create_readwrite_port B
set_clock clk -active r -port B
set_address_bus a_b -width )" + std::to_string(addr_width) + R"( -port B
set_read_enable oe_n_b -active L -port B
set_chip_enable ce_n_b -active L -port B
set_data_output q_b -width )" + std::to_string(data_width) + R"( -port B
)";
    }

    std::string filePath = join_path(get_current_dir_name(), "tmp/sis_" + get_run_timestamp() + "/template.tcl");
    std::ofstream out(filePath);
    if (!out) {
        LOGE << "Failed to write template.tcl: " << filePath;
        return false;
    }
    out << content;
    out.close();

    return true;
}

std::string SiliconSmartManager::get_port_list() {
    std::ostringstream ss;
    int addr_width = get_addr_width(cli_option_);
    int data_width = cli_option_.num_data_bits;

    if (cli_option_.single_port) {
        ss << "vdd vss clk rst_n ce_n we_n oe_n ";
        for (int i = 0; i < addr_width; ++i) {
            ss << "A_" << i << " ";
        }
        for (int i = 0; i < data_width; ++i) {
            ss << "D_" << i << " ";
        }
        for (int i = 0; i < data_width; ++i) {
            ss << "Q_" << i << " ";
        }
    } else {
        ss << "vdd vss clk rst_n ce_n_a we_n_a oe_n_a ";

        for (int i = 0; i < addr_width; ++i) {
            ss << "a_a_" << i << " ";
        }
        for (int i = 0; i < data_width; ++i) {
            ss << "d_a_" << i << " ";
        }
        for (int i = 0; i < data_width; ++i) {
            ss << "q_a_" << i << " ";
        }

        ss << "ce_n_b oe_n_b ";
        for (int i = 0; i < addr_width; ++i) {
            ss << "a_b_" << i << " ";
        }
        for (int i = 0; i < data_width; ++i) {
            ss << "q_b_" << i << " ";
        }
    }
    return ss.str();
}

bool SiliconSmartManager::gen_inst() {
    std::string cell_name = "sram_x" + std::to_string(cli_option_.num_wls * 2) 
                            + "x" + std::to_string(cli_option_.num_data_bits) + 
                            "x" + std::to_string(cli_option_.num_banks);
    std::string content;

    if (cli_option_.single_port) {
        content = R"(set_netlist_file [get_location]/netlists/)" + cell_name + R"(.sp

set_cell_type memory

##
## Pin definitions.
##
add_pin rst_n default -input -async
add_pin A sis_address -input
add_pin D sis_data -input
add_pin we_n default -input
add_pin oe_n default -input
add_pin ce_n default -input
add_pin clk default -clock
add_pin Q sis_data -output

add_forbidden_state {!we_n & !oe_n}

add_pin mem_int default -internal -spice_node {xdata_top/x0_0/x1/x4/x0/q}
add_pin state0 default -internal -spice_node {xctrl/state[0]} -no_model
add_pin state1 default -internal -spice_node {xctrl/state[1]} -no_model

set_subckt_ports { )" + get_port_list() + R"( }
add_table {
    rst_n clk we_n ce_n oe_n A D   : mem mem_2 iqa state0 state1 : mem mem_2 iqa state0 state1
    L     -   -    -    -    - -   : -   -     -   -      -      : n   n     n   1      1
    H     r   L    L    H    L 0/1 : -   -     -   -      -      : 0/1 n     n   n      n
    H     r   L    L    H    H 0/1 : -   -     -   -      -      : n   0/1   n   n      n
    H     r   H    L    L    L -   : 0/1 -     -   -      -      : n   n     0/1 n      n
    H     r   H    L    L    H -   : -   0/1   -   -      -      : n   n     0/1 n      n
    H     r   -    H    -    - -   : -   -     -   -      -      : n   n     n   n      n
    H     -   -    -    -    - -   : -   -     -   -      -      : n   n     n   n      n
}
add_function Q iqa
add_function mem_int mem

define_parameters )" + cell_name + R"( { set liberty_blackbox_model 1 })";
    } else {
        content = R"(set_netlist_file [get_location]/netlists/)" + cell_name + R"(.sp

set_cell_type memory

##
## pin definitions.
##
add_pin rst_n default -input -async
add_pin clk default -input
add_pin a_a sis_address -input
add_pin d_a sis_data -input
add_pin oe_n_a default -input
add_pin we_n_a default -input
add_pin ce_n_a default -input
add_pin a_b sis_address -input
add_pin oe_n_b default -input
add_pin ce_n_b default -input
add_pin q_a sis_data -output
add_pin q_b sis_data -output

add_forbidden_state {!we_n_a & !oe_n_a}

add_pin mem_int default -internal -spice_node {xdata_bottom/x0_0/x1/x0/x0/q}

add_pin state0_a default -internal -spice_node {xctrl/state_a_0} -no_model
add_pin state1_a default -internal -spice_node {xctrl/state_a_1} -no_model
add_pin state1_b default -internal -spice_node {xctrl/state_b} -no_model

set_subckt_ports { )" + get_port_list() + R"( }
add_table {
    rst_n clk we_n_a ce_n_a oe_n_a a_a d_a   : mem mem_2 iqa state0_a state1_a : mem mem_2 iqa state0_a state1_a
    l     -   -      -      -      -   -     : -   -     -   -        -        : n   n     n   1        1
    h     r   l      l      h      l   0/1   : -   -     -   -        -        : 0/1 n     n   n        n
    h     r   l      l      h      h   0/1   : -   -     -   -        -        : n   0/1   n   n        n
    h     r   h      l      l      l   -     : 0/1 -     -   -        -        : n   n     0/1 n        n
    h     r   h      l      l      h   -     : -   0/1   -   -        -        : n   n     0/1 n        n
    h     r   -      h      -      -   -     : -   -     -   -        -        : n   n     n   n        n
    h     -   -      -      -      -   -     : -   -     -   -        -        : n   n     n   n        n
}

add_table {
    rst_n clk ce_n_b oe_n_b a_b   : mem mem_2 iqb  state1_b : mem mem_2 iqb  state1_b
    l     -   -      -      -     : -   -     -    -        : n   n     n    1
    h     r   l      l      l     : 0/1 -     -    -        : n   n     0/1  n
    h     r   l      l      h     : -   0/1   -    -        : n   n     0/1  n
    h     r   h      -      -     : -   -     -    -        : n   n     n    n
    h     -   -      -      -     : -   -     -    -        : n   n     n    n
}


add_function q_a iqa
add_function q_b iqb
add_function mem_int mem

define_parameters )" + cell_name + R"( { set liberty_blackbox_model 1 }
)" ;
    }

    // Mkdir control directory if it doesn't exist
    std::string control_dir = join_path(get_current_dir_name(), "tmp/sis_" + get_run_timestamp() + "/control");
    if (!directory_exists(control_dir)) {
        if (!create_directory(control_dir, nullptr)) {
            LOGE << "Failed to create directory: " << control_dir;
            return false;
        }
    }

    std::string filePath = join_path(control_dir, cell_name + ".inst");
    std::ofstream out(filePath);
    if (!out) {
        LOGE << "Failed to write .inst file: " << filePath;
        return false;
    }
    out << content;
    out.close();

    return true;
}

bool SiliconSmartManager::copy_lib_file() {
    std::string cell_name = "sram_x" + std::to_string(cli_option_.num_wls * 2) 
                            + "x" + std::to_string(cli_option_.num_data_bits) + 
                            "x" + std::to_string(cli_option_.num_banks);

    // Unzip
    std::string src_path = join_path(get_current_dir_name(), "tmp/sis_" + get_run_timestamp() + "/testcase/models/liberty/cells");
    std::string cmd = "bash -c 'cd " + src_path + " && gunzip " + cell_name + "_PVT_0P7V_25C.lib.gz'";
    if (system(cmd.c_str()) != 0) {
        LOGE << "Failed to unzip library file: " << cell_name << "_PVT_0P7V_25C.lib.gz";
        return false;
    }

    // Copy the unzipped .lib file to ./tmp/sis_{timestamp}/netlist/sis_model.lib
    src_path += "/" + cell_name + "_PVT_0P7V_25C.lib";
    std::string dst_path = join_path(get_current_dir_name(), "results/" + cell_name + "_" + get_run_timestamp() + "/" + cell_name + ".lib");

    if (!copy_file(src_path, dst_path)) {
        LOGE << "Failed to copy library file from " << src_path << " to " << dst_path;
        return false;
    }
    return true;
}

bool SiliconSmartManager::run_siliconsmart() {
    if (cli_option_.skip_characterization) {
        LOGI << "SiliconSmart characterization skipped as per configuration.";
        return true;
    }

    std::string sis_dir = join_path(get_current_dir_name(), "tmp/sis_" + get_run_timestamp());

    // Make ./tmp/sis directory
    if (!directory_exists(sis_dir)) {
        if (!create_directory(sis_dir, nullptr)) {
            LOGE << "Failed to create directory: " << sis_dir;
            return false;
        }
    }

    // Make ./tmp/netlist directory
    std::string netlist_dir = join_path(sis_dir, "netlist");
    if (!directory_exists(netlist_dir)) {
        if (!create_directory(netlist_dir, nullptr)) {
            LOGE << "Failed to create directory: " << netlist_dir;
            return false;
        }
    }

    // Move ./tmp/sram_flat_sis_<timestamp>.sp to ./tmp/sis/netlist/sram_flat_sis_<timestamp>.sp
    std::string src_netlist = join_path(get_current_dir_name(), "tmp/sram_flat_sis_" + get_run_timestamp() + ".sp");
    std::string dst_netlist = join_path(netlist_dir, "sram_x" + std::to_string(cli_option_.num_wls * 2) + "x" + std::to_string(cli_option_.num_data_bits) + "x" + std::to_string(cli_option_.num_banks) + ".sp");
    if (!copy_file(src_netlist, dst_netlist)) {
        LOGE << "Failed to copy netlist from " << src_netlist << " to " << dst_netlist;
        return false;
    }

    if (!gen_run()) {
        LOGE << "Failed to generate SiliconSmart run.tcl";
        return false;
    }
    if (!gen_config()) {
        LOGE << "Failed to generate SiliconSmart configure.tcl";
        return false;
    }
    if (!gen_template()) {
        LOGE << "Failed to generate SiliconSmart template.tcl";
        return false;
    }
    if (!gen_inst()) {
        LOGE << "Failed to generate SiliconSmart .inst file";
        return false;
    }

    // Run siliconsmart characterization
    std::string cmd = "cd " + sis_dir + "; siliconsmart ./run.tcl >& /dev/null";
    LOGI << "Running SiliconSmart with command: tcsh -c '" << cmd << "'";

    pid_t child_pid = fork();
    if (child_pid < 0) {
        LOGE << "fork() failed";
        return false;
    }

    if (child_pid == 0) {
        // Child: own process group so parent can kill the whole tree
        setpgid(0, 0);
        execl("/bin/tcsh", "tcsh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    // Monitor log file for fatal task errors in a background thread
    std::string log_file = join_path(sis_dir, "testcase/sis.log");
    std::atomic<bool> process_done{false};
    std::atomic<bool> kill_requested{false};

    std::thread monitor([&]() {
        while (!process_done.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (process_done.load()) break;

            std::ifstream log(log_file);
            if (!log.is_open()) continue;

            std::string line;
            while (std::getline(log, line)) {
                if (line.find("Error:   Task") != std::string::npos) {
                    LOGE << "SiliconSmart log error detected: " << line;
                    LOGI << "Killing SiliconSmart process group (pid=" << child_pid << ")";
                    kill(-child_pid, SIGTERM);
                    kill_requested.store(true);
                    return;
                }
            }
        }
    });

    int status = 0;
    waitpid(child_pid, &status, 0);
    process_done.store(true);
    monitor.join();

    if (kill_requested.load()) {
        LOGE << "SiliconSmart terminated early due to task error in log";
        return false;
    }

    int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (ret != 0) {
        LOGE << "SiliconSmart execution failed with exit code: " << ret;
        return false;
    }

    if (!copy_lib_file()) {
        LOGE << "Failed to copy generated .lib file to results directory";
        return false;
    }

    return true;
}