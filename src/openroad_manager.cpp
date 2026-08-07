#include "openroad_manager.hpp"

#include <cmath>
#include <fstream>
#include "plog/Log.h"
#include "openroad_tcl_generator.hpp"
#include "utils.hpp"

OpenRoadManager::OpenRoadManager(const MainCliOptions& cli_options)
    : cli_options_(cli_options) {}

bool OpenRoadManager::run_openroad_flow() {
    OpenFinRAM::OpenRoadTclGenerator gen;
    gen.set_design_name("ctrl_decode");
    gen.set_site_name("asap7sc7p5t");
    gen.set_site_height(0.27);
    gen.set_bitcell_width(cli_options_.bitcell_width);

    // QoR and work_dir must match YosysManager's CWD-based tmp (repo/tmp) for consistency
    std::string qor_path_cwd = join_path(get_current_dir_name(), "tmp/syn_" + get_run_timestamp() + "/qor_report.txt");
    std::string qor_path_exe = join_path(get_executable_directory(), "tmp/syn_" + get_run_timestamp() + "/qor_report.txt");
    std::string qor_path = file_exists(qor_path_cwd) ? qor_path_cwd : qor_path_exe;
    if (!file_exists(qor_path)) qor_path = qor_path_cwd;
    bool qor_parsed = gen.parse_qor_report(qor_path);
    if (!qor_parsed) {
        LOGE << "Failed to parse QoR report for OpenROAD flow at " << qor_path;
        return false;
    }

    std::string work_dir = join_path(get_current_dir_name(), "tmp/openroad_" + get_run_timestamp());
    if (!directory_exists(work_dir)) {
        if (!create_directory(work_dir, nullptr) && !directory_exists(work_dir)) {
            std::string tmp_root = join_path(get_current_dir_name(), "tmp");
            if (!directory_exists(tmp_root)) create_directory(tmp_root, nullptr);
            if (!create_directory(work_dir, nullptr)) {
                LOGE << "Failed to create OpenROAD work dir: " << work_dir;
                return false;
            }
        }
    }
    // also ensure executable tmp exists for fallback
    if (!directory_exists(join_path(get_executable_directory(), "tmp"))) {
        create_directory(join_path(get_executable_directory(), "tmp"), nullptr);
    }

    std::string output_tcl = join_path(work_dir, "run.tcl");
    int addr_width = get_addr_width(cli_options_);
    int num_ysel = 4;
    double sram_width = 10.0;
    if (cli_options_.single_port) {
        sram_width = (cli_options_.bitcell_width * 2 + 2.376 + cli_options_.bitcell_width * ((cli_options_.num_wls + 3) * 2)) * cli_options_.num_banks - cli_options_.bitcell_width;
    } else {
        sram_width = 15.0;
    }
    double col_width = (sram_width + cli_options_.bitcell_width) / cli_options_.num_banks;
    // Prefer CWD tech (repo root) for tech_root; OpenROAD flow may be run from repo root
    std::string tech_root_cwd = join_path(get_current_dir_name(), "tech");
    std::string tech_root_exe = join_path(get_executable_directory(), "tech");
    std::string tech_root = directory_exists(tech_root_cwd) ? tech_root_cwd : tech_root_exe;
    std::string platform_path = cli_options_.platform_path;
    if (platform_path.empty()) platform_path = tech_root;
    // If platform_path was default ~/iv3/repos/OpenROAD/platform/asap7 but doesn't exist, fallback to tech_root
    if (!directory_exists(platform_path) && !file_exists(join_path(platform_path, "lef/asap7_tech.lef"))) {
        platform_path = tech_root;
    }

    if (!gen.generate_run_tcl(sram_width, 0.0, output_tcl,
                              cli_options_.num_wls, cli_options_.num_wls,
                              num_ysel, addr_width,
                              cli_options_.num_banks,
                              cli_options_.spice_only, col_width,
                              platform_path, tech_root)) {
        LOGE << "Failed to generate OpenROAD run.tcl";
        return false;
    }
    LOGI << "OpenROAD run.tcl generated at: " << output_tcl;

    std::string log_file = join_path(work_dir, "openroad.log");
    std::string openroad_bin = cli_options_.openroad_path;
    if (openroad_bin.empty()) openroad_bin = "openroad";
    bool or_ok = gen.run_openroad(output_tcl, work_dir, log_file, openroad_bin);
    std::string def_path = join_path(work_dir, "ctrl_decode.def");
    std::string v_path = join_path(work_dir, "netlist_for_lvs.v");
    if (!or_ok) {
        LOGE << "OpenROAD periphery flow failed (see " << log_file
             << "); refusing to use partial DEF/netlist artifacts";
        return false;
    }

    if (!file_exists(def_path) || !file_exists(v_path)) {
        LOGE << "OpenROAD completed without required DEF/Verilog outputs";
        return false;
    }

    std::string gds_path = join_path(work_dir, "ctrl_decode.gds");
    if (file_exists(def_path)) {
        auto resolve_platform_file = [&](const std::string& relative_path) {
            std::string platform_file = join_path(platform_path, relative_path);
            if (file_exists(platform_file)) return platform_file;
            return join_path(tech_root, relative_path);
        };

        std::string tech_lef = resolve_platform_file("lef/asap7_tech.lef");
        std::string cell_lef = resolve_platform_file("lef/asap7sc7p5t_28_R.lef");
        std::string macro_gds = resolve_platform_file("gds/asap7sc7p5t_28_R_220121a.gds");
        if (!gen.stream_def_to_gds(
                def_path, tech_lef, cell_lef, macro_gds, gds_path)) {
            LOGE << "Controller GDS stream-out failed; refusing to use the old DEF placeholder";
            return false;
        }
    }

    LOGI << "OpenROAD flow completed successfully";
    if (!file_exists(def_path)) {
        LOGE << "ctrl_decode.def not found at " << def_path;
        return false;
    }
    if (!file_exists(gds_path)) {
        LOGE << "ctrl_decode.gds not found at " << gds_path;
        return false;
    }
    if (!file_exists(v_path)) {
        LOGE << "netlist_for_lvs.v not found at " << v_path;
        return false;
    }

    std::string cdl_path = join_path(tech_root, "cdl/asap7sc7p5t_28_R.cdl");
    if (!file_exists(cdl_path)) {
        cdl_path = join_path(platform_path, "cdl/asap7sc7p5t_28_R.cdl");
    }
    if (!file_exists(cdl_path)) {
        cdl_path = join_path(get_current_dir_name(), "tech/cdl/asap7sc7p5t_28_R.cdl");
    }
    if (!file_exists(cdl_path)) {
        cdl_path = join_path(get_executable_directory(), "tech/cdl/asap7sc7p5t_28_R.cdl");
    }
    if (!gen.run_v2lvs(work_dir, "netlist_for_lvs.v", "netlist_for_lvs.sp", cdl_path)) {
        LOGE << "v2lvs (OpenROAD) failed";
        return false;
    }
    if (!gen.post_process_netlist(work_dir, "netlist_for_lvs.sp", cdl_path)) {
        LOGE << "Post-process (OpenROAD) failed";
        return false;
    }
    return true;
}
