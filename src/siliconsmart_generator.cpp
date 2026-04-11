#include "siliconsmart_generator.hpp"

#include "plog/Log.h"
#include "utils.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>
#include <vector>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace OpenFinRAM {

namespace {

bool path_exists(const std::string& path, bool* is_dir = nullptr) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    if (is_dir) {
        *is_dir = S_ISDIR(st.st_mode);
    }
    return true;
}

bool ensure_directory(const std::string& path) {
    bool is_dir = false;
    if (path_exists(path, &is_dir)) {
        return is_dir;
    }
    if (mkdir(path.c_str(), 0755) != 0) {
        LOGE << "Failed to create directory: " << path << " (" << std::strerror(errno) << ")";
        return false;
    }
    return true;
}

std::string join_path(const std::string& base, const std::string& name) {
    if (base.empty() || base == ".") {
        return name;
    }
    if (base.back() == '/') {
        return base + name;
    }
    return base + "/" + name;
}

bool write_text_file(const std::string& path, const std::string& content) {
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out) {
        LOGE << "Failed to write file: " << path;
        return false;
    }
    out << content;
    return true;
}

bool read_text_file(const std::string& path, std::string& content) {
    std::ifstream in(path.c_str(), std::ios::in);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    content = ss.str();
    return true;
}

bool copy_file(const std::string& src, const std::string& dst) {
    std::ifstream in(src.c_str(), std::ios::binary);
    if (!in) {
        LOGE << "Failed to open source file: " << src;
        return false;
    }
    std::ofstream out(dst.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        LOGE << "Failed to open destination file: " << dst;
        return false;
    }
    out << in.rdbuf();
    return true;
}

void replace_all(std::string& content, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = content.find(from, pos)) != std::string::npos) {
        content.replace(pos, from.length(), to);
        pos += to.length();
    }
}

std::string build_run_tcl(const std::string& cell_name) {
    std::ostringstream ss;
    ss << "exec rm -rf testcase\n\n";
    ss << "set cells {" << cell_name << "}\n";
    ss << "set charpoint testcase\n";
    ss << "create $charpoint\n";
    ss << "set_log_file $charpoint/sis.log\n\n";
    ss << "exec cp configure.tcl ${charpoint}/config/configure.tcl\n";
    ss << "set_location $charpoint\n\n";
    ss << "import -template template.tcl -netlist_dir netlist -extension .sp -instance_dir control\n\n";
    ss << "configure -fast -timing -power $cells\n";
    ss << "characterize $cells\n\n";
    ss << "model -timing -power -output nldm $cells\n";
    return ss.str();
}

std::string build_template_tcl(const std::string& cell_name, uint64_t addr_width, uint64_t data_width) {
    std::ostringstream ss;
    ss << "set_memory_type single_port_ram\n";
    ss << "set_memory_name " << cell_name << "\n";
    ss << "create_readwrite_port A\n";
    ss << "set_clock clk -active r -port A\n";
    ss << "set_address_bus A -width " << addr_width << " -port A\n";
    ss << "set_data_bus D -width " << data_width << " -port A\n";
    ss << "set_write_enable we_n -active L -port A\n";
    ss << "set_read_enable oe_n -active L -port A\n";
    ss << "set_chip_enable ce_n -active L -port A\n";
    ss << "set_data_output Q -width " << data_width << " -port A\n";
    return ss.str();
}


