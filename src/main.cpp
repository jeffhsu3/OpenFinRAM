#include "cell_utils.hpp"
#include "filler_generator.hpp"
#include "gdstk/gdstk.hpp"
#include "layermap.hpp"
#include "spice_generator.hpp"
#include "synthesis_manager.hpp"
#include "spice_converter.hpp"
#include "spice_integrator.hpp"
#include "spice_include_resolver.hpp"
#include "spice_simulator.hpp"
#include "innovus_tcl_generator.hpp"
#include "innovus_manager.hpp"
#include "siliconsmart_generator.hpp"
#include "siliconsmart_manager.hpp"
#include "lvs_runner.hpp"
#include "lef_extractor.hpp"
#include "layout_generator.hpp"
#include "main_config_helpers.hpp"
#include "main_flow_helpers.hpp"
#include "main_layout_helpers.hpp"
#include "utils.hpp"
#include "plog/Appenders/ColorConsoleAppender.h"
#include "plog/Formatters/TxtFormatter.h"
#include "plog/Init.h"
#include "plog/Log.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

// ============================================================================
// Global layer map instance
// ============================================================================
static OpenFinRAM::LayerMap g_layer_map;


// ============================================================================
// Main
// ============================================================================
int main(int argc, char **argv) {
    // Initialize plog
    static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
    plog::init(plog::debug, &consoleAppender);

    LOGI << "Starting OpenFinRAM application";

    // Parse CLI options
    MainCliOptions cli_options = parse_main_cli_options(argc, argv);

    // Generate SPICE netlist
    LOGI << "=== Generating SPICE Netlist ===";
    OpenFinRAM::SpiceGenerator gen(cli_options);
    if (gen.generate()) {
        LOGI << "SPICE netlist generation completed successfully.";
    } else {
        LOGE << "SPICE netlist generation failed.";
        return 1;
    }

    // Run synthesis flow
    SynthesisManager synth_manager(cli_options);
    if (!synth_manager.run_synthesis()) {
        LOGE << "Synthesis flow failed.";
        return 1;
    }

    // Run Innovus flow
    InnovusManager innovus_manager(cli_options);
    if (!innovus_manager.run_innovus_flow()) {
        LOGE << "Innovus flow failed.";
        return 1;
    }

    SpiceIntegrator integrator(cli_options);
    if (!integrator.integrate_sram()) {
        LOGE << "SRAM integration failed.";
        return 1;
    }

    // Run siliconsmart characterization
    SiliconSmartManager sis_manager(cli_options);
    if (!sis_manager.run_siliconsmart()) {
        LOGE << "SiliconSmart characterization failed.";
        return 1;
    }

    // Initialize ASAP7 Layer Map (hardcoded)
    LOGI << "Initializing ASAP7 layermap (hardcoded)...";
    g_layer_map.init_asap7_layermap();
    
    if (g_layer_map.empty()) {
        LOGE << "Failed to initialize layer map!";
        return 1;
    }

    LayoutGenerator layout_gen(cli_options, g_layer_map);
    if (!layout_gen.gen_layout()) {
        LOGE << "Layout generation failed!";
        return 1;
    }

    // // ========================================================================
    // // 讀取 Innovus 產生的 GDS 檔案並添加 Gate polygons
    // // ========================================================================
    // LOGI << "========================================================================";
    // LOGI << "Reading Innovus generated GDS file and adding Gate polygons";
    // LOGI << "========================================================================";
    
    // std::string gds_path = join_path(get_current_dir_name(), "tmp/innovus/ctrl_decode.gds");
    // LOGI << "Reading GDS file: " << gds_path;
    
    // // 讀取 GDS 檔案
    // gdstk::ErrorCode gds_error_code = gdstk::ErrorCode::NoError;
    // gdstk::Library gds_lib = gdstk::read_gds(gds_path.c_str(), 0, 1e-2, nullptr, &gds_error_code);
    
    // if (gds_error_code == gdstk::ErrorCode::NoError && gds_lib.cell_array.count > 0) {
    //     LOGI << "Successfully read GDS file";
    //     LOGI << "Number of cells in library: " << gds_lib.cell_array.count;

    //     add_ctrl_decode_gate_fin_wrappers(gds_lib, g_layer_map);
        
    //     // ================================================================
    //     // 建立 Filler Top 和 Bottom cells
    //     // ================================================================
    //     create_and_add_sram_filler_cells(gds_lib, sram_filler_lib, num_wls, g_layer_map);

    //     // ================================================================
    //     run_sram_gds_integration_and_writeback(
    //         gds_lib,
    //         gds_path,
    //         sram_array,
    //         filler_cgedge,
    //         io_colgrp,
    //         sram_cell_size,
    //         num_wls,
    //         num_data_bits,
    //         addr_width,
    //         num_banks,
    //         g_layer_map);
    // } else {
    //     LOGW << "Failed to read GDS file or no cells found";
    // }

    // consolidate_output_artifacts(num_wls, num_data_bits, num_banks);

    // // // ========================================================================
    // // // Run LVS (create lvs folder, generate _run_control.svrf, run calibre)
    // // // ========================================================================
    // // {
    // //     std::string sram_cell_name = "sram_x" + std::to_string(num_wls * 2) + "x" + std::to_string(num_data_bits);
    // //     std::string lvs_log_path;
    // //     std::string lvs_error;

    // //     LOGI << "Running LVS for cell: " << sram_cell_name;
    // //     bool lvs_ok = OpenFinRAM::run_lvs(".", "../innovus/ctrl_decode.gds.tmp", "../sram.sp", sram_cell_name, &lvs_log_path, &lvs_error);

    // //     if (lvs_ok) {
    // //         LOGI << "LVS completed. CORRECT.";
    // //     } else {
    // //         LOGW << "LVS failed or not correct. Log: " << lvs_log_path;
    // //         if (!lvs_error.empty()) {
    // //             LOGW << "LVS error: " << lvs_error;
    // //         }
    // //     }
    // // }

    // // // ========================================================================
    // // // Export LEF (create cds.lib, import GDS, run abstract)
    // // // ========================================================================
    // // {
    // //     std::string sram_cell_name = "sram_x" + std::to_string(num_wls * 2) + "x" + std::to_string(num_data_bits);
    // //     std::string lef_log_path;
    // //     std::string lef_error;

    // //     LOGI << "Exporting LEF for cell: " << sram_cell_name;
    // //     bool lef_ok = OpenFinRAM::export_lef(".", sram_cell_name, "./innovus/ctrl_decode.gds.tmp", &lef_log_path, &lef_error);

    // //     if (lef_ok) {
    // //         LOGI << "LEF export completed. Log: " << lef_log_path;
    // //     } else {
    // //         LOGW << "LEF export failed. Log: " << lef_log_path;
    // //         if (!lef_error.empty()) {
    // //             LOGW << "LEF export error: " << lef_error;
    // //         }
    // //     }
    // // }

    // // 釋放資源
    // sram_filler_lib.free_all();

    // return 0;
}
