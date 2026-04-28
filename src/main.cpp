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

    // // ========================================================================
    // // Initialize ASAP7 Layer Map (hardcoded)
    // // ========================================================================
    // LOGI << "Initializing ASAP7 layermap (hardcoded)...";
    // g_layer_map.init_asap7_layermap();
    
    // if (g_layer_map.empty()) {
    //     LOGE << "Failed to initialize layer map!";
    //     return 1;
    // }    

    // // ========================================================================
    // // Read SRAM Filler Library (for left/right fillers)
    // // ========================================================================
    // std::string sram_filler_gds_str = join_path(get_current_dir_name(), "tech/gds/srambank_32b_boundary_2.gds");
    // const char* sram_filler_gds = sram_filler_gds_str.c_str();
    // const char* sram_filler_name = "FILLER_BLANK_6t122";
    
    // LOGI << "Reading SRAM filler library: " << sram_filler_gds;
    // gdstk::ErrorCode error_code;
    // gdstk::Library sram_filler_lib = gdstk::read_gds(sram_filler_gds, 0, 1e-2, nullptr, &error_code);
    
    // if (error_code != gdstk::ErrorCode::NoError) {
    //     LOGE << "Error reading SRAM filler GDS!";
    //     return 1;
    // }
    
    // // 取得 SRAM filler cell
    // gdstk::Cell* sram_filler = sram_filler_lib.get_cell(sram_filler_name);
    
    // if (sram_filler == nullptr) {
    //     LOGE << "Cannot find cell '" << sram_filler_name << "' in SRAM filler library!";
    //     sram_filler_lib.free_all();
    //     return 1;
    // }
    
    // LOGI << "Found SRAM filler cell: " << sram_filler_name;

    // // ========================================================================
    // // 取得 SRAM Cell (6T bitcell)
    // // ========================================================================
    // const char* sram_cell_name = "sram_cell_6t_122";
    // gdstk::Cell* sram_cell = sram_filler_lib.get_cell(sram_cell_name);
    
    // if (sram_cell == nullptr) {
    //     LOGE << "Cannot find cell '" << sram_cell_name << "' in SRAM library!";
    //     sram_filler_lib.free_all();
    //     return 1;
    // }
    
    // LOGI << "Found SRAM cell: " << sram_cell_name;
    
    // // 取得 SRAM cell 尺寸
    // OpenFinRAM::CellSize sram_cell_size = OpenFinRAM::get_cell_size(sram_cell, g_layer_map);
    
    // if (!sram_cell_size.valid) {
    //     LOGW << "Cannot get SRAM cell size from BOUNDARY, using bounding box";
    //     gdstk::Vec2 bb_min, bb_max;
    //     sram_cell->bounding_box(bb_min, bb_max);
    //     sram_cell_size.min = bb_min;
    //     sram_cell_size.max = bb_max;
    //     sram_cell_size.width = bb_max.x - bb_min.x;
    //     sram_cell_size.height = bb_max.y - bb_min.y;
    //     sram_cell_size.valid = true;
    // }
    
    // LOGI << "SRAM cell '" << sram_cell_name << "' size: " 
    //      << sram_cell_size.width << " x " << sram_cell_size.height;
    // LOGI << "SRAM cell bounding box: (" << sram_cell_size.min.x << ", " << sram_cell_size.min.y 
    //      << ") to (" << sram_cell_size.max.x << ", " << sram_cell_size.max.y << ")";

    // // ========================================================================
    // // 列出 SRAM cell 中的 labels (查看 WL pin 位置)
    // // ========================================================================
    // LOGI << "=== Labels in SRAM cell '" << sram_cell_name << "' ===";
    // for (uint64_t i = 0; i < sram_cell->label_array.count; ++i) {
    //     gdstk::Label* label = sram_cell->label_array[i];
    //     LOGI << "  Label: '" << (label->text ? label->text : "(null)") << "'"
    //          << " at (" << label->origin.x << ", " << label->origin.y << ")"
    //          << " layer=" << gdstk::get_layer(label->tag) 
    //          << " texttype=" << gdstk::get_type(label->tag);
    // }
    
    // // 列出特定層的 polygon (例如 M3 pin layer = 30, datatype 251 因為 WL 在 M3)
    // LOGI << "=== Polygons on M3 pin layer in SRAM cell (WL related) ===";
    // for (uint64_t i = 0; i < sram_cell->polygon_array.count; ++i) {
    //     gdstk::Polygon* poly = sram_cell->polygon_array[i];
    //     // 檢查是否為 M3 pin layer (layer 30, datatype 251)
    //     if (poly->tag == gdstk::make_tag(30, 251)) {
    //         gdstk::Vec2 bb_min, bb_max;
    //         poly->bounding_box(bb_min, bb_max);
    //         LOGI << "  M3 Pin polygon: "
    //              << " bbox=(" << bb_min.x << "," << bb_min.y << ")-(" << bb_max.x << "," << bb_max.y << ")";
    //     }
    // }
    
    // // 列出 M3 drawing 層 (layer 30, datatype 0) 的 polygon
    // LOGI << "=== All M3 polygons in SRAM cell (layer 30) ===";
    // for (uint64_t i = 0; i < sram_cell->polygon_array.count; ++i) {
    //     gdstk::Polygon* poly = sram_cell->polygon_array[i];
    //     // 檢查是否為 M3 drawing layer (layer 30)
    //     if (gdstk::get_layer(poly->tag) == 30) {
    //         gdstk::Vec2 bb_min, bb_max;
    //         poly->bounding_box(bb_min, bb_max);
    //         double width = bb_max.x - bb_min.x;
    //         double height = bb_max.y - bb_min.y;
    //         LOGI << "  M3 polygon: datatype=" << gdstk::get_type(poly->tag)
    //              << " bbox=(" << bb_min.x << "," << bb_min.y << ")-(" << bb_max.x << "," << bb_max.y << ")"
    //              << " size=(" << width << "x" << height << ")";
    //     }
    // }

    // // ========================================================================
    // // 取得 dummy_sram_6t122 和 tapcell_sram_6t122
    // // ========================================================================
    // const char* dummy_cell_name = "dummy_sram_6t122";
    // const char* tapcell_name = "tapcell_sram_6t122";
    
    // gdstk::Cell* dummy_cell = sram_filler_lib.get_cell(dummy_cell_name);
    // if (dummy_cell == nullptr) {
    //     LOGW << "Cannot find cell '" << dummy_cell_name << "' in SRAM library!";
    // } else {
    //     LOGI << "Found dummy cell: " << dummy_cell_name;
        
    //     // 列出 dummy_cell 的 labels（查看 BL, BLN, VDD, VSS pin 位置）
    //     LOGI << "=== Labels in dummy_sram_6t122 ===";
    //     for (uint64_t i = 0; i < dummy_cell->label_array.count; ++i) {
    //         gdstk::Label* label = dummy_cell->label_array[i];
    //         LOGI << "  Label: '" << (label->text ? label->text : "(null)") << "'"
    //              << " at (" << label->origin.x << ", " << label->origin.y << ")"
    //              << " layer=" << gdstk::get_layer(label->tag) 
    //              << " texttype=" << gdstk::get_type(label->tag);
    //     }
    // }
    
    // gdstk::Cell* tapcell = sram_filler_lib.get_cell(tapcell_name);
    // if (tapcell == nullptr) {
    //     LOGW << "Cannot find cell '" << tapcell_name << "' in SRAM library!";
    // } else {
    //     LOGI << "Found tapcell: " << tapcell_name;
        
    //     // 列出 tapcell 的 labels（查看 VDD, VSS pin 位置）
    //     LOGI << "=== Labels in tapcell_sram_6t122 ===";
    //     for (uint64_t i = 0; i < tapcell->label_array.count; ++i) {
    //         gdstk::Label* label = tapcell->label_array[i];
    //         LOGI << "  Label: '" << (label->text ? label->text : "(null)") << "'"
    //              << " at (" << label->origin.x << ", " << label->origin.y << ")"
    //              << " layer=" << gdstk::get_layer(label->tag) 
    //              << " texttype=" << gdstk::get_type(label->tag);
    //     }
    // }

    // // ========================================================================
    // // 取得 dummy_topbot_v1 和 dummy_topbot_v2 (用於 SRAM array)
    // // ========================================================================
    // const char* dummy_topbot_v1_name = "dummy_topbot_v1";
    // const char* dummy_topbot_v2_name = "dummy_topbot_v2";
    
    // gdstk::Cell* dummy_topbot_v1 = sram_filler_lib.get_cell(dummy_topbot_v1_name);
    // if (dummy_topbot_v1 == nullptr) {
    //     LOGW << "Cannot find cell '" << dummy_topbot_v1_name << "' in SRAM library!";
    // } else {
    //     LOGI << "Found dummy_topbot_v1: " << dummy_topbot_v1_name;
    // }
    
    // gdstk::Cell* dummy_topbot_v2 = sram_filler_lib.get_cell(dummy_topbot_v2_name);
    // if (dummy_topbot_v2 == nullptr) {
    //     LOGW << "Cannot find cell '" << dummy_topbot_v2_name << "' in SRAM library!";
    // } else {
    //     LOGI << "Found dummy_topbot_v2: " << dummy_topbot_v2_name;
    // }

    // // ========================================================================
    // // 測試: 建立 SRAM Column
    // // ========================================================================
    // gdstk::Cell* sram_column = create_sram_column(sram_cell, dummy_cell, tapcell, num_wls, g_layer_map);
    
    // if (sram_column == nullptr) {
    //     LOGE << "Failed to create SRAM column!";
    //     sram_filler_lib.free_all();
    //     return 1;
    // }
    
    // // ========================================================================
    // // 建立 SRAM Array (4 層堆疊)
    // // ========================================================================
    // const uint64_t num_array_rows = 4;  // 堆疊 4 層
    // gdstk::Cell* sram_array = create_sram_array(sram_column, dummy_topbot_v1, dummy_topbot_v2, num_array_rows, g_layer_map);
    
    // if (sram_array == nullptr) {
    //     LOGE << "Failed to create SRAM array!";
    //     sram_filler_lib.free_all();
    //     return 1;
    // }

    // // ========================================================================
    // // 取得 FILLER_cgedge 和 iocolgrp_sram_6t122_v2 (用於 colgrp)
    // // ========================================================================
    // const char* filler_cgedge_name = "FILLER_cgedge";
    // const char* io_colgrp_name = "iocolgrp_sram_6t122_v2";
    
    // gdstk::Cell* filler_cgedge = sram_filler_lib.get_cell(filler_cgedge_name);
    // if (filler_cgedge == nullptr) {
    //     LOGW << "Cannot find cell '" << filler_cgedge_name << "' in SRAM library!";
    // } else {
    //     LOGI << "Found FILLER_cgedge: " << filler_cgedge_name;
    // }
    
    // gdstk::Cell* io_colgrp = sram_filler_lib.get_cell(io_colgrp_name);
    // if (io_colgrp == nullptr) {
    //     LOGW << "Cannot find cell '" << io_colgrp_name << "' in SRAM library!";
    // } else {
    //     LOGI << "Found io_colgrp: " << io_colgrp_name;
        
    //     // 列出 io_colgrp 中的所有 labels（用於找到信號位置）
    //     LOGI << "=== Labels in " << io_colgrp_name << " ===";
    //     for (uint64_t i = 0; i < io_colgrp->label_array.count; ++i) {
    //         gdstk::Label* label = io_colgrp->label_array[i];
    //         uint16_t layer = gdstk::get_layer(label->tag);
    //         uint16_t datatype = gdstk::get_type(label->tag);
    //         LOGI << "  Label: '" << label->text << "' at (" << label->origin.x << ", " 
    //              << label->origin.y << ") layer=" << layer << " texttype=" << datatype;
    //     }
    // }

    // // ========================================================================
    // // 建立 Column Group (colgrp_x{bit*2}x4)
    // // ========================================================================
    // gdstk::Cell* colgrp = nullptr;
    // if (filler_cgedge != nullptr && io_colgrp != nullptr) {
    //     colgrp = create_colgrp(sram_array, filler_cgedge, io_colgrp, num_wls, g_layer_map);
        
    //     if (colgrp == nullptr) {
    //         LOGE << "Failed to create column group!";
    //     }
    // } else {
    //     LOGW << "Skipping colgrp creation due to missing cells";
    // }

    // // ========================================================================
    // // 建立 Stacked Column Group (垂直堆疊 colgrp)
    // // ========================================================================
    // gdstk::Cell* stacked_colgrp = nullptr;
    // if (colgrp != nullptr) {
    //     char stacked_name[64];
    //     snprintf(stacked_name, sizeof(stacked_name), "stacked_colgrp_x%dx%lu", num_wls * 2, num_data_bits / 2);
        
    //     stacked_colgrp = create_stacked_colgrp(colgrp, num_data_bits / 2, stacked_name, g_layer_map);
        
    //     if (stacked_colgrp == nullptr) {
    //         LOGE << "Failed to create stacked column group!";
    //     }
    // } else {
    //     LOGW << "Skipping stacked colgrp creation due to missing colgrp";
    // }

    // // ========================================================================
    // // MUX: 水平並排 stacked_colgrp（即使 num_banks=1 也建立一致的 cell 名稱）
    // // ========================================================================
    // if (stacked_colgrp != nullptr && num_banks >= 1) {
    //     char muxed_name[96];
    //     snprintf(muxed_name, sizeof(muxed_name), "stacked_colgrp_x%dx%lux%lu", num_wls * 2, num_data_bits / 2, num_banks);

    //     gdstk::Cell* muxed_colgrp = create_muxed_colgrp(stacked_colgrp, num_banks, muxed_name, g_layer_map);
    //     if (muxed_colgrp == nullptr) {
    //         LOGE << "Failed to create muxed column group!";
    //     } else {
    //         stacked_colgrp = muxed_colgrp;
    //         LOGI << "Using muxed stacked_colgrp: " << stacked_colgrp->name;
    //     }
    // }

    // RuntimeDerivedParams derived_params = derive_runtime_params(num_wls, num_banks);
    // uint64_t num_wl = derived_params.num_wl;
    // uint64_t addr_width = derived_params.addr_width;
    // log_runtime_derived_params(derived_params);

    // // ========================================================================
    // // 輸出 SRAM column, SRAM array 和 colgrp 到同一個 GDS 檔案
    // // ========================================================================
    // {
    //     gdstk::Library sram_lib = {};
    //     sram_lib.init("SRAM_LIB", sram_filler_lib.unit, sram_filler_lib.precision);
        
    //     // Helper lambda: 加入 cell 及其相依（如果尚未存在）
    //     auto add_cell_with_deps = [&sram_lib](gdstk::Cell* cell) {
    //         if (cell == nullptr) return;
            
    //         // 加入 cell 本身
    //         if (sram_lib.get_cell(cell->name) == nullptr) {
    //             gdstk::Cell* cell_copy = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    //             cell_copy->copy_from(*cell, nullptr, true);
    //             sram_lib.cell_array.append(cell_copy);
    //         }
            
    //         // 加入相依 cells
    //         gdstk::Map<gdstk::Cell*> deps = {};
    //         cell->get_dependencies(true, deps);
    //         for (auto* item = deps.next(nullptr); item != nullptr; item = deps.next(item)) {
    //             if (sram_lib.get_cell(item->value->name) == nullptr) {
    //                 gdstk::Cell* dep_copy = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    //                 dep_copy->copy_from(*item->value, nullptr, true);
    //                 sram_lib.cell_array.append(dep_copy);
    //             }
    //         }
    //         deps.clear();
    //     };
        
    //     // 加入 colgrp（頂層 cell，如果存在）
    //     if (colgrp != nullptr) {
    //         sram_lib.cell_array.append(colgrp);
    //     }
        
    //     // 加入 stacked_colgrp（如果存在）
    //     if (stacked_colgrp != nullptr) {
    //         add_cell_with_deps(stacked_colgrp);
    //     }
        
    //     // 加入 sram_array
    //     add_cell_with_deps(sram_array);
        
    //     // 加入 sram_column 及其相依
    //     add_cell_with_deps(sram_column);
        
    //     // 加入 sram_cell 及其相依
    //     add_cell_with_deps(sram_cell);
        
    //     // 加入 dummy_cell 及其相依
    //     add_cell_with_deps(dummy_cell);
        
    //     // 加入 tapcell 及其相依
    //     add_cell_with_deps(tapcell);
        
    //     // 加入 dummy_topbot_v1 及其相依
    //     add_cell_with_deps(dummy_topbot_v1);
        
    //     // 加入 dummy_topbot_v2 及其相依
    //     add_cell_with_deps(dummy_topbot_v2);
        
    //     // 加入 FILLER_cgedge 及其相依
    //     add_cell_with_deps(filler_cgedge);
        
    //     // 加入 io_colgrp 及其相依
    //     add_cell_with_deps(io_colgrp);
        
    //     const char* sram_output = "sram_array_test.gds";
    //     LOGI << "Writing SRAM library to: " << sram_output;
    //     LOGI << "  Contains: " << sram_column->name << ", " << sram_array->name;
    //     if (colgrp != nullptr) {
    //         LOGI << "  Contains: " << colgrp->name;
    //     }
    //     if (stacked_colgrp != nullptr) {
    //         LOGI << "  Contains: " << stacked_colgrp->name;
    //     }
    //     error_code = sram_lib.write_gds(sram_output, 0, nullptr);
        
    //     if (error_code != gdstk::ErrorCode::NoError) {
    //         LOGE << "Error writing SRAM GDS!";
    //     } else {
    //         LOGI << "Successfully created SRAM library: " << sram_output;
    //     }
    // }

    

    // // ========================================================================
    // // PEX 策略選擇: 統計預測 vs 實際執行
    // // ========================================================================
    // // 設定為 true 則執行實際 PEX（耗時但精確）
    // // 設定為 false 則使用統計回歸預測（快速但基於歷史數據）
    // bool run_actual_pex = false;
    // bool pex_success = run_or_predict_pex(run_actual_pex, num_wls, num_data_bits);

    // // ========================================================================
    // // 開始合成流程 (Design Compiler synthesis with parameterized design)
    // // ========================================================================
    // uint64_t base_delay_cnt = 1;             // 其他 buffer 的初始數量
    // uint64_t delay_prech_cnt = 1;             // precharge buffer 初始數量 (約 1/10)
    // uint64_t attempt = 0;

    // // 使用二分搜尋來找最小可行的 buffer 數
    // const uint64_t min_buffer_cnt = 1;
    // const uint64_t max_buffer_cnt = 1;
    // uint64_t low_buffer = min_buffer_cnt;
    // uint64_t high_buffer = max_buffer_cnt;
    // uint64_t best_pass_buffer = 0;
    // bool force_best_run = false;

    // // 選擇測試模式：
    // // 1. Quick mode: 測試 4 個代表性地址
    // // 2. Parallel mode: 為每個 address bit 生成獨立的測試（可並行執行）
    // // 3. Random mode: 隨機選擇指定百分比的地址進行測試
    // // 4. Full mode: 測試所有地址（時間較長）
    // bool use_parallel_mode = false;  // 設為 true 使用並行測試
    // bool use_random_mode = true;     // 設為 true 使用隨機測試
    // double random_test_percentage = 10.0;  // 測試 10% 的記憶體位置
    // uint64_t random_seed = 0;        // 0 = 自動生成隨機種子，非0 = 指定種子以重現結果

    // bool verification_passed = false;
    // bool sis_passed = false;
    // if (!run_characterization) {
    //     LOGW << "Verification and SiliconSmart are disabled (run_characterization=0).";
    // }
    // while (!sis_passed) {
    //     if (!decide_base_delay_count(
    //             run_characterization,
    //             force_best_run,
    //             low_buffer,
    //             high_buffer,
    //             best_pass_buffer,
    //             min_buffer_cnt,
    //             max_buffer_cnt,
    //             base_delay_cnt)) {
    //         return 1;
    //     }

    //     if (attempt > 3) {
    //         LOGE << "Exceeded maximum synthesis attempts (3). Exiting.";
    //         return 1;
    //     }

    //     attempt++; 
    //     verification_passed = false;

    //     // delay_prech_cnt = std::max<uint64_t>(1, base_delay_cnt / 10);  // 10 倍差距

    //     run_synthesis_stage(
    //         attempt,
    //         addr_width,
    //         num_wls,
    //         num_banks,
    //         delay_prech_cnt,
    //         base_delay_cnt,
    //         num_data_bits,
    //         run_characterization,
    //         low_buffer,
    //         high_buffer,
    //         pex_success);

    //     run_spice_conversion_stage();

    //     run_innovus_stage(
    //         stacked_colgrp,
    //         num_wls,
    //         addr_width,
    //         num_banks,
    //         g_layer_map);

    //     run_sram_integration_stage(
    //         addr_width,
    //         num_data_bits,
    //         num_wls,
    //         num_banks);

    //     // ========================================================================
    //     // 開始 SPICE Simulation 驗證流程
    //     // ========================================================================
    //     verification_passed = run_spice_simulation_verification(
    //         run_characterization,
    //         use_random_mode,
    //         use_parallel_mode,
    //         random_test_percentage,
    //         random_seed,
    //         addr_width,
    //         num_data_bits,
    //         num_wl);

    //     if (run_characterization && !verification_passed) {
    //         if (!update_bisection_on_failure(
    //                 base_delay_cnt,
    //                 max_buffer_cnt,
    //                 best_pass_buffer,
    //                 low_buffer,
    //                 high_buffer,
    //                 force_best_run,
    //                 "verification failed")) {
    //             return 1;
    //         }
    //         continue;
    //     }

    //     if (!run_characterization) {
    //         LOGW << "Skipping SiliconSmart (run_characterization=0).";
    //         sis_passed = true;
    //         break;
    //     }

    //     bool sis_ok = run_siliconsmart_and_check(attempt, num_wls, num_data_bits, addr_width);
    //     if (!sis_ok) {
    //         LOGW << "SiliconSmart reported errors (found 'Error:   Task' in log).";
    //         if (!update_bisection_on_failure(
    //                 base_delay_cnt,
    //                 max_buffer_cnt,
    //                 best_pass_buffer,
    //                 low_buffer,
    //                 high_buffer,
    //                 force_best_run,
    //                 "SiliconSmart failed")) {
    //             return 1;
    //         }
    //         continue;
    //     }

    //     if (!update_bisection_on_success(
    //             base_delay_cnt,
    //             low_buffer,
    //             force_best_run,
    //             best_pass_buffer,
    //             high_buffer)) {
    //         sis_passed = true;
    //         break;
    //     }

    //     continue;
    // }

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