std::string build_inst_file(const std::string& cell_name, uint64_t addr_width, uint64_t data_width) {
    std::ostringstream ss;
    ss << "set_netlist_file [get_location]/netlists/" << cell_name << "_flat.sp\n\n";
    ss << "set_cell_type memory\n\n";
    ss << "##\n## Pin definitions.\n##\n";
    ss << "add_pin rst_n default -input -async\n";
    ss << "add_pin A sis_address_" << addr_width << " -input\n";
    ss << "add_pin D sis_data_" << data_width << " -input\n";
    ss << "add_pin we_n default -input\n";
    ss << "add_pin oe_n default -input\n";
    ss << "add_pin ce_n default -input\n";
    ss << "add_pin clk default -clock\n";
    ss << "add_pin Q sis_data_" << data_width << " -output\n\n";

    ss << "add_forbidden_state {!we_n & !oe_n}\n\n";

    ss << "add_pin mem_int default -internal -spice_node {xdata_top/x0_0/x1/x4/x0/q}\n\n";
    ss << "add_pin state0 default -internal -spice_node {xctrl/state[0]} -no_model\n";
    ss << "add_pin state1 default -internal -spice_node {xctrl/state[1]} -no_model\n\n";

    ss << "set_subckt_ports { vdd vss clk rst_n ce_n we_n oe_n ";
    for (uint64_t i = 0; i < addr_width; ++i) {
        ss << "A_" << i << " ";
    }
    for (uint64_t i = 0; i < data_width; ++i) {
        ss << "D_" << i << " ";
    }
    for (uint64_t i = 0; i < data_width; ++i) {
        ss << "Q_" << i << " ";
    }
    ss << "}\n\n";

    ss << "add_table {\n";
    ss << "    rst_n clk we_n ce_n oe_n A D   : mem mem_2 iqa state0 state1 : mem mem_2 iqa state0 state1\n";
    ss << "    L     -   -    -    -    - -   : -   -     -   -      -      : n   n     n   1      1\n";
    ss << "    H     r   L    L    H    L 0/1 : -   -     -   -      -      : 0/1 n     n   n      n\n";
    ss << "    H     r   L    L    H    H 0/1 : -   -     -   -      -      : n   0/1   n   n      n\n";
    ss << "    H     r   H    L    L    L -   : 0/1 -     -   -      -      : n   n     0/1 n      n\n";
    ss << "    H     r   H    L    L    H -   : -   0/1   -   -      -      : n   n     0/1 n      n\n";
    ss << "    H     r   -    H    -    - -   : -   -     -   -      -      : n   n     n   n      n\n";
    ss << "    H     -   -    -    -    - -   : -   -     -   -      -      : n   n     n   n      n\n";
    ss << "}\n";
    ss << "add_function Q iqa\n";
    ss << "add_function mem_int mem\n\n";

    ss << "create_parameter sis_pruning_with_flat_netlist\n";
    ss << "set_config_opt -- sis_pruning_with_flat_netlist 1\n\n";
    ss << "#set_config_opt -type timing state_partitions all\n";

    ss << "set_config_opt -type delay -from clk -to Q whens {!oe_n}\n";
    ss << "set_config_opt -type delay -from oe_n -to Q state_partitions none\n\n";

    ss << "define_parameters " << cell_name << " { set liberty_blackbox_model 1 }\n\n";

    ss << "#set_memory_type single_port_ram\n\n";
    ss << "#create_readwrite_port A\n";
    ss << "#set_clock clk -port A -active r\n";
    ss << "#set_address_bus A -width " << addr_width << " -port A\n";
    ss << "#set_data_bus D -width " << data_width << " -port A\n";
    ss << "#set_chip_enable ce_n -width 1 -active L -port A\n";
    ss << "#set_write_enable we_n -width 1 -active L -port A\n";
    ss << "#set_read_enable oe_n -width 1 -active L -port A\n";
    ss << "#set_data_output Q -width " << data_width << " -port A\n\n";
    ss << "#validate_targetbits_for_addressbus\n";
    return ss.str();
}

bool scan_log_for_error(const std::string& log_path, std::streampos& offset) {
    std::ifstream log(log_path.c_str(), std::ios::in);
    if (!log) {
        return false;
    }

    log.seekg(offset);
    std::string line;
    while (std::getline(log, line)) {
        if (line.find("Error:   Task") != std::string::npos) {
            offset = log.tellg();
            return true;
        }
    }
    offset = log.tellg();
    return false;
}

std::string build_configure_template() {
    std::string cur_path = get_executable_directory();

    std::string content = R"CFG(pintype sis_address_{{ADDR_WIDTH}}->default {
set bus_width {{ADDR_WIDTH}}
}

pintype sis_data_{{DATA_WIDTH}}->default {
set bus_width {{DATA_WIDTH}}
}

# See SiliconSmart User Guide Appendix B for a complete list of parameters and definitions

#################################
# OPERATING CONDITIONS DEFINITION
#################################
create_operating_condition PVT_0P7V_25C
add_opc_supplies PVT_0P7V_25C vdd 0.7
add_opc_grounds PVT_0P7V_25C vss 0
set_opc_temperature PVT_0P7V_25C 25
set_opc_process PVT_0P7V_25C {
    { .inc '{{MODEL_PATH}}' }
}


#################################
# GLOBAL CONFIGURATION PARAMETERS
#################################
define_parameters default {

    set active_pvts { PVT_0P7V_25C }

    # If using IBIS, one operating condition must be specified in ibis_typ_pvt
    # set ibis_typ_pvt op_cond

    # FINESIM
    set simulator finesim
    set simulator_cmd {finesim -w <input_deck> -o <listing_file> >&/dev/null}
    set simulation_tmpdir /tmp

    # FINESIM EMBEDDED
    # set simulator finesim_embedded

    # HSPICE
    # set simulator hspice
    # set simulator_cmd {hspice <input_deck> -o <listing_file>}
    
    # HSPICE (client/server mode)
    # set simulator hspice_cs
    # set simulator_cmd {hspice -CC <input_deck> -port <port_num> -o <listing_file>}
    
    # SPECTRE
    # set simulator spectre6
    # set simulator_cmd {spectremdl -tab -batch <mdl_file> -design <input_deck> <listing_file> >&/dev/null}
    
    # ELDO
    # set simulator eldo
    # set simulator_cmd {eldo -compat -i <input_deck> > <listing_file> >&/dev/null}

    # MSIM
    # set simulator msim
    # (csh)
    # set simulator_cmd {msim -hsp -i <input_deck> -o <listing_file> >&/dev/null}
    # (sh)
    # set simulator_cmd {msim -hsp -i <input_deck> -o <listing_file> 2>/dev/null}

    set simulator_options {
        "common,finesim: post=1 finesim_output=fsdb finesim_mode=promd finesim_spred=3"
    }
    # Default simulator options for Finesim, Hspice, Spectre, Msim, and Eldo
    # set simulator_options {
	# "common,finesim_embedded: probe=1 finesim_output=fsdb finesim_mode=spicehd finesim_method=gearv numdgt=7 measdgt=7"
	# "common,finesim: probe=1 finesim_output=fsdb finesim_mode=spicehd finesim_method=gearv numdgt=7 measdgt=7"
	# "power,finesim_embedded: probe=1 finesim_output=tr0 finesim_mode=spice2  finesim_qlevel=3 finesim_method=gear finesim_leakage_mode=1"
	# "common,hspice: probe=1 runlvl=5 numdgt=7 measdgt=7 acct=1 nopage"
	
	# "common,spectre6: compression=yes step=10ps maxstep=1ns relref=allglobal"
	# "common,spectre6: method=trap lteratio=4 gmin=1e-18 autostop=0 save=none"

	# "common,msim: probe=1 accurate=1"
	
	# "common,eldo: gmindc=1n gmin=1p itl1=500 ingold=1 numdgt=4 measout=0 cptime=18000 relvar=0.01"
	# "op,eldo: dv=0.5 method=gear"
	# "tran,eldo: brief=0 relvar=0.001"
	# "optimize,eldo: lvltim=3 relvar=0.001"
	# "power,eldo: method=gear"
    # }

    # Simulation resolution
    set time_res_high 1e-12
    set time_res_low 100e-12
    #set gate_leakage_time_scaling_factor 100
    
    # Controls which supplies are measured for power consumption
    set power_meas_supplies { vdd }

    # list of ground supplies used (required for Functional Recognition)
    set power_meas_grounds { vss }

    # specifies which multi-rail format to be used in Liberty model; none, v1, or v2.
    # set liberty_multi_rail_format v2

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



    # LOAD SHARE PARAMETERS
    #  job_scheduler: 'lsf' (Platform), 'grid' (SunGrid), or 'standalone' (local machine)
    set job_scheduler standalone
    set run_list_maxsize 16
    set normal_queue {bnormal -R rusage[mem=16000]} 

    set separate_cell_initialization off
    set tran_leakage_with_separate_init 0
    set model_failed_cells_in_lib 1
    #set simulation_node_initialization_file ic.sp
    set gzip_init_files 0
}


############################
# DEFAULT PINTYPE PARAMETERS
############################
pintype default {

    # set total_slew_multiplier 2.0
    
	set logic_high_name vdd
    set logic_high_threshold 0.9

    set logic_low_name vss
    set logic_low_threshold 0.1

    set prop_delay_level 0.5

    set explicit_points_slew { 0.005e-9 0.01e-9 0.02e-9 0.04e-9 0.08e-9 0.16e-9 0.32e-9  }
    set explicit_points_load { 0.00576e-12 0.01152e-12 0.02304e-12 0.04608e-12 0.09216e-12 0.18432e-12 0.36864e-12 }

    set initial_delay 5e-9 

#     # Number of slew and load indices
#     # (when importing with -use_default_slews -use_default_loads)
#     set numsteps_slew 5
#     set numsteps_load 5
#     set constraint_numsteps_slew 3

#     # Operating load ranges
#     set smallest_load 10e-15
#     #set largest_load 90e-15
#     #set autorange_load state

#     # Operating slew ranges
#     # this is what is in the liberty
#     #set smallest_slew 28e-12
#     #set largest_slew 1.02e-09
#     #set max_tout 1.02e-09
#     #
#     # due to slew_derate_from_library=0.5 
#     set smallest_slew 10.0e-12
#     set largest_slew 5.0e-9
#     set max_tout 5.0e-9

#     # Automatically determine largest_load based on max_tout; off or on
#     set autorange_load on

#     # Noise of points in for noise height
#     set numsteps_height 8

#     # Input noise width.
#     set numsteps_width 5

#     # driver model: pwl, emulated, active, active-waveform, custom
#     set driver_mode emulated

#     # driver cell name (relevant only when driver_mode is "active")
#    #  set driver pwl
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
)CFG";

    replace_all(content, "{{MODEL_PATH}}", join_path(cur_path, "models/hspice/7nm_TT.pm"));
    return content;
}

} // namespace

bool SiliconSmartGenerator::generate(const SiliconSmartConfig& config) const {
    if (config.cell_name.empty()) {
        LOGE << "SiliconSmartGenerator: cell_name is empty";
        return false;
    }
    if (config.addr_width == 0 || config.data_width == 0) {
        LOGE << "SiliconSmartGenerator: addr_width/data_width must be > 0";
        return false;
    }

    const std::string sis_dir = config.sis_dir;
    if (!ensure_directory(sis_dir)) {
        LOGE << "SiliconSmartGenerator: failed to create sis directory: " << sis_dir;
        return false;
    }

    const std::string netlist_dir = join_path(sis_dir, "netlist");
    const std::string control_dir = join_path(sis_dir, "control");

    if (!ensure_directory(sis_dir)) return false;
    if (!ensure_directory(netlist_dir)) return false;
    if (!ensure_directory(control_dir)) return false;

    const std::string run_tcl_path = join_path(sis_dir, "run.tcl");
    const std::string template_tcl_path = join_path(sis_dir, "template.tcl");
    const std::string configure_tcl_path = join_path(sis_dir, "configure.tcl");

    if (!write_text_file(run_tcl_path, build_run_tcl(config.cell_name))) {
        return false;
    }

    if (!write_text_file(template_tcl_path, build_template_tcl(config.cell_name, config.addr_width, config.data_width))) {
        return false;
    }

    std::string configure_content = build_configure_template();
    replace_all(configure_content, "{{ADDR_WIDTH}}", std::to_string(config.addr_width));
    replace_all(configure_content, "{{DATA_WIDTH}}", std::to_string(config.data_width));

    if (!write_text_file(configure_tcl_path, configure_content)) {
        return false;
    }

    const std::string netlist_dst = join_path(netlist_dir, config.cell_name + ".sp");
    if (!copy_file(config.flat_spice_path, netlist_dst)) {
        return false;
    }

    const std::string inst_path = join_path(control_dir, config.cell_name + ".inst");
    if (!write_text_file(inst_path, build_inst_file(config.cell_name, config.addr_width, config.data_width))) {
        return false;
    }

    LOGI << "SiliconSmart setup completed in: " << sis_dir;
    LOGI << "  Cell: " << config.cell_name;
    LOGI << "  Addr width: " << config.addr_width;
    LOGI << "  Data width: " << config.data_width;
    return true;
}

bool SiliconSmartGenerator::run_siliconsmart(const SiliconSmartConfig& config) const {
    const std::string log_path = join_path(config.sis_dir, "testcase/sis.log");

    LOGI << "Running SiliconSmart in: " << config.sis_dir;
    LOGI << "Monitoring log: " << log_path;

    pid_t pid = fork();
    if (pid < 0) {
        LOGE << "Failed to fork for SiliconSmart";
        return false;
    }

    if (pid == 0) {
        std::ostringstream cmd;
        cmd << "cd " << config.sis_dir << " && siliconsmart ./run.tcl";
        execlp("tcsh", "tcsh", "-c", cmd.str().c_str(), (char*)nullptr);
        _exit(127);
    }

    std::streampos log_offset = 0;
    bool error_found = false;
    int status = 0;

    while (true) {
        pid_t wait_rc = waitpid(pid, &status, WNOHANG);
        if (wait_rc == pid) {
            break;
        }

        if (scan_log_for_error(log_path, log_offset)) {
            error_found = true;
            LOGW << "SiliconSmart log reported error. Terminating SiliconSmart...";
            kill(pid, SIGTERM);

            for (int i = 0; i < 20; ++i) {
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    break;
                }
                usleep(100000);
            }

            if (waitpid(pid, &status, WNOHANG) == 0) {
                LOGW << "SiliconSmart did not exit after SIGTERM; sending SIGKILL";
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            break;
        }

        usleep(500000);
    }

    if (error_found) {
        return false;
    }

    if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        if (rc != 0) {
            LOGE << "SiliconSmart failed with code: " << rc;
            return false;
        }
    } else if (WIFSIGNALED(status)) {
        LOGE << "SiliconSmart terminated by signal: " << WTERMSIG(status);
        return false;
    }

    return true;
}

} // namespace OpenFinRAM
