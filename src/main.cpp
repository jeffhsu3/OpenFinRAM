#include "cell_utils.hpp"
#include "filler_generator.hpp"
#include "gdstk/gdstk.hpp"
#include "layermap.hpp"
#include "spice_generator.hpp"
#include "synthesis_manager.hpp"
#include "spice_converter.hpp"
#include "spice_integrator.hpp"
#include "spice_include_resolver.hpp"
#include "pex_runner.hpp"
#include "spice_simulator.hpp"
#include "innovus_tcl_generator.hpp"
#include "siliconsmart_generator.hpp"
#include "lvs_runner.hpp"
#include "lef_extractor.hpp"
#include "utils.hpp"
#include "plog/Appenders/ColorConsoleAppender.h"
#include "plog/Formatters/TxtFormatter.h"
#include "plog/Init.h"
#include "plog/Log.h"

#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
bool path_exists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

bool remove_directory_recursive(const std::string& path, std::string* error) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        if (error) {
            *error = std::string("lstat failed: ") + std::strerror(errno);
        }
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (::unlink(path.c_str()) != 0) {
            if (error) {
                *error = std::string("unlink failed: ") + std::strerror(errno);
            }
            return false;
        }
        return true;
    }

    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        if (error) {
            *error = std::string("opendir failed: ") + std::strerror(errno);
        }
        return false;
    }

    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
            continue;
        }

        std::string child = path;
        if (!child.empty() && child[child.size() - 1] != '/') {
            child += '/';
        }
        child += name;

        if (!remove_directory_recursive(child, error)) {
            ::closedir(dir);
            return false;
        }
    }

    ::closedir(dir);
    if (::rmdir(path.c_str()) != 0) {
        if (error) {
            *error = std::string("rmdir failed: ") + std::strerror(errno);
        }
        return false;
    }

    return true;
}

bool update_bisection_on_failure(
    uint64_t base_delay_cnt,
    uint64_t max_buffer_cnt,
    uint64_t best_pass_buffer,
    uint64_t& low_buffer,
    uint64_t high_buffer,
    bool& force_best_run,
    const char* failure_reason) {
    if (base_delay_cnt >= max_buffer_cnt && best_pass_buffer == 0) {
        LOGE << "Bisection failed: max_buffer_cnt=" << max_buffer_cnt << " also failed";
        return false;
    }

    low_buffer = base_delay_cnt + 1;
    if (low_buffer > high_buffer && best_pass_buffer > 0) {
        force_best_run = true;
    }
    LOGW << "Bisection update: " << failure_reason << ", new range = [" << low_buffer << ", " << high_buffer << "]";
    return true;
}

bool run_spice_simulation_verification(
    bool run_verification,
    bool use_random_mode,
    bool use_parallel_mode,
    double random_test_percentage,
    uint64_t random_seed,
    uint64_t addr_width,
    uint64_t num_stacked_rows,
    uint64_t num_wl) {
    bool verification_passed = false;

    if (run_verification) {
        LOGI << "\n========================================";
        LOGI << "Starting SPICE Simulation Verification";
        LOGI << "========================================\n";

        // 建立 simulation 配置 (使用與合成相同的 addr_width 計算)
        OpenFinRAM::SpiceSimConfig sim_config;
        sim_config.addr_bits = addr_width;  // 使用計算得到的 addr_width
        sim_config.data_bits = num_stacked_rows;
        sim_config.num_wordlines = num_wl * 2;  // 總 wordline 數 = WLT + WLB
        sim_config.sram_netlist = "./sram.sp";
        sim_config.output_dir = ".";
        sim_config.t_clk = 10.0;

        // 建立 simulator
        OpenFinRAM::SpiceSimulator simulator(sim_config);

        verification_passed = false;
        if (use_random_mode) {
            // 隨機測試模式：測試指定百分比的隨機地址
            LOGI << "Using random BIST mode";
            LOGI << "Test coverage: " << random_test_percentage << "% of memory";
            LOGI << "Total addresses: " << (1UL << addr_width);
            LOGI << "Addresses to test: " << static_cast<uint64_t>((1UL << addr_width) * random_test_percentage / 100.0);
            LOGI << "Random seed: " << (random_seed == 0 ? "auto-generated" : std::to_string(random_seed)) << "\n";

            std::string random_tb_file = sim_config.output_dir + "/sram_bist_tb_random.sp";

            if (simulator.generate_random_testbench(random_tb_file, random_test_percentage, random_seed)) {
                LOGI << "\n✓ Generated random testbench: " << random_tb_file;
                LOGI << "Running simulation...";

                if (simulator.run_simulation(random_tb_file, 8)) {
                    std::string mt0_file = random_tb_file;
                    if (mt0_file.size() > 3 && mt0_file.substr(mt0_file.size() - 3) == ".sp") {
                        mt0_file = mt0_file.substr(0, mt0_file.size() - 3);
                    }
                    mt0_file += ".mt0";

                    auto measurements = simulator.parse_measurements(mt0_file);
                    verification_passed = simulator.verify_results(measurements);
                } else {
                    LOGE << "Simulation failed";
                    verification_passed = false;
                }
            } else {
                LOGE << "Failed to generate random testbench";
                verification_passed = false;
            }
        } else if (use_parallel_mode) {
            // 並行測試模式：生成多個小型 testbench，每個測試一個 address bit
            LOGI << "Using parallel BIST mode - generating per-bit testbenches";
            LOGI << "This will generate " << addr_width << " testbenches (one per address bit)";
            LOGI << "Each testbench tests only 2 addresses for faster simulation";
            LOGI << "You can run these in parallel for maximum speed\n";

            // 生成並行測試文件
            std::vector<std::string> testbench_files = simulator.generate_parallel_testbenches();

            if (testbench_files.empty()) {
                LOGE << "Failed to generate parallel testbenches";
                verification_passed = false;
            } else {
                LOGI << "\n✓ Generated " << testbench_files.size() << " testbench files:";
                for (const auto& tb_file : testbench_files) {
                    LOGI << "  - " << tb_file;
                }

                LOGI << "\nTo run simulations in parallel, use:";
                LOGI << "  Option 1: Run all sequentially:";
                for (size_t i = 0; i < testbench_files.size(); ++i) {
                    LOGI << "    finesim -np 8 -w " << testbench_files[i]
                         << " -o sram_bist_tb_bit" << i;
                }
                LOGI << "\n  Option 2: Run in parallel (example with GNU parallel):";
                LOGI << "    parallel -j 4 'finesim -np 8 -w {} -o {.}' ::: sram_bist_tb_bit*.sp";

                LOGI << "\n  Option 3: Run first testbench only as a quick check:";
                LOGI << "    finesim -np 8 -w " << testbench_files[0]
                     << " -o sram_bist_tb_bit0";

                // 執行第一個 testbench 作為示例
                LOGI << "\nRunning first testbench (bit 0) as verification sample...";
                if (simulator.run_simulation(testbench_files[0], 8)) {
                    std::string mt0_file = testbench_files[0];
                    if (mt0_file.size() > 3 && mt0_file.substr(mt0_file.size() - 3) == ".sp") {
                        mt0_file = mt0_file.substr(0, mt0_file.size() - 3);
                    }
                    mt0_file += ".mt0";

                    auto measurements = simulator.parse_measurements(mt0_file);
                    verification_passed = simulator.verify_results(measurements);

                    if (verification_passed) {
                        LOGI << "✓ Sample testbench (bit 0) PASSED";
                        LOGI << "  All " << testbench_files.size()
                             << " testbenches are ready for parallel execution";
                    } else {
                        LOGW << "✗ Sample testbench (bit 0) FAILED";
                    }
                }
            }
        } else {
            // 快速測試模式：只測試幾個代表性地址
            LOGI << "Using quick BIST mode (testing 4 addresses)...";
            verification_passed = simulator.run_bist_verification(true);  // true = quick mode
        }
    } else {
        LOGW << "Skipping SPICE Simulation Verification (run_verification=0).";
        verification_passed = true;
    }

    if (run_verification) {
        LOGI << "\n========================================";
        if (verification_passed) {
            LOGI << "✓ SRAM verification PASSED - functional correctness confirmed!";
        } else {
            LOGW << "✗ SRAM verification FAILED - check simulation results";
        }
        LOGI << "========================================";
    } else {
        LOGW << "SRAM verification skipped (run_verification=0).";
    }

    return verification_passed;
}

void consolidate_output_artifacts(uint64_t test_num_bits, uint64_t num_stacked_rows, uint64_t num_mux) {
    // Make directory for results
    std::string results_dir = "results";
    if (!directory_exists(results_dir) && !create_directory(results_dir, nullptr)) {
        LOGW << "Failed to create results directory: " << results_dir;
    } else {
        LOGI << "Results directory created: " << results_dir;
    }

    // Move tmp/innovus/ctrl_decode.gds.tmp to results/{timestamp}/sramx{}.gds
    std::string timestamp = get_current_timestamp();
    std::string output_gds_path = join_path("tmp/innovus", "ctrl_decode.gds.tmp");
    std::string final_gds_name = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows) + "x" + std::to_string(num_mux) + ".gds";
    std::string final_gds_path = join_path(join_path(results_dir, timestamp), final_gds_name);
    if (!directory_exists(join_path(results_dir, timestamp)) && !create_directory(join_path(results_dir, timestamp), nullptr)) {
        LOGW << "Failed to create timestamped results directory: " << join_path(results_dir, timestamp);
    } else {
        if (std::rename(output_gds_path.c_str(), final_gds_path.c_str()) != 0) {
            LOGW << "Failed to move generated GDS file to final location: " << final_gds_path;
        } else {
            LOGI << "Generated GDS file moved to: " << final_gds_path;
        }
    }

    // ========================================================================
    // 展開 SRAM SPICE netlist 的 .inc/.include，輸出為 sram_flat.sp
    // ========================================================================
    {
        const std::string input_sp = join_path(get_current_dir_name(), "sram.sp");
        const std::string output_sp = join_path(get_current_dir_name(), "sram_flat.sp");
        std::string error;

        LOGI << "Expanding SPICE includes: " << input_sp << " -> " << output_sp;

        if (!SpiceIncludeResolver::resolve_to_file(input_sp, output_sp, error)) {
            LOGW << "Failed to expand SPICE includes: " << error;
        } else {
            LOGI << "Successfully generated flat SPICE netlist: " << output_sp;
        }
    }

    // Move sram_flat.sp to results/{timestamp}/sramx{}.sp
    std::string output_sp_path = "sram_flat.sp";
    std::string final_sp_name = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows) + "x" + std::to_string(num_mux) + ".sp";
    std::string final_sp_path = join_path(join_path(results_dir, timestamp), final_sp_name);
    if (std::rename(output_sp_path.c_str(), final_sp_path.c_str()) != 0) {
        LOGW << "Failed to move generated SP file to final location: " << final_sp_path;
    } else {
        LOGI << "Generated SP file moved to: " << final_sp_path;
    }
}

bool run_siliconsmart_and_check(
    uint64_t attempt,
    uint64_t test_num_bits,
    uint64_t num_stacked_rows,
    uint64_t addr_width) {
    // ========================================================================
    // 展開 SRAM SPICE netlist 的 .inc/.include，輸出為 sram_flat.sp
    // ========================================================================
    {
        const std::string input_sp = "./sram.sp";
        const std::string output_sp = "./sram_flat.sp";
        std::string error;

        LOGI << "Expanding SPICE includes: " << input_sp << " -> " << output_sp;

        if (!SpiceIncludeResolver::resolve_to_file(input_sp, output_sp, error)) {
            LOGW << "Failed to expand SPICE includes: " << error;
        } else {
            LOGI << "Successfully generated flat SPICE netlist: " << output_sp;
        }
    }

    // ========================================================================
    // 產生 SiliconSmart (SIS) 所需檔案
    // ========================================================================
    const std::string sis_dir = "./sis" + std::stoi(attempt < 10 ? "0" : "") + std::to_string(attempt);
    if (path_exists(sis_dir)) {
        LOGI << "Removing existing SiliconSmart directory: " << sis_dir;
        std::string remove_error;
        if (!remove_directory_recursive(sis_dir, &remove_error)) {
            LOGW << "Failed to remove SiliconSmart directory: " << remove_error;
        }
    }

    std::string sram_cell_name = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows);

    OpenFinRAM::SiliconSmartConfig sis_config;
    sis_config.cell_name = sram_cell_name;
    sis_config.addr_width = addr_width;
    sis_config.data_width = num_stacked_rows;
    sis_config.sis_dir = sis_dir;
    sis_config.flat_spice_path = "./sram_flat.sp";
    sis_config.configure_template_path = sis_dir + "/configure.tcl";

    OpenFinRAM::SiliconSmartGenerator sis_gen;
    bool sis_ok = sis_gen.generate(sis_config);
    if (!sis_ok) {
        LOGW << "Failed to generate SiliconSmart files";
    } else {
        sis_ok = sis_gen.run_siliconsmart(sis_config);
        if (!sis_ok) {
            LOGW << "Failed to run SiliconSmart";
        }
    }

    // ========================================================================
    // 檢查 SiliconSmart log 是否有錯誤
    // ========================================================================
    bool sis_has_error = !sis_ok;
    const std::string sis_log_path = sis_dir + "/testcase/sis.log";
    std::ifstream sis_log(sis_log_path);
    if (!sis_log.is_open()) {
        LOGW << "Cannot open SiliconSmart log: " << sis_log_path;
        sis_has_error = true;
    } else {
        std::string line;
        while (std::getline(sis_log, line)) {
            if (line.find("Error:   Task") != std::string::npos) {
                sis_has_error = true;
                break;
            }
        }
    }

    return !sis_has_error;
}
}  // namespace

// ============================================================================
// 全域 LayerMap 實例
// ============================================================================
static OpenFinRAM::LayerMap g_layer_map;

// ============================================================================
// 建立 SRAM Column (sramcol_x{bit})
// 
// 擺放規則：
// - bitcell 數量必須是偶數
// - 沿著 X 軸（水平方向）擺放成一個 row
// - 正著擺的 cells 用 repetition，間距為 2 * cell_width（隔一個 cell）
// - Y 軸翻轉的 cells 用 repetition，間距為 2 * cell_width，並有 cell_width 的 offset
// - 兩組交錯排列形成完整的 row
// - 最右邊加上 dummy_cell 和 tapcell
// ============================================================================
gdstk::Cell* create_sram_column(
    gdstk::Cell* sram_cell,           // SRAM bitcell
    gdstk::Cell* dummy_cell,          // dummy_sram_6t122 (在 row 最右邊)
    gdstk::Cell* tapcell,             // tapcell_sram_6t122 (在 dummy 右邊)
    uint64_t num_bits,                // bitcell 數量（必須是偶數）
    const OpenFinRAM::LayerMap& layer_map)
{
    // 檢查 bitcell 數量是否=0
    if (num_bits == 0) {
        LOGE << "Number of bits must be a positive number, got: " << num_bits;
        return nullptr;
    }
    
    // 取得 SRAM cell 尺寸
    OpenFinRAM::CellSize cell_size = OpenFinRAM::get_cell_size(sram_cell, layer_map);
    
    if (!cell_size.valid) {
        LOGW << "Cannot get SRAM cell size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        sram_cell->bounding_box(bb_min, bb_max);
        cell_size.min = bb_min;
        cell_size.max = bb_max;
        cell_size.width = bb_max.x - bb_min.x;
        cell_size.height = bb_max.y - bb_min.y;
        cell_size.valid = true;
    }
    
    double cell_width = cell_size.width;
    double cell_height = cell_size.height;
    
    LOGI << "Creating SRAM column with " << num_bits << " bits";
    LOGI << "SRAM cell size: " << cell_width << " x " << cell_height;
    
    // 計算每組的數量（正著擺和翻轉各一半）
    uint64_t num_normal = num_bits / 2;
    uint64_t num_flipped = num_bits / 2;
    
    // 建立 cell 名稱: sramcol_x{bit}
    char cell_name[64];
    snprintf(cell_name, sizeof(cell_name), "sramcol_x%lu", (unsigned long)num_bits);
    
    // 建立新的 Cell
    gdstk::Cell* column_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    column_cell->init(cell_name);
    
    // ========================================================================
    // 放置正著擺的 cells（位於 x = 0, 2w, 4w, ...）
    // ========================================================================
    gdstk::Reference* ref_normal = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
    ref_normal->init(sram_cell);
    ref_normal->origin = {0.0 - cell_size.min.x, 0.0 - cell_size.min.y};
    ref_normal->magnification = 1.0;
    
    if (num_normal > 1) {
        ref_normal->repetition.type = gdstk::RepetitionType::Rectangular;
        ref_normal->repetition.columns = num_normal;
        ref_normal->repetition.rows = 1;
        ref_normal->repetition.spacing = {2.0 * cell_width, 0.0};  // 間距為 2 倍寬度（水平方向）
    }
    
    column_cell->reference_array.append(ref_normal);
    
    // ========================================================================
    // 放置 Y 軸翻轉的 cells（位於 x = w, 3w, 5w, ...）
    // Y 軸翻轉 = x_reflection = true，然後旋轉 180 度
    // 或者用 magnification = -1 on x (但 gdstk 用 x_reflection)
    // ========================================================================
    gdstk::Reference* ref_flipped = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
    ref_flipped->init(sram_cell);
    // Y 軸翻轉（mirror about Y axis）：需要 rotation = 180 度 + x_reflection
    // 或者直接用負的 magnification（但 gdstk 不支援）
    // 實際上 Y 軸鏡射 = 先 x_reflection 再旋轉 180 度
    // 但更簡單的方式：直接設定 origin 並翻轉
    // 
    // Y 軸翻轉後，cell 的右邊變成左邊
    // 要讓翻轉後的 cell 左邊界對齊 x = cell_width，需要調整 origin
    ref_flipped->origin = {2.0 * cell_width - cell_size.min.x, 0.0 - cell_size.min.y};
    ref_flipped->rotation = M_PI;  // 旋轉 180 度
    ref_flipped->x_reflection = true;
    ref_flipped->magnification = 1.0;
    
    if (num_flipped > 1) {
        ref_flipped->repetition.type = gdstk::RepetitionType::Rectangular;
        ref_flipped->repetition.columns = num_flipped;
        ref_flipped->repetition.rows = 1;
        ref_flipped->repetition.spacing = {2.0 * cell_width, 0.0};  // 間距為 2 倍寬度（水平方向）
    }
    
    column_cell->reference_array.append(ref_flipped);
    
    // ========================================================================
    // 計算 SRAM bitcell 總寬度
    // ========================================================================
    double sram_total_width = num_bits * cell_width;
    double current_x = sram_total_width;  // 下一個 cell 的 x 位置
    
    // ========================================================================
    // 放置 dummy_cell（在 SRAM row 最右邊）
    // ========================================================================
    double dummy_width = 0.0;
    if (dummy_cell != nullptr) {
        OpenFinRAM::CellSize dummy_size = OpenFinRAM::get_cell_size(dummy_cell, layer_map);
        if (!dummy_size.valid) {
            gdstk::Vec2 bb_min, bb_max;
            dummy_cell->bounding_box(bb_min, bb_max);
            dummy_size.min = bb_min;
            dummy_size.max = bb_max;
            dummy_size.width = bb_max.x - bb_min.x;
            dummy_size.height = bb_max.y - bb_min.y;
        }
        dummy_width = dummy_size.width;
        
        gdstk::Reference* ref_dummy = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref_dummy->init(dummy_cell);
        ref_dummy->origin = {current_x - dummy_size.min.x, 0.0 - dummy_size.min.y};
        ref_dummy->magnification = 1.0;
        column_cell->reference_array.append(ref_dummy);
        
        LOGI << "  Added dummy_cell at x = " << current_x << " (width: " << dummy_width << ")";
        current_x += dummy_width;
    }
    
    // ========================================================================
    // 放置 tapcell（在 dummy 右邊）
    // ========================================================================
    double tapcell_width = 0.0;
    if (tapcell != nullptr) {
        OpenFinRAM::CellSize tapcell_size = OpenFinRAM::get_cell_size(tapcell, layer_map);
        if (!tapcell_size.valid) {
            gdstk::Vec2 bb_min, bb_max;
            tapcell->bounding_box(bb_min, bb_max);
            tapcell_size.min = bb_min;
            tapcell_size.max = bb_max;
            tapcell_size.width = bb_max.x - bb_min.x;
            tapcell_size.height = bb_max.y - bb_min.y;
        }
        tapcell_width = tapcell_size.width;
        
        gdstk::Reference* ref_tapcell = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref_tapcell->init(tapcell);
        ref_tapcell->origin = {current_x - tapcell_size.min.x, 0.0 - tapcell_size.min.y};
        ref_tapcell->magnification = 1.0;
        column_cell->reference_array.append(ref_tapcell);
        
        LOGI << "  Added tapcell at x = " << current_x << " (width: " << tapcell_width << ")";
        current_x += tapcell_width;
    }
    
    // ========================================================================
    // 計算整體尺寸並加入 BOUNDARY
    // ========================================================================
    double total_width = current_x;  // 包含 dummy 和 tapcell
    
    gdstk::Vec2 boundary_min = {0.0, 0.0};
    gdstk::Vec2 boundary_max = {total_width, cell_height};
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map);
    column_cell->polygon_array.append(boundary);
    
    // ========================================================================
    // 加入 WL (Word Line) Pins
    // 
    // WL 在 SRAM cell 中的資訊（從 sram_cell_6t_122 取得）:
    // - Label: 'WL' at (0.0555, -0.028) layer=30 (M3) texttype=251
    // 
    // 在 sramcol 中，每個 bitcell 有一個獨立的 WL
    // Pin 使用 label 在 pin layer (layer 30, datatype 251)，不繪製矩形
    // ========================================================================
    
    // 取得 M3 pin layer tag (layer 30, datatype 251)
    const OpenFinRAM::LayerDef* m3_pin_layer = layer_map.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
    
    if (m3_pin_layer == nullptr) {
        LOGW << "Cannot find M3 pin layer definition, skipping WL pins";
    } else {
        LOGI << "  Adding " << num_bits << " WL pins (WL[0] to WL[" << (num_bits-1) << "])";
        
        const double wl_y = -0.028;  // WL 的 y 位置（從 sram_cell）
        
        for (uint64_t i = 0; i < num_bits; ++i) {
            // 計算這個 bitcell 的中心 x 位置
            double wl_x = (i + 0.5) * cell_width;
            
            // 建立 WL[i] pin label（使用 pin layer）
            char wl_name[32];
            snprintf(wl_name, sizeof(wl_name), "WL[%lu]", (unsigned long)i);
            
            gdstk::Label* wl_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
            wl_label->init(wl_name);
            wl_label->origin = {wl_x, wl_y};
            wl_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
            column_cell->label_array.append(wl_label);
        }
        
        LOGI << "  Added " << num_bits << " WL pins on M3 pin layer (" 
             << m3_pin_layer->layer_number << ", " << m3_pin_layer->datatype << ")";
    }
    
    // ========================================================================
    // 加入 BL, BLN, VDD, VSS Pins (在 dummy_sram_6t122 位置)
    // 
    // 這些 pins 是整個 sramcol 共用的，所以只在 dummy_sram_6t122 位置打一個 pin
    // Pin 使用 label 在 pin layer (layer 20, datatype 251)，不繪製矩形
    // 
    // 從 dummy_sram_6t122 取得的 label 位置：
    // - BLN: (0.0175, 0.086) layer=20 (M2)
    // - vdd!: (0.082, 0.135) layer=20 (M2)
    // - vss!: (0.0735, 0.0365) layer=20 (M2) - 底部
    // - vss!: (0.0735, 0.235) layer=20 (M2) - 頂部
    // ========================================================================
    
    const OpenFinRAM::LayerDef* m2_pin_layer = layer_map.get_layer("M2", OpenFinRAM::LayerPurpose::Pin);
    
    if (m2_pin_layer == nullptr) {
        LOGW << "Cannot find M2 pin layer definition, skipping BL/BLN/VDD/VSS pins";
    } else if (dummy_cell != nullptr) {
        double sram_total_width = num_bits * cell_width;  // dummy_cell 的 x 位置
        
        LOGI << "  Adding BL/BLN/VDD/VSS pins at dummy_sram position (x=" << sram_total_width << ")";
        
        // 從 dummy_sram_6t122 的 label 位置（相對於 dummy cell）
        struct PinInfo {
            const char* name;
            double x_offset;  // 相對於 dummy_cell 左邊界的 x offset
            double y;         // y 座標
        };
        
        PinInfo pins[] = {
            {"BLN", 0.0175, 0.086},      // BLN pin
            {"VDD", 0.082, 0.135},       // VDD pin
            {"VSS", 0.0735, 0.0365},     // VSS pin (底部)
            {"VSS", 0.0735, 0.235},      // VSS pin (頂部)
        };
        
        for (const auto& pin_info : pins) {
            // 計算 pin 在 sramcol 中的絕對位置
            double pin_x = sram_total_width + pin_info.x_offset;
            double pin_y = pin_info.y;
            
            // 建立 pin label（使用 pin layer）
            gdstk::Label* label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
            label->init(pin_info.name);
            label->origin = {pin_x, pin_y};
            label->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
            column_cell->label_array.append(label);
        }
        
        LOGI << "  Added BL/BLN/VDD/VSS pins on M2 pin layer (" 
             << m2_pin_layer->layer_number << ", " << m2_pin_layer->datatype << ")";
    }
    
    LOGI << "Created SRAM column '" << cell_name << "'";
    LOGI << "  Normal cells: " << num_normal << " (at x = 0, 2w, 4w, ...)";
    LOGI << "  Flipped cells: " << num_flipped << " (at x = w, 3w, 5w, ...)";
    LOGI << "  Dummy cell: " << (dummy_cell ? dummy_cell->name : "none");
    LOGI << "  Tapcell: " << (tapcell ? tapcell->name : "none");
    LOGI << "  Total size: " << total_width << " x " << cell_height;
    
    return column_cell;
}

// ============================================================================
// 建立 SRAM Array (將多個 sramcol_x{bit} 垂直堆疊)
// 
// 擺放規則：
// - 接收一個 sramcol cell 和堆疊層數 (num_rows)
// - 沿著 Y 軸垂直堆疊
// - 第 0, 2, 4... 層正著擺
// - 第 1, 3, 5... 層朝 X 軸翻轉 (x_reflection = true)
// - 第 1, 3 層 (row index 0, 2) 在最左邊加入 dummy_topbot_v1
// - 第 2, 4 層 (row index 1, 3) 在最左邊加入 dummy_topbot_v2
// - 最後加上整體的 BOUNDARY
// ============================================================================
gdstk::Cell* create_sram_array(
    gdstk::Cell* sram_col,            // SRAM column cell (sramcol_x{bit})
    gdstk::Cell* dummy_topbot_v1,     // dummy_topbot_v1 (用於第 1, 3 層)
    gdstk::Cell* dummy_topbot_v2,     // dummy_topbot_v2 (用於第 2, 4 層)
    uint64_t num_rows,                // 堆疊層數
    const OpenFinRAM::LayerMap& layer_map)
{
    if (sram_col == nullptr || num_rows == 0) {
        LOGE << "Invalid parameters for create_sram_array";
        return nullptr;
    }
    
    // 取得 sramcol 尺寸
    OpenFinRAM::CellSize col_size = OpenFinRAM::get_cell_size(sram_col, layer_map);
    
    if (!col_size.valid) {
        LOGW << "Cannot get SRAM column size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        sram_col->bounding_box(bb_min, bb_max);
        col_size.min = bb_min;
        col_size.max = bb_max;
        col_size.width = bb_max.x - bb_min.x;
        col_size.height = bb_max.y - bb_min.y;
        col_size.valid = true;
    }
    
    double col_width = col_size.width;
    double col_height = col_size.height;
    
    // 取得 dummy_topbot_v1 尺寸
    double dummy_v1_width = 0.0;
    OpenFinRAM::CellSize dummy_v1_size = {};
    if (dummy_topbot_v1 != nullptr) {
        dummy_v1_size = OpenFinRAM::get_cell_size(dummy_topbot_v1, layer_map);
        if (!dummy_v1_size.valid) {
            gdstk::Vec2 bb_min, bb_max;
            dummy_topbot_v1->bounding_box(bb_min, bb_max);
            dummy_v1_size.min = bb_min;
            dummy_v1_size.max = bb_max;
            dummy_v1_size.width = bb_max.x - bb_min.x;
            dummy_v1_size.height = bb_max.y - bb_min.y;
            dummy_v1_size.valid = true;
        }
        dummy_v1_width = dummy_v1_size.width;
        LOGI << "dummy_topbot_v1 size: " << dummy_v1_size.width << " x " << dummy_v1_size.height;
    }
    
    // 取得 dummy_topbot_v2 尺寸
    double dummy_v2_width = 0.0;
    OpenFinRAM::CellSize dummy_v2_size = {};
    if (dummy_topbot_v2 != nullptr) {
        dummy_v2_size = OpenFinRAM::get_cell_size(dummy_topbot_v2, layer_map);
        if (!dummy_v2_size.valid) {
            gdstk::Vec2 bb_min, bb_max;
            dummy_topbot_v2->bounding_box(bb_min, bb_max);
            dummy_v2_size.min = bb_min;
            dummy_v2_size.max = bb_max;
            dummy_v2_size.width = bb_max.x - bb_min.x;
            dummy_v2_size.height = bb_max.y - bb_min.y;
            dummy_v2_size.valid = true;
        }
        dummy_v2_width = dummy_v2_size.width;
        LOGI << "dummy_topbot_v2 size: " << dummy_v2_size.width << " x " << dummy_v2_size.height;
    }
    
    // 計算 dummy 的最大寬度（用於計算整體偏移）
    double max_dummy_width = std::max(dummy_v1_width, dummy_v2_width);
    
    LOGI << "Creating SRAM array with " << num_rows << " rows";
    LOGI << "SRAM column size: " << col_width << " x " << col_height;
    LOGI << "Max dummy width: " << max_dummy_width;
    
    // 建立 cell 名稱: array_x{bits}x{rows}
    // 從 sramcol 名稱中提取 bits 數量
    const char* col_name = sram_col->name;
    int bits = 0;
    const char* x_pos = strstr(col_name, "_x");
    if (x_pos != nullptr) {
        bits = atoi(x_pos + 2);
    }
    
    char array_name[64];
    snprintf(array_name, sizeof(array_name), "array_x%dx%lu", bits, (unsigned long)num_rows);
    
    // 建立新的 Cell
    gdstk::Cell* array_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    array_cell->init(array_name);
    
    // sramcol 的 X 起始位置（在 dummy 右邊）
    double sramcol_x_offset = max_dummy_width;
    
    // ========================================================================
    // 逐層放置 sramcol 和對應的 dummy
    // - 偶數層 (0, 2, 4, ...): 正著擺，加入 dummy_topbot_v1 (Y軸翻轉)
    // - 奇數層 (1, 3, 5, ...): X 軸翻轉，加入 dummy_topbot_v2 (旋轉180度)
    // ========================================================================
    for (uint64_t row = 0; row < num_rows; row++) {
        double y_pos = row * col_height;
        
        // 放置 sramcol
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(sram_col);
        ref->magnification = 1.0;
        
        if (row % 2 == 0) {
            // 偶數層：正著擺
            ref->origin = {sramcol_x_offset - col_size.min.x, y_pos - col_size.min.y};
            ref->x_reflection = false;
            LOGI << "  Row " << row << ": sramcol normal at x=" << sramcol_x_offset << ", y=" << y_pos;
            
            // 放置 dummy_topbot_v1 在最左邊 (Y軸翻轉: rotation = PI + x_reflection = true)
            if (dummy_topbot_v1 != nullptr) {
                gdstk::Reference* dummy_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                dummy_ref->init(dummy_topbot_v1);
                // Y軸翻轉後，cell 的右邊變左邊，需要調整 x 座標
                dummy_ref->origin = {dummy_v1_size.width - dummy_v1_size.min.x, y_pos - dummy_v1_size.min.y};
                dummy_ref->magnification = 1.0;
                dummy_ref->rotation = M_PI;  // 旋轉 180 度
                dummy_ref->x_reflection = true;  // 加上 x_reflection = Y軸翻轉
                array_cell->reference_array.append(dummy_ref);
                LOGI << "  Row " << row << ": dummy_topbot_v1 (Y-flipped) at x=0, y=" << y_pos;
            }
        } else {
            // 奇數層：X 軸翻轉
            ref->origin = {sramcol_x_offset - col_size.min.x, y_pos + col_height - col_size.min.y};
            ref->x_reflection = true;
            LOGI << "  Row " << row << ": sramcol X-flipped at x=" << sramcol_x_offset << ", y=" << y_pos;
            
            // 放置 dummy_topbot_v2 在最左邊 (旋轉 180 度)
            if (dummy_topbot_v2 != nullptr) {
                gdstk::Reference* dummy_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                dummy_ref->init(dummy_topbot_v2);
                // 旋轉 180 度後，需要調整 origin 使左下角對齊
                // 因為旋轉 180 度，Y 座標需要下調 1 倍 cell height
                dummy_ref->origin = {dummy_v2_size.width - dummy_v2_size.min.x, 
                                     y_pos - dummy_v2_size.min.y};
                dummy_ref->magnification = 1.0;
                dummy_ref->rotation = M_PI;  // 旋轉 180 度
                dummy_ref->x_reflection = true;
                array_cell->reference_array.append(dummy_ref);
                LOGI << "  Row " << row << ": dummy_topbot_v2 (180° rotated) at x=0, y=" << y_pos;
            }
        }
        
        array_cell->reference_array.append(ref);
    }
    
    // ========================================================================
    // 計算整體尺寸並加入 BOUNDARY
    // ========================================================================
    double total_width = max_dummy_width + col_width;
    double total_height = num_rows * col_height;
    
    gdstk::Vec2 boundary_min = {0.0, 0.0};
    gdstk::Vec2 boundary_max = {total_width, total_height};
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map);
    array_cell->polygon_array.append(boundary);
    
    // ========================================================================
    // 加入 Pins for array_x{bit}x{rows}
    // 
    // 1. WL pins: 只在最下面的 row (row 0)，數量等於 bits
    //    - 從 sramcol 的 dummy_sram 位置推導
    //    - sramcol 中已經有 WL[0]~WL[bits-1]
    //    - 在 array 中，這些 WL 的位置需要加上 sramcol_x_offset
    // 
    // 2. BL/BLN pins: 每個 row 都有，所以有 num_rows 條
    //    - BL[0]~BL[num_rows-1], BLN[0]~BLN[num_rows-1]
    //    - 位置在每個 row 對應的 dummy_sram 位置
    // 
    // 3. VDD/VSS pins: 在 dummy_sram 位置（最下面的 row）
    // ========================================================================
    
    // 取得 M3 pin layer (for WL) 和 M2 pin layer (for BL/BLN/VDD/VSS)
    const OpenFinRAM::LayerDef* m3_pin_layer = layer_map.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
    const OpenFinRAM::LayerDef* m2_pin_layer = layer_map.get_layer("M2", OpenFinRAM::LayerPurpose::Pin);
    
    // === WL Pins (只在 row 0) ===
    if (m3_pin_layer != nullptr && bits > 0) {
        LOGI << "  Adding " << bits << " WL pins (WL[0] to WL[" << (bits-1) << "]) at row 0";
        
        const double wl_y = -0.028;  // WL 的 y 位置（從 sram_cell）
        const double cell_width = col_width / (bits + 1.216);  // 粗略估計 bitcell 寬度
        // 更精確的方式：從 sramcol 名稱推導
        // sramcol_x{bits} 包含 bits 個 SRAM cells + 1 dummy + 1 tapcell
        // SRAM cell width = 0.108, dummy = 0.108, tapcell = 0.108
        const double sram_cell_width = 0.108;
        
        for (int i = 0; i < bits; ++i) {
            // WL[i] 的 x 位置：sramcol_x_offset + (i + 0.5) * sram_cell_width
            double wl_x = sramcol_x_offset + (i + 0.5) * sram_cell_width;
            
            char wl_name[32];
            snprintf(wl_name, sizeof(wl_name), "WL[%d]", i);
            
            gdstk::Label* wl_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
            wl_label->init(wl_name);
            wl_label->origin = {wl_x, wl_y};
            wl_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
            array_cell->label_array.append(wl_label);
        }
        
        LOGI << "  Added " << bits << " WL pins on M3 pin layer";
    }
    
    // === BL/BLN Pins (每個 row 都有) ===
    if (m2_pin_layer != nullptr) {
        LOGI << "  Adding " << num_rows << " BL/BLN pin pairs (BL[0:" << (num_rows-1) 
             << "], BLN[0:" << (num_rows-1) << "])";
        
        // BLN 和 BL 在 dummy_sram_6t122 中的原始座標（相對於 dummy_sram 的原點）
        const double bln_x_in_dummy = 0.0175;  // dummy_sram 內的 x
        const double bln_y_in_dummy = 0.086;   // dummy_sram 內的 y
        
        // BL 在 sram_cell 中的位置（從 sram_cell_6t122 取得）
        const double bl_x_in_cell = 0.13;      // sram_cell 內的 x
        const double bl_y_in_cell = 0.1885;    // sram_cell 內的 y
        
        // dummy_sram 在 sramcol 中的位置
        const double dummy_x_in_col = bits * 0.108;  // sramcol 內，dummy 的 x 起點
        
        // BLN 相對於 sramcol 原點的座標
        const double bln_x_in_col = dummy_x_in_col + bln_x_in_dummy;
        const double bln_y_in_col = bln_y_in_dummy;
        
        // BL 在第一個 bitcell 中（x=0 開始）
        const double bl_x_in_col = bl_x_in_cell;
        const double bl_y_in_col = bl_y_in_cell;
        
        for (uint64_t row = 0; row < num_rows; ++row) {
            double bln_pin_x, bln_pin_y;
            double bl_pin_x, bl_pin_y;
            
            if (row % 2 == 0) {
                // 偶數 row (0, 2, ...): sramcol 正常放置
                // 排列：BL[row] 在下方，BLN[row] 在上方
                bln_pin_x = sramcol_x_offset + bln_x_in_col;
                bln_pin_y = row * col_height + bln_y_in_col;
                
                bl_pin_x = sramcol_x_offset + bl_x_in_col;
                bl_pin_y = row * col_height + bl_y_in_col;
                
                LOGI << "  Row " << row << " (normal): BL at (" << bl_pin_x << ", " << bl_pin_y 
                     << "), BLN at (" << bln_pin_x << ", " << bln_pin_y << ")";
            } else {
                // 奇數 row (1, 3, ...): sramcol X 軸翻轉
                // 排列：BLN[row] 在下方，BL[row] 在上方
                bln_pin_x = sramcol_x_offset + bln_x_in_col;
                bln_pin_y = (row + 1) * col_height - bln_y_in_col;
                
                bl_pin_x = sramcol_x_offset + bl_x_in_col;
                bl_pin_y = (row + 1) * col_height - bl_y_in_col;
                
                LOGI << "  Row " << row << " (X-flipped): BLN at (" << bln_pin_x << ", " << bln_pin_y 
                     << "), BL at (" << bl_pin_x << ", " << bl_pin_y << ")";
            }
            
            // BLN[row]
            char bln_name[32];
            snprintf(bln_name, sizeof(bln_name), "BLN[%lu]", (unsigned long)row);
            
            gdstk::Label* bln_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
            bln_label->init(bln_name);
            bln_label->origin = {bln_pin_x, bln_pin_y};
            bln_label->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
            array_cell->label_array.append(bln_label);
            
            // BL[row]
            char bl_name[32];
            snprintf(bl_name, sizeof(bl_name), "BL[%lu]", (unsigned long)row);
            
            gdstk::Label* bl_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
            bl_label->init(bl_name);
            bl_label->origin = {bl_pin_x, bl_pin_y};
            bl_label->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
            array_cell->label_array.append(bl_label);
        }
        
        LOGI << "  Added " << (num_rows * 2) << " BL/BLN pins on M2 pin layer";
        
        // === VDD/VSS Pins (在最下面的 row，dummy_sram 位置) ===
        const double vdd_x_offset = bits * 0.108 + 0.082;   // dummy_sram 位置 + VDD offset
        const double vss_x_offset = bits * 0.108 + 0.0735;  // dummy_sram 位置 + VSS offset
        const double vdd_y = 0.135;
        const double vss_y_bottom = 0.0365;
        const double vss_y_top = 0.235;
        
        // VDD pin
        gdstk::Label* vdd_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vdd_label->init("VDD");
        vdd_label->origin = {sramcol_x_offset + vdd_x_offset, vdd_y};
        vdd_label->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
        array_cell->label_array.append(vdd_label);
        
        // VSS pin (底部)
        gdstk::Label* vss_label_bottom = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vss_label_bottom->init("VSS");
        vss_label_bottom->origin = {sramcol_x_offset + vss_x_offset, vss_y_bottom};
        vss_label_bottom->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
        array_cell->label_array.append(vss_label_bottom);
        
        // VSS pin (頂部) 
        gdstk::Label* vss_label_top = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vss_label_top->init("VSS");
        vss_label_top->origin = {sramcol_x_offset + vss_x_offset, vss_y_top};
        vss_label_top->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
        array_cell->label_array.append(vss_label_top);
        
        LOGI << "  Added VDD and VSS pins on M2 pin layer";
    }
    
    LOGI << "Created SRAM array '" << array_name << "'";
    LOGI << "  Rows: " << num_rows;
    LOGI << "  Total size: " << total_width << " x " << total_height;
    
    return array_cell;
}

// ============================================================================
// 建立 Column Group (colgrp_x{bit*2}x4)
// 
// 擺放順序 (沿著 X 軸)：
// 1. FILLER_cgedge (最左邊)
// 2. array_x{bit}x4 (左側 SRAM array)
// 3. iocolgrp_sram_6t122_v2 (中間 IO)
// 4. array_x{bit}x4 (右側 SRAM array)
// 5. FILLER_cgedge (最右邊)
// ============================================================================
gdstk::Cell* create_colgrp(
    gdstk::Cell* sram_array,          // array_x{bit}x4 (會放置兩個)
    gdstk::Cell* filler_cgedge,       // FILLER_cgedge
    gdstk::Cell* io_colgrp,           // iocolgrp_sram_6t122_v2
    int bits,                         // 單個 array 的 bits 數量
    const OpenFinRAM::LayerMap& layer_map)
{
    if (sram_array == nullptr || filler_cgedge == nullptr || io_colgrp == nullptr) {
        LOGE << "Invalid parameters for create_colgrp";
        return nullptr;
    }
    
    // 取得各 cell 尺寸
    OpenFinRAM::CellSize array_size = OpenFinRAM::get_cell_size(sram_array, layer_map);
    OpenFinRAM::CellSize filler_size = OpenFinRAM::get_cell_size(filler_cgedge, layer_map);
    OpenFinRAM::CellSize io_size = OpenFinRAM::get_cell_size(io_colgrp, layer_map);
    
    if (!array_size.valid) {
        gdstk::Vec2 bb_min, bb_max;
        sram_array->bounding_box(bb_min, bb_max);
        array_size.min = bb_min;
        array_size.max = bb_max;
        array_size.width = bb_max.x - bb_min.x;
        array_size.height = bb_max.y - bb_min.y;
        array_size.valid = true;
    }
    
    if (!filler_size.valid) {
        gdstk::Vec2 bb_min, bb_max;
        filler_cgedge->bounding_box(bb_min, bb_max);
        filler_size.min = bb_min;
        filler_size.max = bb_max;
        filler_size.width = bb_max.x - bb_min.x;
        filler_size.height = bb_max.y - bb_min.y;
        filler_size.valid = true;
    }
    
    if (!io_size.valid) {
        gdstk::Vec2 bb_min, bb_max;
        io_colgrp->bounding_box(bb_min, bb_max);
        io_size.min = bb_min;
        io_size.max = bb_max;
        io_size.width = bb_max.x - bb_min.x;
        io_size.height = bb_max.y - bb_min.y;
        io_size.valid = true;
    }
    
    LOGI << "Creating column group with " << bits << " bits per array";
    LOGI << "  SRAM array size: " << array_size.width << " x " << array_size.height;
    LOGI << "  FILLER_cgedge size: " << filler_size.width << " x " << filler_size.height;
    LOGI << "  io_colgrp size: " << io_size.width << " x " << io_size.height;
    
    // 建立 cell 名稱: colgrp_x{bit*2}x4
    char colgrp_name[64];
    snprintf(colgrp_name, sizeof(colgrp_name), "colgrp_x%dx4", bits * 2);
    
    // 建立新的 Cell
    gdstk::Cell* colgrp_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    colgrp_cell->init(colgrp_name);
    
    double current_x = 0.0;
    double cell_height = array_size.height;  // 使用 array 的高度作為整體高度
    
    // 記錄 iocolgrp 的起始 x 座標（用於後續 via 計算）
    double left_array_x_start = filler_size.width;
    double iocolgrp_x_start = left_array_x_start + array_size.width;
    
    // ========================================================================
    // 1. 放置左側 FILLER_cgedge
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(filler_cgedge);
        ref->origin = {current_x - filler_size.min.x, 0.0 - filler_size.min.y};
        ref->magnification = 1.0;
        colgrp_cell->reference_array.append(ref);
        LOGI << "  Added left FILLER_cgedge at x=" << current_x;
        current_x += filler_size.width;
    }
    
    // ========================================================================
    // 2. 放置左側 array_x{bit}x4
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(sram_array);
        ref->origin = {current_x - array_size.min.x, 0.0 - array_size.min.y};
        ref->magnification = 1.0;
        colgrp_cell->reference_array.append(ref);
        LOGI << "  Added left SRAM array at x=" << current_x;
        current_x += array_size.width;
    }
    
    // ========================================================================
    // 3. 放置 iocolgrp_sram_6t122_v2
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(io_colgrp);
        ref->origin = {current_x - io_size.min.x, 0.0 - io_size.min.y + 0.0135};
        ref->magnification = 1.0;
        colgrp_cell->reference_array.append(ref);
        LOGI << "  Added io_colgrp at x=" << current_x;
        current_x += io_size.width;
    }
    
    // ========================================================================
    // 4. 放置右側 array_x{bit}x4 (Y軸翻轉)
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(sram_array);
        // Y軸翻轉後，cell 的右邊變左邊，需要調整 x 座標
        // origin.x = current_x + array_width 使翻轉後的左邊界對齊 current_x
        ref->origin = {current_x + array_size.width - array_size.min.x, 0.0 - array_size.min.y};
        ref->magnification = 1.0;
        ref->rotation = M_PI;
        ref->x_reflection = true;
        colgrp_cell->reference_array.append(ref);
        LOGI << "  Added right SRAM array (Y-flipped) at x=" << current_x;
        current_x += array_size.width;
    }
    
    // ========================================================================
    // 5. 放置右側 FILLER_cgedge
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(filler_cgedge);
        ref->origin = {current_x - filler_size.min.x, 0.0 - filler_size.min.y};
        ref->magnification = 1.0;
        colgrp_cell->reference_array.append(ref);
        LOGI << "  Added right FILLER_cgedge at x=" << current_x;
        current_x += filler_size.width;
    }
    
    // ========================================================================
    // 計算整體尺寸並加入 BOUNDARY
    // ========================================================================
    double total_width = current_x;
    
    gdstk::Vec2 boundary_min = {0.0, 0.0};
    gdstk::Vec2 boundary_max = {total_width, cell_height};
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map);
    colgrp_cell->polygon_array.append(boundary);
    
    // ========================================================================
    // 加入 WL Pins for colgrp_x{bit*2}x4
    // 左側 array: WLT[bits-1:0]
    // 右側 array: WLB[bits-1:0]
    // ========================================================================
    const OpenFinRAM::LayerDef* m3_pin_layer = layer_map.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
    
    if (m3_pin_layer != nullptr) {
        LOGI << "  Adding WL pins for colgrp";
        
        // WL 在 sram_cell 中的 y 位置（相對於 cell）
        const double wl_y = -0.028;
        
        // sram_cell 寬度
        const double cell_width = 0.108;
        
        // array 中 dummy_topbot 的寬度（在 sramcol 左側）
        const double dummy_width = 0.108;
        
        // 左側 array 的起始 x 位置（filler_cgedge 之後）
        const double left_array_x = filler_size.width;
        
        // 左側 array 中 sramcol 的起始 x（需要跳過 dummy_topbot）
        const double left_sramcol_x = left_array_x + dummy_width;
        
        // 右側 array 的起始 x 位置（filler + left_array + io_colgrp 之後）
        const double right_array_x = filler_size.width + array_size.width + io_size.width;
        
        // 右側 array 中 sramcol 的起始 x（Y軸翻轉後，dummy_topbot 在右側）
        // Y軸翻轉後：
        // 1. array 最左側是 dummy_topbot（寬度 0.108）
        // 2. 然後是 sramcol，sramcol 內部左側也有 dummy（寬度 0.108）
        // 3. 所以 bitcell 區域從 right_array_x + 0.108 + 0.108 = right_array_x + 0.216 開始
        const double right_sramcol_x = right_array_x + 2 * dummy_width;
        
        // 左側 array: WLT[bits-1:0]
        LOGI << "  Adding " << bits << " WLT pins (WLT[0] to WLT[" << (bits-1) << "])";
        LOGI << "  Left sramcol starts at x=" << left_sramcol_x;
        for (int i = 0; i < bits; ++i) {
            // WL 位於每個 bitcell 的中心
            double wl_x = left_sramcol_x + (i + 0.5) * cell_width;
            
            if (i == 0 || i == bits - 1) {
                LOGI << "    WLT[" << i << "] at x=" << wl_x;
            }
            
            char wl_name[32];
            snprintf(wl_name, sizeof(wl_name), "WLT[%d]", i);
            
            gdstk::Label* label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
            label->init(wl_name);
            label->origin = {wl_x, wl_y};
            label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
            colgrp_cell->label_array.append(label);
        }
        
        // 右側 array (Y軸翻轉): WLB[bits-1:0]
        // Y軸翻轉 = rotation 180° + x_reflection
        // 翻轉後，原本在左側的 bitcell 變到右側
        LOGI << "  Adding " << bits << " WLB pins (WLB[0] to WLB[" << (bits-1) << "])";
        LOGI << "  Right sramcol starts at x=" << right_sramcol_x;
        for (int i = 0; i < bits; ++i) {
            // 右側 array Y軸翻轉後
            // sramcol 寬度（不含 dummy）= bits * cell_width + dummy_sram_width + tapcell_width
            // 但 WL 只在 bitcell 區域，所以是 bits * cell_width
            const double sramcol_bitcell_width = bits * cell_width;
            
            // 原本 bitcell i 在相對座標 (i * cell_width + cell_width/2, 0)
            // Y軸翻轉後：x' = sramcol_bitcell_width - (i+0.5) * cell_width
            double wl_x = right_sramcol_x + sramcol_bitcell_width - (i + 0.5) * cell_width;
            
            if (i == 0 || i == bits - 1) {
                LOGI << "    WLB[" << i << "] at x=" << wl_x;
            }
            
            // Y 軸翻轉後，wl_y (-0.028) 變成 array_height - (-0.028) = array_height + 0.028
            double wl_y_flipped = array_size.height + wl_y;
            
            char wl_name[32];
            snprintf(wl_name, sizeof(wl_name), "WLB[%d]", i);
            
            gdstk::Label* label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
            label->init(wl_name);
            label->origin = {wl_x, wl_y_flipped};
            label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
            colgrp_cell->label_array.append(label);
        }
        
        LOGI << "  Added " << (bits * 2) << " WL pins on M3 pin layer (30, 251)";
    }
    
    // ========================================================================
    // 加入 iocolgrp 的信號 Pins
    // blprechtn, blprechbn, yselt[3:0], yseltn[3:0], yselb[3:0], yselbn[3:0]
    // 這些信號在 iocolgrp_sram_6t122_v2 中，需要加上 iocolgrp 的 x 偏移
    // ========================================================================
    if (m3_pin_layer != nullptr && io_colgrp != nullptr) {
        LOGI << "  Adding iocolgrp signal pins";
        
        // iocolgrp 在 colgrp 中的 x 起始位置
        const double io_x_offset = filler_size.width + array_size.width;
        
        // 從 iocolgrp cell 中取得所有 M3 pin layer 的 labels
        for (uint64_t i = 0; i < io_colgrp->label_array.count; ++i) {
            gdstk::Label* orig_label = io_colgrp->label_array[i];
            uint16_t layer = gdstk::get_layer(orig_label->tag);
            uint16_t datatype = gdstk::get_type(orig_label->tag);
            
            // 只處理 M3 pin layer (30, 251) 的信號
            if (layer == 30 && datatype == 251) {
                const char* name = orig_label->text;
                
                // 檢查是否為我們需要的信號
                bool is_target_signal = false;
                if (strncmp(name, "BLPRECHTN", 9) == 0 || 
                    strncmp(name, "BLPRECHBN", 9) == 0 ||
                    strncmp(name, "yselt<", 6) == 0 ||
                    strncmp(name, "yseltn<", 7) == 0 ||
                    strncmp(name, "yselb<", 6) == 0 ||
                    strncmp(name, "yselbn<", 7) == 0 ||
                    strcasecmp(name, "wrena") == 0 ||
                    // strcasecmp(name, "vsswrite") == 0 ||
                    strcasecmp(name, "saprechn") == 0 ||
                    strcasecmp(name, "sae") == 0 ||
                    strcasecmp(name, "wrenan") == 0 ||
                    strcasecmp(name, "wd") == 0 ||
                    strcasecmp(name, "saob") == 0 || 
                    strcasecmp(name, "oe_n") == 0) {
                    is_target_signal = true;
                }
                
                if (is_target_signal) {
                    // 將 pin 名稱中的 <> 轉換為 []
                    // 並處理特殊重命名：WD -> D, saob -> Q
                    char new_name[64];
                    
                    // 特殊重命名
                    if (strcasecmp(name, "wd") == 0) {
                        strcpy(new_name, "D");
                    } else if (strcasecmp(name, "saob") == 0) {
                        strcpy(new_name, "Q");
                    } else {
                        // 一般處理：<> 轉 []
                        const char* src = name;
                        char* dst = new_name;
                        while (*src && (dst - new_name) < 63) {
                            if (*src == '<') {
                                *dst++ = '[';
                            } else if (*src == '>') {
                                *dst++ = ']';
                            } else {
                                *dst++ = *src;
                            }
                            src++;
                        }
                        *dst = '\0';
                    }
                    
                    // 建立新的 label，位置加上 io_x_offset
                    gdstk::Label* label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                    label->init(new_name);
                    label->origin = {io_x_offset + orig_label->origin.x, orig_label->origin.y};
                    label->tag = orig_label->tag;
                    colgrp_cell->label_array.append(label);
                    
                    LOGI << "    Added pin '" << new_name << "' at (" 
                         << label->origin.x << ", " << label->origin.y << ")";
                }
            }
        }
    }
    
    // ========================================================================
    // 加入三根 M3 metal rectangles
    // 位置相對於 iocolgrp 左邊 boundary
    // ========================================================================
    const OpenFinRAM::LayerDef* m3_drawing_layer = layer_map.get_layer("M3", OpenFinRAM::LayerPurpose::Drawing);
    
    if (m3_drawing_layer != nullptr) {
        LOGI << "  Adding M3 metal rectangles in iocolgrp region";
        
        // iocolgrp 在 colgrp 中的 x 起始位置
        const double io_x_offset = filler_size.width + array_size.width;
        
        // Metal 參數：相對於 iocolgrp 左邊的 x 偏移、寬度
        const double metal_specs[][2] = {
            {0.531, 0.09},   // Metal 1: x_offset=0.531, width=0.09
            {1.071, 0.026}  // Metal 2: x_offset=1.071, width=0.026
            // {1.224, 0.036}   // Metal 3: x_offset=1.224, width=0.036
        };
        
        // Y 範圍：從 -0.0035 開始，高度 1.114
        const double metal_y_start = -0.0035;
        const double metal_height = 1.114;
        const double metal_y_end = metal_y_start + metal_height;
        
        for (int i = 0; i < 2; ++i) {
            double x_offset_from_io = metal_specs[i][0];
            double width = metal_specs[i][1];
            
            // 計算全局座標
            double x_left = io_x_offset + x_offset_from_io;
            double x_right = x_left + width;
            
            // 建立矩形 polygon
            gdstk::Polygon* rect = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
            gdstk::Vec2 points[4] = {
                {x_left, metal_y_start},
                {x_right, metal_y_start},
                {x_right, metal_y_end},
                {x_left, metal_y_end}
            };
            rect->point_array.extend({.capacity = 0, .count = 4, .items = points});
            rect->tag = gdstk::make_tag(m3_drawing_layer->layer_number, m3_drawing_layer->datatype);
            colgrp_cell->polygon_array.append(rect);
            
            LOGI << "    Added M3 metal " << (i+1) << " at x=[" << x_left << ", " << x_right 
                 << "], y=[" << metal_y_start << ", " << metal_y_end << "], width=" << width;
        }
    }
    
    // // ========================================================================
    // // 加入四條橫向 M4 metal rectangles
    // // ========================================================================
    // const OpenFinRAM::LayerDef* m4_drawing_layer = layer_map.get_layer("M4", OpenFinRAM::LayerPurpose::Drawing);
    
    // if (m4_drawing_layer != nullptr) {
    //     LOGI << "  Adding M4 metal rectangles";
        
    //     // iocolgrp 在 colgrp 中的 x 起始位置
    //     const double io_x_offset = filler_size.width + array_size.width;
        
    //     // 左側 array 的起始 x 位置（從 filler_cgedge 之後開始）
    //     const double left_array_x_start = filler_size.width;
        
    //     // 右側 array 的結束 x 位置（io_colgrp 之後 + array_size.width）
    //     const double right_array_x_end = io_x_offset + io_size.width + array_size.width;
        
    //     // M4 Metal 1: 離 iocolgrp 左邊 0.6, 寬度 0.716, 高度 0.024
    //     // y 位置離 x=0 (即 y=0) 0.0755
    //     {
    //         double x_left = io_x_offset + 0.6;
    //         double x_right = x_left + 0.716;
    //         double y_bottom = 0.0755;
    //         double y_top = y_bottom + 0.024;
            
    //         gdstk::Polygon* rect = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         rect->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         rect->tag = gdstk::make_tag(m4_drawing_layer->layer_number, m4_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(rect);
            
    //         LOGI << "    Added M4 metal 1 at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "], width=" << 0.716 << ", height=0.024";
    //     }
        
    //     // M4 Metal 2: 離 iocolgrp 左邊 0.6, 寬度 0.648, 高度 0.024
    //     // y 位置離 x=0 (即 y=0) 0.1715
    //     {
    //         double x_left = io_x_offset + 0.6;
    //         double x_right = x_left + 0.648;
    //         double y_bottom = 0.1715;
    //         double y_top = y_bottom + 0.024;
            
    //         gdstk::Polygon* rect = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         rect->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         rect->tag = gdstk::make_tag(m4_drawing_layer->layer_number, m4_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(rect);
            
    //         LOGI << "    Added M4 metal 2 at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "], width=" << 0.648 << ", height=0.024";
    //     }
        
    //     // M4 Metal 3: 離 y=0 0.8195，橫跨左右兩個 array，高 0.048
    //     {
    //         double x_left = left_array_x_start - 0.007;
    //         double x_right = right_array_x_end;
    //         double y_bottom = 0.8195;
    //         double y_top = y_bottom + 0.048;
            
    //         gdstk::Polygon* rect = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         rect->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         rect->tag = gdstk::make_tag(m4_drawing_layer->layer_number, m4_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(rect);
            
    //         LOGI << "    Added M4 metal 3 at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "], width=" << (x_right - x_left) << ", height=0.048";
    //     }
        
    //     // M4 Metal 4: 離 y=0 0.9155，橫跨左右兩個 array，高 0.048
    //     {
    //         double x_left = left_array_x_start - 0.007;
    //         double x_right = right_array_x_end + 0.005;
    //         double y_bottom = 0.9155;
    //         double y_top = y_bottom + 0.048;
            
    //         gdstk::Polygon* rect = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         rect->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         rect->tag = gdstk::make_tag(m4_drawing_layer->layer_number, m4_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(rect);
            
    //         LOGI << "    Added M4 metal 4 at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "], width=" << (x_right - x_left) << ", height=0.048";
    //     }
    // }
    
    // // ========================================================================
    // // 加入 V3 (Via3) 來連接 M3 和 M4
    // // ========================================================================
    // const OpenFinRAM::LayerDef* v3_drawing_layer = layer_map.get_layer("V3", OpenFinRAM::LayerPurpose::Drawing);
    
    // if (v3_drawing_layer != nullptr) {
    //     LOGI << "  Adding V3 vias to connect M3 and M4";
        
    //     // iocolgrp 在 colgrp 中的 x 起始位置
    //     const double io_x_offset = filler_size.width + array_size.width;
        
    //     // Via 尺寸
    //     const double via_width = 0.018;
    //     const double via_height = 0.024;
        
    //     // V3 for M4 Metal 1: 離 iocolgrp 左邊 0.963，從 y=0.0755 開始（對齊 M4 metal 1）
    //     {
    //         double x_left = io_x_offset + 0.963;
    //         double x_right = x_left + via_width;
    //         double y_bottom = 0.0755;
    //         double y_top = y_bottom + via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via 1 (for M4 metal 1) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "], size=" << via_width << "x" << via_height;
    //     }
        
    //     // V3 for M4 Metal 2: 離 iocolgrp 左邊 1.188，從 y=0.1715 開始（對齊 M4 metal 2）
    //     {
    //         double x_left = io_x_offset + 1.188;
    //         double x_right = x_left + via_width;
    //         double y_bottom = 0.1715;
    //         double y_top = y_bottom + via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via 2 (for M4 metal 2) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "], size=" << via_width << "x" << via_height;
    //     }
        
    //     // ========================================================================
    //     // V3 for M4 Metal 3 和 Metal 4 (連接到 VDD 和 VSS)
    //     // Metal 3 連接到 VSS，Metal 4 連接到 VDD
    //     // 需要在左右兩個 array 的 dummy cell 位置各放置 via
    //     // ========================================================================
        
    //     // 左側 array 的起始 x 位置
    //     const double left_array_x_start = filler_size.width;
        
    //     // 右側 array 的起始 x 位置（io_colgrp 之後）
    //     const double right_array_x_start = io_x_offset + io_size.width;
        
    //     // array 中 dummy_topbot 的寬度
    //     const double dummy_width = 0.108;
        
    //     // VDD/VSS via 尺寸
    //     const double power_via_width = 0.018;
    //     const double power_via_height = 0.048;  // M4 metal 的高度
        
    //     // M4 Metal 3 (VSS) 的 y 位置: 0.8195
    //     const double metal3_y = 0.8195;
        
    //     // M4 Metal 4 (VDD) 的 y 位置: 0.9155
    //     const double metal4_y = 0.9155;
        
    //     // dummy_sram_6t122 中 VDD 和 VSS 的相對位置
    //     const double vss_x_in_dummy = 0.0735;  // VSS x 位置在 dummy 中
    //     const double vdd_x_in_dummy = 0.082;   // VDD x 位置在 dummy 中
        
    //     // 1. 左側 array 左邊的 dummy cell - VSS (M4 Metal 3)
    //     {
    //         double x_left = left_array_x_start + vss_x_in_dummy - 0.0195 - power_via_width / 2.0;
    //         double x_right = x_left + power_via_width;
    //         double y_bottom = metal3_y;
    //         double y_top = y_bottom + power_via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via for VSS (left array, left dummy) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "]";
    //     }
        
    //     // 2. 左側 array 左邊的 dummy cell - VDD (M4 Metal 4)
    //     {
    //         double x_left = left_array_x_start + vdd_x_in_dummy - 0.064 - power_via_width / 2.0;
    //         double x_right = x_left + power_via_width;
    //         double y_bottom = metal4_y;
    //         double y_top = y_bottom + power_via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via for VDD (left array, left dummy) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "]";
    //     }
        
    //     // 3. 右側 array 右邊的 dummy cell - VSS (M4 Metal 3)
    //     // 右側 array 是 Y 軸翻轉，所以 dummy 在右邊（最右側）
    //     // Y 軸翻轉後，vss_x_in_dummy 的位置也要翻轉
    //     {
    //         double x_left = right_array_x_start + array_size.width - vss_x_in_dummy + 0.0195 - power_via_width / 2.0;
    //         double x_right = x_left + power_via_width;
    //         double y_bottom = metal3_y;
    //         double y_top = y_bottom + power_via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via for VSS (right array, right dummy) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "]";
    //     }
        
    //     // 4. 右側 array 右邊的 dummy cell - VDD (M4 Metal 4)
    //     {
    //         double x_left = right_array_x_start + array_size.width - vdd_x_in_dummy + 0.064 - power_via_width / 2.0;
    //         double x_right = x_left + power_via_width;
    //         double y_bottom = metal4_y;
    //         double y_top = y_bottom + power_via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via for VDD (right array, right dummy) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "]";
    //     }
        
    //     // ========================================================================
    //     // array_x{bit}x4 右邊也有 dummy_sram_6t122，需要加入 VDD/VSS via
    //     // ========================================================================
        
    //     // 左側 array 的右邊 dummy (在 sramcol 的右側)
    //     // sramcol 寬度 = bits * 0.108，dummy_sram 在 sramcol 的右側
    //     const double left_array_right_dummy_x = left_array_x_start + bits * 0.108;
        
    //     // 5. 左側 array 右邊的 dummy cell - VSS (M4 Metal 3)
    //     {
    //         double x_left = left_array_right_dummy_x + vss_x_in_dummy - 0.0195 - power_via_width / 2.0 + 0.108;
    //         double x_right = x_left + power_via_width;
    //         double y_bottom = metal3_y;
    //         double y_top = y_bottom + power_via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via for VSS (left array, right dummy) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "]";
    //     }
        
    //     // 6. 左側 array 右邊的 dummy cell - VDD (M4 Metal 4)
    //     {
    //         double x_left = left_array_right_dummy_x + vdd_x_in_dummy - 0.064 - power_via_width / 2.0 + 0.198;
    //         double x_right = x_left + power_via_width;
    //         double y_bottom = metal4_y;
    //         double y_top = y_bottom + power_via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via for VDD (left array, right dummy) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "]";
    //     }
        
    //     // 右側 array 的左邊 dummy (Y軸翻轉後，sramcol 在左側，dummy 也在左側)
    //     // 右側 array Y軸翻轉後，dummy_topbot 在最左側 (寬度 0.108)，然後是 sramcol
    //     const double right_array_left_dummy_x = right_array_x_start + dummy_width;
        
    //     // 7. 右側 array 左邊的 dummy cell (sramcol 左側) - VSS (M4 Metal 3)
    //     // Y軸翻轉後，原本在右側的 dummy_sram 變到左側
    //     {
    //         double x_left = right_array_left_dummy_x + dummy_width - vss_x_in_dummy + 0.0195 - power_via_width / 2.0;
    //         double x_right = x_left + power_via_width;
    //         double y_bottom = metal3_y;
    //         double y_top = y_bottom + power_via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via for VSS (right array, left dummy) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "]";
    //     }
        
    //     // 8. 右側 array 左邊的 dummy cell (sramcol 左側) - VDD (M4 Metal 4)
    //     {
    //         double x_left = right_array_left_dummy_x + dummy_width - vdd_x_in_dummy + 0.064 - power_via_width / 2.0 - 0.09;
    //         double x_right = x_left + power_via_width;
    //         double y_bottom = metal4_y;
    //         double y_top = y_bottom + power_via_height;
            
    //         gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //         gdstk::Vec2 points[4] = {
    //             {x_left, y_bottom},
    //             {x_right, y_bottom},
    //             {x_right, y_top},
    //             {x_left, y_top}
    //         };
    //         via->point_array.extend({.capacity = 0, .count = 4, .items = points});
    //         via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //         colgrp_cell->polygon_array.append(via);
            
    //         LOGI << "    Added V3 via for VDD (right array, left dummy) at x=[" << x_left << ", " << x_right 
    //              << "], y=[" << y_bottom << ", " << y_top << "]";
    //     }
    // }
    
    // // 新增兩個額外的 V3 via 在 iocolgrp 區域
    // {
    //     // Via 1: x = iocolgrp_x + 0.531, width = 0.09, y = 0.9145
    //     double via1_x_left = iocolgrp_x_start + 0.531;
    //     double via1_width = 0.09;
    //     double via1_x_right = via1_x_left + via1_width;
    //     double via1_y_bottom = 0.9155;
    //     double via1_y_top = via1_y_bottom + 0.048;  // V3 via height
        
    //     gdstk::Polygon* via1 = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //     via1->point_array.ensure_slots(4);
    //     gdstk::Vec2 via1_points[] = {
    //         {via1_x_left, via1_y_bottom},
    //         {via1_x_right, via1_y_bottom},
    //         {via1_x_right, via1_y_top},
    //         {via1_x_left, via1_y_top}
    //     };
    //     via1->point_array.extend({.capacity = 0, .count = 4, .items = via1_points});
    //     via1->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //     colgrp_cell->polygon_array.append(via1);
        
    //     LOGI << "    Added V3 via (iocolgrp region 1) at x=[" << via1_x_left << ", " << via1_x_right 
    //          << "], y=[" << via1_y_bottom << ", " << via1_y_top << "]";
        
    //     // Via 2: x = iocolgrp_x + 1.071, width = 0.027, y = 0.8195
    //     double via2_x_left = iocolgrp_x_start + 1.071;
    //     double via2_width = 0.027;
    //     double via2_x_right = via2_x_left + via2_width;
    //     double via2_y_bottom = 0.8195;
    //     double via2_y_top = via2_y_bottom + 0.048;  // V3 via height
        
    //     gdstk::Polygon* via2 = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //     via2->point_array.ensure_slots(4);
    //     gdstk::Vec2 via2_points[] = {
    //         {via2_x_left, via2_y_bottom},
    //         {via2_x_right, via2_y_bottom},
    //         {via2_x_right, via2_y_top},
    //         {via2_x_left, via2_y_top}
    //     };
    //     via2->point_array.extend({.capacity = 0, .count = 4, .items = via2_points});
    //     via2->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //     colgrp_cell->polygon_array.append(via2);
        
    //     LOGI << "    Added V3 via (iocolgrp region 2) at x=[" << via2_x_left << ", " << via2_x_right 
    //          << "], y=[" << via2_y_bottom << ", " << via2_y_top << "]";
        
    //     // Via 3: x = iocolgrp_x + 1.224, width = 0.036, y = 0.8195
    //     double via3_x_left = iocolgrp_x_start + 1.224;
    //     double via3_width = 0.036;
    //     double via3_x_right = via3_x_left + via3_width;
    //     double via3_y_bottom = 0.8195;
    //     double via3_y_top = via3_y_bottom + 0.048;  // V3 via height
        
    //     gdstk::Polygon* via3 = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    //     via3->point_array.ensure_slots(4);
    //     gdstk::Vec2 via3_points[] = {
    //         {via3_x_left, via3_y_bottom},
    //         {via3_x_right, via3_y_bottom},
    //         {via3_x_right, via3_y_top},
    //         {via3_x_left, via3_y_top}
    //     };
    //     via3->point_array.extend({.capacity = 0, .count = 4, .items = via3_points});
    //     via3->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
    //     colgrp_cell->polygon_array.append(via3);
        
    //     LOGI << "    Added V3 via (iocolgrp region 3) at x=[" << via3_x_left << ", " << via3_x_right 
    //          << "], y=[" << via3_y_bottom << ", " << via3_y_top << "]";
    // }
    
    LOGI << "Created column group '" << colgrp_name << "'";
    LOGI << "  Total size: " << total_width << " x " << cell_height;
    
    return colgrp_cell;
}

// ============================================================================
// 垂直堆疊 colgrp cells
// 將多個 colgrp 沿著 Y 軸（垂直方向）堆疊，產生新的 cell
// ============================================================================
gdstk::Cell* create_stacked_colgrp(
    gdstk::Cell* colgrp,              // colgrp cell (會重複堆疊)
    uint64_t num_rows,                // 堆疊的列數
    const char* stacked_name,         // 新 cell 的名稱
    const OpenFinRAM::LayerMap& layer_map)
{
    if (colgrp == nullptr || num_rows == 0) {
        LOGE << "Invalid parameters for create_stacked_colgrp";
        return nullptr;
    }
    
    // 取得 colgrp 尺寸
    OpenFinRAM::CellSize colgrp_size = OpenFinRAM::get_cell_size(colgrp, layer_map);
    
    if (!colgrp_size.valid) {
        LOGW << "Cannot get colgrp size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        colgrp->bounding_box(bb_min, bb_max);
        colgrp_size.min = bb_min;
        colgrp_size.max = bb_max;
        colgrp_size.width = bb_max.x - bb_min.x;
        colgrp_size.height = bb_max.y - bb_min.y;
        colgrp_size.valid = true;
    }
    
    double colgrp_width = colgrp_size.width;
    double colgrp_height = colgrp_size.height - 0.0135;
    
    LOGI << "Creating stacked column group with " << num_rows << " rows";
    LOGI << "  colgrp size: " << colgrp_width << " x " << colgrp_height;
    
    // 建立新的 Cell
    gdstk::Cell* stacked_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    stacked_cell->init(stacked_name);
    
    // 垂直堆疊 colgrp
    // 使用 repetition 來高效堆疊
    gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
    ref->init(colgrp);
    ref->origin = {0.0 - colgrp_size.min.x, 0.0 - colgrp_size.min.y};
    ref->magnification = 1.0;
    
    if (num_rows > 1) {
        ref->repetition.type = gdstk::RepetitionType::Rectangular;
        ref->repetition.columns = 1;
        ref->repetition.rows = num_rows;
        ref->repetition.spacing = {0.0, colgrp_height};  // 垂直方向間距為 colgrp 高度
    }
    
    stacked_cell->reference_array.append(ref);
    
    double total_height = num_rows * colgrp_height;
    
    // ========================================================================
    // 加入 VDD 和 VSS pins (M3 pin layer)
    // ========================================================================
    const OpenFinRAM::LayerDef* m3_pin_layer = layer_map.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
    
    if (m3_pin_layer != nullptr) {
        // VDD pin - x=0.125, y=0
        gdstk::Label* vdd_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vdd_label->init("VDD");
        vdd_label->origin = {0.125, 0.0};
        vdd_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
        stacked_cell->label_array.append(vdd_label);
        
        LOGI << "  Added VDD pin at (" << vdd_label->origin.x << ", " << vdd_label->origin.y 
             << ") on M3 pin layer";
        
        // VSS pin - x=0.162, y=0
        gdstk::Label* vss_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vss_label->init("VSS");
        vss_label->origin = {0.162, 0.0};
        vss_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
        stacked_cell->label_array.append(vss_label);
        
        LOGI << "  Added VSS pin at (" << vss_label->origin.x << ", " << vss_label->origin.y 
             << ") on M3 pin layer";
        
        // ====================================================================
        // 加入 WLT, WLB 和其他信號 pins - 只加最下面的 colgrp，位置跟 colgrp 一樣
        // ====================================================================
        LOGI << "  Adding WLT, WLB and signal pins from bottom colgrp";
        
        int wlt_count = 0;
        int wlb_count = 0;
        int signal_count = 0;
        
        // 遍歷 colgrp 的所有 labels，複製需要的 pins
        for (uint64_t i = 0; i < colgrp->label_array.count; ++i) {
            gdstk::Label* orig_label = colgrp->label_array[i];
            uint16_t layer = gdstk::get_layer(orig_label->tag);
            uint16_t datatype = gdstk::get_type(orig_label->tag);
            
            // 只處理 M3 pin layer (30, 251) 的 pins
            if (layer == 30 && datatype == 251) {
                const char* name = orig_label->text;
                bool should_copy = false;
                
                // 檢查是否為需要的信號
                if (strncmp(name, "WLT[", 4) == 0) {
                    should_copy = true;
                    wlt_count++;
                } else if (strncmp(name, "WLB[", 4) == 0) {
                    should_copy = true;
                    wlb_count++;
                } else if (strncmp(name, "yselt[", 6) == 0 ||
                           strncmp(name, "yseltn[", 7) == 0 ||
                           strncmp(name, "yselb[", 6) == 0 ||
                           strncmp(name, "yselbn[", 7) == 0 ||
                           strcasecmp(name, "blprechtn") == 0 ||
                           strcasecmp(name, "blprechbn") == 0 ||
                           strcasecmp(name, "wrena") == 0 ||
                        //    strcasecmp(name, "vsswrite") == 0 ||
                           strcasecmp(name, "sae") == 0 ||
                           strcasecmp(name, "saprechn") == 0 ||
                           strcasecmp(name, "wrenan") == 0) {
                    should_copy = true;
                    signal_count++;
                }
                
                if (should_copy) {
                    // 複製 label 到 stacked_cell，位置保持不變（最下面的 colgrp）
                    gdstk::Label* new_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                    new_label->init(name);
                    new_label->origin = orig_label->origin;  // 位置跟 colgrp 一樣
                    new_label->tag = orig_label->tag;
                    stacked_cell->label_array.append(new_label);
                }
            }
        }
        
        LOGI << "  Added " << wlt_count << " WLT pins, " << wlb_count << " WLB pins, and " 
             << signal_count << " signal pins on M3 pin layer";
        
        // ====================================================================
        // 加入 D[n-1:0] 和 Q[n-1:0] pins - 每個 colgrp 一個，形成 bus
        // ====================================================================
        LOGI << "  Adding D[" << (num_rows-1) << ":0] and Q[" << (num_rows-1) << ":0] pins";
        
        // 先找到 colgrp 中 D 和 Q 的原始位置
        double d_x = 0.0, d_y = 0.0;
        double q_x = 0.0, q_y = 0.0;
        bool found_d = false, found_q = false;
        
        for (uint64_t i = 0; i < colgrp->label_array.count; ++i) {
            gdstk::Label* orig_label = colgrp->label_array[i];
            uint16_t layer = gdstk::get_layer(orig_label->tag);
            uint16_t datatype = gdstk::get_type(orig_label->tag);
            
            if (layer == 30 && datatype == 251) {
                const char* name = orig_label->text;
                if (strcasecmp(name, "D") == 0) {
                    d_x = orig_label->origin.x;
                    d_y = orig_label->origin.y;
                    found_d = true;
                } else if (strcasecmp(name, "Q") == 0) {
                    q_x = orig_label->origin.x;
                    q_y = orig_label->origin.y;
                    found_q = true;
                }
            }
        }
        
        if (found_d && found_q) {
            // 為每個堆疊的 colgrp 加入 D[i] 和 Q[i]
            for (uint64_t row = 0; row < num_rows; ++row) {
                // 計算當前 row 的 y 偏移
                double y_offset = row * colgrp_height;
                
                // 加入 D[i]
                char d_name[32];
                snprintf(d_name, sizeof(d_name), "D[%lu]", (unsigned long)row);
                gdstk::Label* d_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                d_label->init(d_name);
                d_label->origin = {d_x, d_y + y_offset};
                d_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
                stacked_cell->label_array.append(d_label);
                
                // 加入 Q[i]
                char q_name[32];
                snprintf(q_name, sizeof(q_name), "Q[%lu]", (unsigned long)row);
                gdstk::Label* q_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                q_label->init(q_name);
                q_label->origin = {q_x, q_y + y_offset};
                q_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
                stacked_cell->label_array.append(q_label);
            }
            
            LOGI << "  Added " << num_rows << " D pins (D[0] to D[" << (num_rows-1) << "]) and "
                 << num_rows << " Q pins (Q[0] to Q[" << (num_rows-1) << "]) on M3 pin layer";
        } else {
            LOGW << "Cannot find D or Q pins in colgrp (found_d=" << found_d << ", found_q=" << found_q << ")";
        }
    } else {
        LOGW << "Cannot find M3 pin layer, skipping VDD/VSS pins";
    }
    
    // ========================================================================
    // 加入 BOUNDARY
    // ========================================================================
    gdstk::Vec2 boundary_min = {0.0, 0.0};
    gdstk::Vec2 boundary_max = {colgrp_width, total_height};
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map);
    stacked_cell->polygon_array.append(boundary);
    
    LOGI << "Created stacked column group '" << stacked_name << "'";
    LOGI << "  Total size: " << colgrp_width << " x " << total_height;
    
    return stacked_cell;
}

// ============================================================================
// 水平堆疊 stacked_colgrp cells (MUX 結構)
// 將多個 stacked_colgrp 沿著 X 軸（水平方向）並排，產生新的 cell
// ============================================================================
gdstk::Cell* create_muxed_colgrp(
    gdstk::Cell* stacked_colgrp,       // 已垂直堆疊完成的 colgrp cell
    uint64_t num_mux,                  // 水平方向堆疊數量
    const char* muxed_name,            // 新 cell 的名稱
    const OpenFinRAM::LayerMap& layer_map)
{
    if (stacked_colgrp == nullptr || num_mux == 0) {
        LOGE << "Invalid parameters for create_muxed_colgrp";
        return nullptr;
    }

    // 取得 stacked_colgrp 尺寸
    OpenFinRAM::CellSize stacked_size = OpenFinRAM::get_cell_size(stacked_colgrp, layer_map);

    if (!stacked_size.valid) {
        LOGW << "Cannot get stacked_colgrp size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        stacked_colgrp->bounding_box(bb_min, bb_max);
        stacked_size.min = bb_min;
        stacked_size.max = bb_max;
        stacked_size.width = bb_max.x - bb_min.x;
        stacked_size.height = bb_max.y - bb_min.y;
        stacked_size.valid = true;
    }

    double stacked_width = stacked_size.width;
    double stacked_height = stacked_size.height;

    LOGI << "Creating muxed column group with " << num_mux << " columns";
    LOGI << "  stacked_colgrp size: " << stacked_width << " x " << stacked_height;

    // 建立新的 Cell
    gdstk::Cell* muxed_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    muxed_cell->init(muxed_name);

    // 水平堆疊 stacked_colgrp
    gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
    ref->init(stacked_colgrp);
    ref->origin = {0.0 - stacked_size.min.x, 0.0 - stacked_size.min.y};
    ref->magnification = 1.0;

    if (num_mux > 1) {
        ref->repetition.type = gdstk::RepetitionType::Rectangular;
        ref->repetition.columns = num_mux;
        ref->repetition.rows = 1;
        ref->repetition.spacing = {stacked_width, 0.0};  // 水平方向間距為 stacked_colgrp 寬度
    }

    muxed_cell->reference_array.append(ref);

    double total_width = num_mux * stacked_width;

    // ========================================================================
    // 加入 BOUNDARY
    // ========================================================================
    gdstk::Vec2 boundary_min = {0.0, 0.0};
    gdstk::Vec2 boundary_max = {total_width, stacked_height};
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map);
    muxed_cell->polygon_array.append(boundary);

    LOGI << "Created muxed column group '" << muxed_name << "'";
    LOGI << "  Total size: " << total_width << " x " << stacked_height;

    return muxed_cell;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char **argv) {
    // Initialize plog
    static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
    plog::init(plog::debug, &consoleAppender);

    LOGI << "Starting OpenFinRAM application";

    // ========================================================================
    // Parse command line arguments
    // ========================================================================
    uint64_t test_num_bits = 2;        // Default: 2 bits
    uint64_t num_stacked_rows = 2;     // Default: 2 stacked rows
    uint64_t num_mux = 1;
    bool run_verification = true;      // Default: run verification (SPICE + SIS)
    
    if (argc >= 2) {
        test_num_bits = std::stoull(argv[1]);
        LOGI << "Using test_num_bits from command line: " << test_num_bits;
    } else {
        LOGI << "Using default test_num_bits: " << test_num_bits;
    }
    
    if (argc >= 3) {
        num_stacked_rows = std::stoull(argv[2]);
        LOGI << "Using num_stacked_rows from command line: " << num_stacked_rows;
    } else {
        LOGI << "Using default num_stacked_rows: " << num_stacked_rows;
    }

    if (argc >= 4) {
        num_mux = std::stoull(argv[3]);
        LOGI << "Using num_mux from command line: " << num_mux;
    } else {
        LOGI << "Using default num_mux: " << num_mux;
    }
    
    if (argc >= 5) {
        uint64_t verify_arg = std::stoull(argv[4]);
        run_verification = (verify_arg != 0);
        LOGI << "Using run_verification from command line: " << (run_verification ? "1 (enabled)" : "0 (disabled)");
    } else {
        LOGI << "Using default run_verification: 1 (enabled)";
    }
    
    LOGI << "Configuration: test_num_bits=" << test_num_bits 
         << ", num_stacked_rows=" << num_stacked_rows
         << ", num_mux=" << num_mux
         << ", run_verification=" << (run_verification ? 1 : 0);

    // ========================================================================
    // Initialize ASAP7 Layer Map (hardcoded)
    // ========================================================================
    LOGI << "Initializing ASAP7 layermap (hardcoded)...";
    g_layer_map.init_asap7_layermap();
    
    if (g_layer_map.empty()) {
        LOGE << "Failed to initialize layer map!";
        return 1;
    }    

    // ========================================================================
    // Read SRAM Filler Library (for left/right fillers)
    // ========================================================================
    std::string sram_filler_gds_str = join_path(get_current_dir_name(), "tech/gds/srambank_32b_boundary_2.gds");
    const char* sram_filler_gds = sram_filler_gds_str.c_str();
    const char* sram_filler_name = "FILLER_BLANK_6t122";
    
    LOGI << "Reading SRAM filler library: " << sram_filler_gds;
    gdstk::ErrorCode error_code;
    gdstk::Library sram_filler_lib = gdstk::read_gds(sram_filler_gds, 0, 1e-2, nullptr, &error_code);
    
    if (error_code != gdstk::ErrorCode::NoError) {
        LOGE << "Error reading SRAM filler GDS!";
        return 1;
    }
    
    // 取得 SRAM filler cell
    gdstk::Cell* sram_filler = sram_filler_lib.get_cell(sram_filler_name);
    
    if (sram_filler == nullptr) {
        LOGE << "Cannot find cell '" << sram_filler_name << "' in SRAM filler library!";
        sram_filler_lib.free_all();
        return 1;
    }
    
    LOGI << "Found SRAM filler cell: " << sram_filler_name;

    // ========================================================================
    // 取得 SRAM Cell (6T bitcell)
    // ========================================================================
    const char* sram_cell_name = "sram_cell_6t_122";
    gdstk::Cell* sram_cell = sram_filler_lib.get_cell(sram_cell_name);
    
    if (sram_cell == nullptr) {
        LOGE << "Cannot find cell '" << sram_cell_name << "' in SRAM library!";
        sram_filler_lib.free_all();
        return 1;
    }
    
    LOGI << "Found SRAM cell: " << sram_cell_name;
    
    // 取得 SRAM cell 尺寸
    OpenFinRAM::CellSize sram_cell_size = OpenFinRAM::get_cell_size(sram_cell, g_layer_map);
    
    if (!sram_cell_size.valid) {
        LOGW << "Cannot get SRAM cell size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        sram_cell->bounding_box(bb_min, bb_max);
        sram_cell_size.min = bb_min;
        sram_cell_size.max = bb_max;
        sram_cell_size.width = bb_max.x - bb_min.x;
        sram_cell_size.height = bb_max.y - bb_min.y;
        sram_cell_size.valid = true;
    }
    
    LOGI << "SRAM cell '" << sram_cell_name << "' size: " 
         << sram_cell_size.width << " x " << sram_cell_size.height;
    LOGI << "SRAM cell bounding box: (" << sram_cell_size.min.x << ", " << sram_cell_size.min.y 
         << ") to (" << sram_cell_size.max.x << ", " << sram_cell_size.max.y << ")";

    // ========================================================================
    // 列出 SRAM cell 中的 labels (查看 WL pin 位置)
    // ========================================================================
    LOGI << "=== Labels in SRAM cell '" << sram_cell_name << "' ===";
    for (uint64_t i = 0; i < sram_cell->label_array.count; ++i) {
        gdstk::Label* label = sram_cell->label_array[i];
        LOGI << "  Label: '" << (label->text ? label->text : "(null)") << "'"
             << " at (" << label->origin.x << ", " << label->origin.y << ")"
             << " layer=" << gdstk::get_layer(label->tag) 
             << " texttype=" << gdstk::get_type(label->tag);
    }
    
    // 列出特定層的 polygon (例如 M3 pin layer = 30, datatype 251 因為 WL 在 M3)
    LOGI << "=== Polygons on M3 pin layer in SRAM cell (WL related) ===";
    for (uint64_t i = 0; i < sram_cell->polygon_array.count; ++i) {
        gdstk::Polygon* poly = sram_cell->polygon_array[i];
        // 檢查是否為 M3 pin layer (layer 30, datatype 251)
        if (poly->tag == gdstk::make_tag(30, 251)) {
            gdstk::Vec2 bb_min, bb_max;
            poly->bounding_box(bb_min, bb_max);
            LOGI << "  M3 Pin polygon: "
                 << " bbox=(" << bb_min.x << "," << bb_min.y << ")-(" << bb_max.x << "," << bb_max.y << ")";
        }
    }
    
    // 列出 M3 drawing 層 (layer 30, datatype 0) 的 polygon
    LOGI << "=== All M3 polygons in SRAM cell (layer 30) ===";
    for (uint64_t i = 0; i < sram_cell->polygon_array.count; ++i) {
        gdstk::Polygon* poly = sram_cell->polygon_array[i];
        // 檢查是否為 M3 drawing layer (layer 30)
        if (gdstk::get_layer(poly->tag) == 30) {
            gdstk::Vec2 bb_min, bb_max;
            poly->bounding_box(bb_min, bb_max);
            double width = bb_max.x - bb_min.x;
            double height = bb_max.y - bb_min.y;
            LOGI << "  M3 polygon: datatype=" << gdstk::get_type(poly->tag)
                 << " bbox=(" << bb_min.x << "," << bb_min.y << ")-(" << bb_max.x << "," << bb_max.y << ")"
                 << " size=(" << width << "x" << height << ")";
        }
    }

    // ========================================================================
    // 取得 dummy_sram_6t122 和 tapcell_sram_6t122
    // ========================================================================
    const char* dummy_cell_name = "dummy_sram_6t122";
    const char* tapcell_name = "tapcell_sram_6t122";
    
    gdstk::Cell* dummy_cell = sram_filler_lib.get_cell(dummy_cell_name);
    if (dummy_cell == nullptr) {
        LOGW << "Cannot find cell '" << dummy_cell_name << "' in SRAM library!";
    } else {
        LOGI << "Found dummy cell: " << dummy_cell_name;
        
        // 列出 dummy_cell 的 labels（查看 BL, BLN, VDD, VSS pin 位置）
        LOGI << "=== Labels in dummy_sram_6t122 ===";
        for (uint64_t i = 0; i < dummy_cell->label_array.count; ++i) {
            gdstk::Label* label = dummy_cell->label_array[i];
            LOGI << "  Label: '" << (label->text ? label->text : "(null)") << "'"
                 << " at (" << label->origin.x << ", " << label->origin.y << ")"
                 << " layer=" << gdstk::get_layer(label->tag) 
                 << " texttype=" << gdstk::get_type(label->tag);
        }
    }
    
    gdstk::Cell* tapcell = sram_filler_lib.get_cell(tapcell_name);
    if (tapcell == nullptr) {
        LOGW << "Cannot find cell '" << tapcell_name << "' in SRAM library!";
    } else {
        LOGI << "Found tapcell: " << tapcell_name;
        
        // 列出 tapcell 的 labels（查看 VDD, VSS pin 位置）
        LOGI << "=== Labels in tapcell_sram_6t122 ===";
        for (uint64_t i = 0; i < tapcell->label_array.count; ++i) {
            gdstk::Label* label = tapcell->label_array[i];
            LOGI << "  Label: '" << (label->text ? label->text : "(null)") << "'"
                 << " at (" << label->origin.x << ", " << label->origin.y << ")"
                 << " layer=" << gdstk::get_layer(label->tag) 
                 << " texttype=" << gdstk::get_type(label->tag);
        }
    }

    // ========================================================================
    // 取得 dummy_topbot_v1 和 dummy_topbot_v2 (用於 SRAM array)
    // ========================================================================
    const char* dummy_topbot_v1_name = "dummy_topbot_v1";
    const char* dummy_topbot_v2_name = "dummy_topbot_v2";
    
    gdstk::Cell* dummy_topbot_v1 = sram_filler_lib.get_cell(dummy_topbot_v1_name);
    if (dummy_topbot_v1 == nullptr) {
        LOGW << "Cannot find cell '" << dummy_topbot_v1_name << "' in SRAM library!";
    } else {
        LOGI << "Found dummy_topbot_v1: " << dummy_topbot_v1_name;
    }
    
    gdstk::Cell* dummy_topbot_v2 = sram_filler_lib.get_cell(dummy_topbot_v2_name);
    if (dummy_topbot_v2 == nullptr) {
        LOGW << "Cannot find cell '" << dummy_topbot_v2_name << "' in SRAM library!";
    } else {
        LOGI << "Found dummy_topbot_v2: " << dummy_topbot_v2_name;
    }

    // ========================================================================
    // 測試: 建立 SRAM Column
    // ========================================================================
    gdstk::Cell* sram_column = create_sram_column(sram_cell, dummy_cell, tapcell, test_num_bits, g_layer_map);
    
    if (sram_column == nullptr) {
        LOGE << "Failed to create SRAM column!";
        sram_filler_lib.free_all();
        return 1;
    }
    
    // ========================================================================
    // 建立 SRAM Array (4 層堆疊)
    // ========================================================================
    const uint64_t num_array_rows = 4;  // 堆疊 4 層
    gdstk::Cell* sram_array = create_sram_array(sram_column, dummy_topbot_v1, dummy_topbot_v2, num_array_rows, g_layer_map);
    
    if (sram_array == nullptr) {
        LOGE << "Failed to create SRAM array!";
        sram_filler_lib.free_all();
        return 1;
    }

    // ========================================================================
    // 取得 FILLER_cgedge 和 iocolgrp_sram_6t122_v2 (用於 colgrp)
    // ========================================================================
    const char* filler_cgedge_name = "FILLER_cgedge";
    const char* io_colgrp_name = "iocolgrp_sram_6t122_v2";
    
    gdstk::Cell* filler_cgedge = sram_filler_lib.get_cell(filler_cgedge_name);
    if (filler_cgedge == nullptr) {
        LOGW << "Cannot find cell '" << filler_cgedge_name << "' in SRAM library!";
    } else {
        LOGI << "Found FILLER_cgedge: " << filler_cgedge_name;
    }
    
    gdstk::Cell* io_colgrp = sram_filler_lib.get_cell(io_colgrp_name);
    if (io_colgrp == nullptr) {
        LOGW << "Cannot find cell '" << io_colgrp_name << "' in SRAM library!";
    } else {
        LOGI << "Found io_colgrp: " << io_colgrp_name;
        
        // 列出 io_colgrp 中的所有 labels（用於找到信號位置）
        LOGI << "=== Labels in " << io_colgrp_name << " ===";
        for (uint64_t i = 0; i < io_colgrp->label_array.count; ++i) {
            gdstk::Label* label = io_colgrp->label_array[i];
            uint16_t layer = gdstk::get_layer(label->tag);
            uint16_t datatype = gdstk::get_type(label->tag);
            LOGI << "  Label: '" << label->text << "' at (" << label->origin.x << ", " 
                 << label->origin.y << ") layer=" << layer << " texttype=" << datatype;
        }
    }

    // ========================================================================
    // 建立 Column Group (colgrp_x{bit*2}x4)
    // ========================================================================
    gdstk::Cell* colgrp = nullptr;
    if (filler_cgedge != nullptr && io_colgrp != nullptr) {
        colgrp = create_colgrp(sram_array, filler_cgedge, io_colgrp, test_num_bits, g_layer_map);
        
        if (colgrp == nullptr) {
            LOGE << "Failed to create column group!";
        }
    } else {
        LOGW << "Skipping colgrp creation due to missing cells";
    }

    // ========================================================================
    // 建立 Stacked Column Group (垂直堆疊 colgrp)
    // ========================================================================
    gdstk::Cell* stacked_colgrp = nullptr;
    if (colgrp != nullptr) {
        char stacked_name[64];
        snprintf(stacked_name, sizeof(stacked_name), "stacked_colgrp_x%dx%lu", test_num_bits * 2, num_stacked_rows / 2);
        
        stacked_colgrp = create_stacked_colgrp(colgrp, num_stacked_rows / 2, stacked_name, g_layer_map);
        
        if (stacked_colgrp == nullptr) {
            LOGE << "Failed to create stacked column group!";
        }
    } else {
        LOGW << "Skipping stacked colgrp creation due to missing colgrp";
    }

    // ========================================================================
    // MUX: 水平並排 stacked_colgrp（即使 num_mux=1 也建立一致的 cell 名稱）
    // ========================================================================
    if (stacked_colgrp != nullptr && num_mux >= 1) {
        char muxed_name[96];
        snprintf(muxed_name, sizeof(muxed_name), "stacked_colgrp_x%dx%lux%lu", test_num_bits * 2, num_stacked_rows / 2, num_mux);

        gdstk::Cell* muxed_colgrp = create_muxed_colgrp(stacked_colgrp, num_mux, muxed_name, g_layer_map);
        if (muxed_colgrp == nullptr) {
            LOGE << "Failed to create muxed column group!";
        } else {
            stacked_colgrp = muxed_colgrp;
            LOGI << "Using muxed stacked_colgrp: " << stacked_colgrp->name;
        }
    }

    // 計算正確的 addr_width
    // WLT/WLB 各有 test_num_bits 條
    uint64_t num_wl = test_num_bits;  // 每個 WLT 或 WLB 的數量
    uint64_t wl_addr_bits = 0;
    uint64_t temp = num_wl;
    while (temp > 1) {
        wl_addr_bits++;
        temp >>= 1;
    }
    // MUX 選擇位數 (num_mux=1 -> 0 bits)
    uint64_t mux_addr_bits = 0;
    temp = num_mux;
    while (temp > 1) {
        mux_addr_bits++;
        temp >>= 1;
    }
    // 總地址位 = WL選擇位 + top/bottom位(1) + ysel位(2) + mux位
    uint64_t addr_width = wl_addr_bits + 1 + 2 + mux_addr_bits;
    
    LOGI << "Address width calculation:";
    LOGI << "  WLT/WLB count: " << num_wl;
    LOGI << "  WL address bits: " << wl_addr_bits;
    LOGI << "  Top/Bottom bit: 1";
    LOGI << "  YSel bits: 2";
    LOGI << "  MUX bits: " << mux_addr_bits;
    LOGI << "  Total addr_width: " << addr_width;

    // ========================================================================
    // 輸出 SRAM column, SRAM array 和 colgrp 到同一個 GDS 檔案
    // ========================================================================
    {
        gdstk::Library sram_lib = {};
        sram_lib.init("SRAM_LIB", sram_filler_lib.unit, sram_filler_lib.precision);
        
        // Helper lambda: 加入 cell 及其相依（如果尚未存在）
        auto add_cell_with_deps = [&sram_lib](gdstk::Cell* cell) {
            if (cell == nullptr) return;
            
            // 加入 cell 本身
            if (sram_lib.get_cell(cell->name) == nullptr) {
                gdstk::Cell* cell_copy = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
                cell_copy->copy_from(*cell, nullptr, true);
                sram_lib.cell_array.append(cell_copy);
            }
            
            // 加入相依 cells
            gdstk::Map<gdstk::Cell*> deps = {};
            cell->get_dependencies(true, deps);
            for (auto* item = deps.next(nullptr); item != nullptr; item = deps.next(item)) {
                if (sram_lib.get_cell(item->value->name) == nullptr) {
                    gdstk::Cell* dep_copy = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
                    dep_copy->copy_from(*item->value, nullptr, true);
                    sram_lib.cell_array.append(dep_copy);
                }
            }
            deps.clear();
        };
        
        // 加入 colgrp（頂層 cell，如果存在）
        if (colgrp != nullptr) {
            sram_lib.cell_array.append(colgrp);
        }
        
        // 加入 stacked_colgrp（如果存在）
        if (stacked_colgrp != nullptr) {
            add_cell_with_deps(stacked_colgrp);
        }
        
        // 加入 sram_array
        add_cell_with_deps(sram_array);
        
        // 加入 sram_column 及其相依
        add_cell_with_deps(sram_column);
        
        // 加入 sram_cell 及其相依
        add_cell_with_deps(sram_cell);
        
        // 加入 dummy_cell 及其相依
        add_cell_with_deps(dummy_cell);
        
        // 加入 tapcell 及其相依
        add_cell_with_deps(tapcell);
        
        // 加入 dummy_topbot_v1 及其相依
        add_cell_with_deps(dummy_topbot_v1);
        
        // 加入 dummy_topbot_v2 及其相依
        add_cell_with_deps(dummy_topbot_v2);
        
        // 加入 FILLER_cgedge 及其相依
        add_cell_with_deps(filler_cgedge);
        
        // 加入 io_colgrp 及其相依
        add_cell_with_deps(io_colgrp);
        
        const char* sram_output = "sram_array_test.gds";
        LOGI << "Writing SRAM library to: " << sram_output;
        LOGI << "  Contains: " << sram_column->name << ", " << sram_array->name;
        if (colgrp != nullptr) {
            LOGI << "  Contains: " << colgrp->name;
        }
        if (stacked_colgrp != nullptr) {
            LOGI << "  Contains: " << stacked_colgrp->name;
        }
        error_code = sram_lib.write_gds(sram_output, 0, nullptr);
        
        if (error_code != gdstk::ErrorCode::NoError) {
            LOGE << "Error writing SRAM GDS!";
        } else {
            LOGI << "Successfully created SRAM library: " << sram_output;
        }
    }

    // ========================================================================
    // 生成 SPICE netlist - 與 GDS 相同配置
    // ========================================================================
    LOGI << "=== Generating SPICE Netlist ===";
    
    // 使用與 GDS 相同的配置
    // num_array_rows = 4 (每個 array 有 test_num_bits * 4 個 WL)
    const uint64_t total_wordlines = test_num_bits;  // test_num_bits * num_array_rows * 4
    
    {
        OpenFinRAM::SpiceConfig config;
        config.num_wordlines = total_wordlines;
        config.num_colgrp = num_stacked_rows / 2;
        config.data_bits = num_stacked_rows / 2;
        config.num_mux = num_mux;
        config.output_dir = ".";
        
        OpenFinRAM::SpiceGenerator gen(config);
        gen.generate("sram_colgrp.sp");
    }

    // ========================================================================
    // PEX 策略選擇: 統計預測 vs 實際執行
    // ========================================================================
    // 設定為 true 則執行實際 PEX（耗時但精確）
    // 設定為 false 則使用統計回歸預測（快速但基於歷史數據）
    bool run_actual_pex = false;
    bool pex_success = false;
    
    if (run_actual_pex) {
        LOGI << "";
        LOGI << "========================================================================";
        LOGI << "Starting PEX (Parasitic Extraction) - Actual Run";
        LOGI << "========================================================================";
        
        // 配置 PEX
        OpenFinRAM::PEXRunner::Config pex_config;
        pex_config.gds_path = "../sram_array_test.gds";
        pex_config.spice_path = "../sram_colgrp.sp";
        
        // Cell 名稱: stacked_colgrp_x{num_bits*2}x{num_stacked_rows}
        char pex_cell_name[128];
        snprintf(pex_cell_name, sizeof(pex_cell_name), 
                 "stacked_colgrp_x%lux%lu", 
                 (unsigned long)(test_num_bits * 2), 
                 (unsigned long)num_stacked_rows / 2);
        pex_config.cell_name = pex_cell_name;
        pex_config.output_dir = ".";  // 在當前目錄下創建 run_{cell_name}
        pex_config.turbo_count = 8;   // 多線程加速
        
        // 創建 PEX runner 並執行
        OpenFinRAM::PEXRunner pex_runner(pex_config);
        
        LOGI << "PEX Configuration:";
        LOGI << "  Cell: " << pex_config.cell_name;
        LOGI << "  GDS: " << pex_config.gds_path;
        LOGI << "  SPICE: " << pex_config.spice_path;
        LOGI << "  Output Dir: " << pex_config.output_dir;
        
        pex_success = pex_runner.run();
        
        if (pex_success) {
            LOGI << "";
            LOGI << "========================================================================";
            LOGI << "PEX completed successfully!";
            LOGI << "PEX Netlist: " << pex_runner.get_pex_netlist_path();
            LOGI << "========================================================================";
        } else {
            LOGE << "";
            LOGE << "========================================================================";
            LOGE << "PEX failed! Check log files for details.";
            LOGE << "========================================================================";
        }
    } else {
        LOGI << "";
        LOGI << "========================================================================";
        LOGI << "Skipping actual PEX - Using statistical prediction";
        LOGI << "========================================================================";
        LOGI << "PEX is time-consuming. Using regression-based capacitance estimation.";
        LOGI << "To run actual PEX, set run_actual_pex = true in main.cpp";
        pex_success = false;  // 標記為不使用 PEX 結果
    }

    // ========================================================================
    // 開始合成流程 (Design Compiler synthesis with parameterized design)
    // ========================================================================
    uint64_t base_delay_cnt = 1;             // 其他 buffer 的初始數量
    uint64_t delay_prech_cnt = 1;             // precharge buffer 初始數量 (約 1/10)
    uint64_t attempt = 0;

    // 使用二分搜尋來找最小可行的 buffer 數
    const uint64_t min_buffer_cnt = 1;
    const uint64_t max_buffer_cnt = 1;
    uint64_t low_buffer = min_buffer_cnt;
    uint64_t high_buffer = max_buffer_cnt;
    uint64_t best_pass_buffer = 0;
    bool force_best_run = false;

    // 選擇測試模式：
    // 1. Quick mode: 測試 4 個代表性地址
    // 2. Parallel mode: 為每個 address bit 生成獨立的測試（可並行執行）
    // 3. Random mode: 隨機選擇指定百分比的地址進行測試
    // 4. Full mode: 測試所有地址（時間較長）
    bool use_parallel_mode = false;  // 設為 true 使用並行測試
    bool use_random_mode = true;     // 設為 true 使用隨機測試
    double random_test_percentage = 10.0;  // 測試 10% 的記憶體位置
    uint64_t random_seed = 0;        // 0 = 自動生成隨機種子，非0 = 指定種子以重現結果

    bool verification_passed = false;
    bool sis_passed = false;
    if (!run_verification) {
        LOGW << "Verification and SiliconSmart are disabled (run_verification=0).";
    }
    while (!sis_passed) {
        if (run_verification) {
            if (force_best_run) {
                base_delay_cnt = best_pass_buffer;
            } else if (low_buffer > high_buffer) {
                if (best_pass_buffer > 0) {
                    force_best_run = true;
                    base_delay_cnt = best_pass_buffer;
                    LOGI << "Bisection complete. Re-running with best buffer count = " << best_pass_buffer;
                } else {
                    LOGE << "Bisection failed: no passing buffer count in [" << min_buffer_cnt << ", " << max_buffer_cnt << "]";
                    return 1;
                }
            } else {
                base_delay_cnt = low_buffer + (high_buffer - low_buffer) / 2;
            }
        } else {
            base_delay_cnt = 10;
        }

        if (attempt > 3) {
            LOGE << "Exceeded maximum synthesis attempts (3). Exiting.";
            return 1;
        }

        attempt++; 
        verification_passed = false;

        // delay_prech_cnt = std::max<uint64_t>(1, base_delay_cnt / 10);  // 10 倍差距


        LOGI << "\n========================================";
        LOGI << "Starting Design Compiler Synthesis";
        LOGI << "Attempt: " << attempt;
        LOGI << "  delay_prech_cnt = " << delay_prech_cnt;
        LOGI << "  delay_wl_cnt    = " << base_delay_cnt;
        LOGI << "  delay_sense_cnt = " << base_delay_cnt;
        LOGI << "  delay_write_cnt = " << base_delay_cnt;
        if (run_verification) {
            LOGI << "  bisection range = [" << low_buffer << ", " << high_buffer << "]";
            LOGI << "  bisection mid   = " << base_delay_cnt;
        }
        LOGI << "========================================\n";

        // 建立合成配置
        SynthesisConfig synth_config(
            addr_width,                     // ADDR_WIDTH (動態計算)
            test_num_bits,                  // NUM_WLT (adjusted based on test_num_bits)
            num_mux,                        // MUX_RATIO
            delay_prech_cnt,                // delay_prech_cnt
            base_delay_cnt,                 // delay_wl_cnt
            base_delay_cnt,                 // delay_sense_cnt
            base_delay_cnt                  // delay_write_cnt
        );

        // 設定路徑
        synth_config.verilog_path = join_path(get_current_dir_name(), "tech/verilog");
        synth_config.syn_path = join_path(get_current_dir_name(), "tmp/verilog");
        synth_config.output_path = join_path(get_current_dir_name(), "tmp/verilog");

        // 建立合成管理器
        SynthesisManager synth_manager(synth_config);

        // ========================================================================
        // 載入電容值: PEX vs 統計預測
        // ========================================================================
        if (pex_success) {
            // 方案 A: 從實際 PEX 結果載入電容值（最精確）
            std::string rep_file = "pex/rep.txt";

            LOGI << "\n========================================";
            LOGI << "Loading Capacitance from PEX Results";
            LOGI << "========================================\n";
            LOGI << "Rep file: " << rep_file;

            if (synth_manager.load_capacitance_from_pex(rep_file)) {
                LOGI << "Successfully loaded capacitance data from PEX";
                LOGI << "Synthesis will use PEX-extracted load values";
            } else {
                LOGW << "Failed to load PEX capacitance data";
                LOGW << "Falling back to statistical prediction...";

                // PEX 載入失敗，改用統計預測
                if (synth_manager.predict_capacitance(test_num_bits * 2, num_stacked_rows)) {
                    LOGI << "Successfully predicted capacitance using regression model";
                } else {
                    LOGW << "Failed to predict capacitance, synthesis will use default values";
                }
            }
        } else {
            // 方案 B: 使用統計回歸模型預測電容值（快速）
            LOGI << "\n========================================";
            LOGI << "Using Statistical Capacitance Prediction";
            LOGI << "========================================\n";

            // 參數說明:
            // - bit_num: test_num_bits * 2 (因為有 2 bits per column)
            // - stacked: num_stacked_rows (垂直堆疊層數)
            int bit_num = test_num_bits * 2;
            int stacked = num_stacked_rows;

            LOGI << "Configuration for prediction:";
            LOGI << "  bit_num = " << bit_num << " (test_num_bits=" << test_num_bits << " * 2)";
            LOGI << "  stacked = " << stacked;

            if (synth_manager.predict_capacitance(bit_num, stacked)) {
                LOGI << "\n✓ Successfully predicted capacitance using regression model";
                LOGI << "  Model accuracy: R² > 0.87 for all signals";
                LOGI << "  Synthesis will use predicted load values";
                LOGI << "\nNote: For most accurate results, consider running actual PEX";
                LOGI << "      and updating the regression model with new data points.";
            } else {
                LOGW << "Failed to predict capacitance";
                LOGW << "Synthesis will proceed with default load values";
            }
        }

        // 執行合成
        LOGI << "\n========================================";
        LOGI << "Running Design Compiler Synthesis";
        LOGI << "========================================\n";

        if (!synth_manager.run_synthesis()) {
            LOGW << "Synthesis flow completed with warnings/errors. Check logs for details.";
        } else {
            LOGI << "Synthesis flow completed successfully!";
        }

        // ========================================================================
        // 開始 Verilog 到 SPICE 轉換流程
        // ========================================================================
        LOGI << "\n========================================";
        LOGI << "Starting Verilog to SPICE Conversion";
        LOGI << "========================================\n";

        // 建立轉換配置
        SpiceConversionConfig conv_config(
            join_path(get_current_dir_name(), "tmp/verilog"),
            join_path(get_current_dir_name(), "tmp/verilog")
        );

        conv_config.netlist_v = join_path(get_current_dir_name(), "tmp/verilog/netlist.v");
        conv_config.netlist_sp = join_path(get_current_dir_name(), "tmp/verilog/netlist.sp");
        conv_config.cdl_file = join_path(get_current_dir_name(), "tech/cdl/asap7sc7p5t_28_R.cdl");

        // 建立轉換器並執行
        SpiceConverter converter(conv_config);
        if (!converter.convert_to_spice()) {
            LOGW << "SPICE conversion flow completed with warnings/errors. Check logs for details.";
        } else {
            LOGI << "SPICE conversion flow completed successfully!";
        }

        // ========================================================================
        // 產生 Innovus TCL Script（用於 control logic P&R）
        // ========================================================================
        if (stacked_colgrp != nullptr) {
            LOGI << "";
            LOGI << "========================================================================";
            LOGI << "Generating Innovus TCL Script for Control Logic P&R";
            LOGI << "========================================================================";
            
            // 取得 stacked_colgrp 寬度
            OpenFinRAM::CellSize stacked_size = OpenFinRAM::get_cell_size(stacked_colgrp, g_layer_map);
            
            if (!stacked_size.valid) {
                LOGW << "Cannot get stacked_colgrp size from BOUNDARY, using bounding box";
                gdstk::Vec2 bb_min, bb_max;
                stacked_colgrp->bounding_box(bb_min, bb_max);
                stacked_size.min = bb_min;
                stacked_size.max = bb_max;
                stacked_size.width = bb_max.x - bb_min.x;
                stacked_size.height = bb_max.y - bb_min.y;
                stacked_size.valid = true;
            }
            
            double sram_width = stacked_size.width - 0.054 * 2;  // 減去兩側 54 nm 的 margin
            LOGI << "SRAM (stacked_colgrp) width (w. margin): " << sram_width << " um";
            LOGI << "SRAM (stacked_colgrp) height: " << stacked_size.height << " um";
            
            // 建立 Innovus TCL Generator
            OpenFinRAM::InnovusTclGenerator tcl_gen;
            
            tcl_gen.set_design_name("ctrl_decode");
            tcl_gen.set_site_name("asap7sc7p5t");
            tcl_gen.set_site_height(0.27);
            tcl_gen.set_cpu_count(8, 0);
            
            // 解析 QoR report（如果存在）
            std::string qor_file = join_path(get_current_dir_name(), "tmp/verilog/qor_report.txt");
            bool qor_parsed = tcl_gen.parse_qor_report(qor_file);
            
            if (qor_parsed) {
                // 產生 run.tcl（高度自動計算）
                std::string output_tcl = join_path(get_current_dir_name(), "tmp/innovus/ctrl_run.tcl");
                
                // ysel 有 4 根（yselt[0-3], yseltn[0-3], yselb[0-3], yselbn[0-3]）
                int num_ysel = 4;
                
                if (tcl_gen.generate_run_tcl(sram_width, 0.0, output_tcl, 
                                            test_num_bits, test_num_bits, num_ysel, addr_width, num_mux)) {
                    LOGI << "";
                    LOGI << "✓ Successfully generated Innovus TCL script: " << output_tcl;
                    LOGI << "  Design: ctrl_decode";
                    LOGI << "  Floorplan width: " << sram_width << " um (matches stacked_colgrp)";
                    
                    const OpenFinRAM::QoRReport& qor = tcl_gen.get_qor_report();
                    double calculated_height = tcl_gen.calculate_floorplan_height(sram_width);
                    LOGI << "  Floorplan height: " << calculated_height << " um (auto-calculated)";
                    LOGI << "  Cell area: " << qor.cell_area << " um^2";
                    LOGI << "  Utilization: " << (qor.cell_area / (sram_width * calculated_height) * 100.0) << " %";
                    LOGI << "";
                    
                    // 執行 Innovus
                    LOGI << "========================================================================";
                    LOGI << "Running Innovus for Place & Route...";
                    LOGI << "========================================================================";
                    
                    std::string work_dir = join_path(get_current_dir_name(), "tmp/innovus");
                    std::string log_file = "ctrl_decode_innovus.log";
                    
                    if (tcl_gen.run_innovus(output_tcl, work_dir, log_file)) {
                        LOGI << "";
                        LOGI << "✓ Innovus execution completed successfully";
                        LOGI << "  Check output files in: " << work_dir;
                        LOGI << "  Log file: " << work_dir << "/" << log_file;
                        LOGI << "";
                        
                        // 執行 v2lvs 將 Verilog netlist 轉換為 SPICE
                        LOGI << "========================================================================";
                        LOGI << "Running v2lvs for Verilog to SPICE conversion...";
                        LOGI << "========================================================================";
                        
                        if (tcl_gen.run_v2lvs(work_dir)) {
                            LOGI << "";
                            LOGI << "✓ v2lvs execution completed successfully";
                            LOGI << "  Generated SPICE netlist: " << work_dir << "/netlist_for_lvs.sp";
                            LOGI << "";
                            
                            // 執行 netlist 後處理
                            LOGI << "========================================================================";
                            LOGI << "Post-processing SPICE netlist...";
                            LOGI << "========================================================================";
                            
                            std::string cdl_file = join_path(get_current_dir_name(), "tech/cdl/asap7sc7p5t_28_R.cdl");
                            
                            if (tcl_gen.post_process_netlist(work_dir, "netlist_for_lvs.sp", cdl_file)) {
                                LOGI << "";
                                LOGI << "✓ Netlist post-processing completed successfully";
                                LOGI << "  Processed netlist: " << work_dir << "/netlist_for_lvs.sp";
                                LOGI << "  - Merged continuation lines";
                                LOGI << "  - Added VDD VSS to SUBCKT definitions";
                                LOGI << "  - Expanded $PINS format";
                                LOGI << "  - Processed .CONNECT directives";
                                LOGI << "";
                            } else {
                                LOGW << "Netlist post-processing failed";
                                LOGW << "The netlist may not be properly formatted for LVS";
                            }
                        } else {
                            LOGW << "v2lvs execution failed";
                            LOGW << "Please check if netlist_for_lvs.v exists in: " << work_dir;
                        }
                    } else {
                        LOGW << "Innovus execution failed";
                        LOGW << "Please check log file: " << work_dir << "/" << log_file;
                    }
                } else {
                    LOGW << "Failed to generate Innovus TCL script";
                }
            } else {
                LOGW << "QoR report not found or invalid: " << qor_file;
                LOGW << "Skipping Innovus TCL generation";
                LOGW << "Please run synthesis first to generate QoR report";
            }
            
            LOGI << "========================================================================";
            LOGI << "";
        } else {
            LOGW << "Stacked colgrp not created, skipping Innovus TCL generation";
        }

        // ========================================================================
        // 開始 SRAM 集成流程
        // ========================================================================
        LOGI << "\n========================================";
        LOGI << "Starting SRAM Integration";
        LOGI << "========================================\n";

        // 建立集成配置
        SramIntegrationConfig integ_config(
            addr_width,                              // addr_width
            num_stacked_rows,                 // data_width
            test_num_bits,          // num_wordlines
            num_mux                                 // mux_ratio
        );

        integ_config.ctrl_netlist = join_path(get_current_dir_name(), "tmp/innovus/netlist_for_lvs.sp");
        // integ_config.ctrl_netlist = "./verilog/netlist.sp";
        integ_config.datapath_netlist = "./sram_colgrp.sp";
        integ_config.output_netlist = "./sram.sp";

        // 建立集成器並執行
        SpiceIntegrator integrator(integ_config);
        std::string integrated_sram = integrator.integrate_sram("sram.sp");

        if (integrated_sram.empty()) {
            LOGW << "SRAM integration completed with warnings/errors. Check logs for details.";
        } else {
            LOGI << "SRAM integration completed successfully!";
            LOGI << "Generated integrated SRAM netlist: " << integrated_sram;
        }

        // ========================================================================
        // 開始 SPICE Simulation 驗證流程
        // ========================================================================
        verification_passed = run_spice_simulation_verification(
            run_verification,
            use_random_mode,
            use_parallel_mode,
            random_test_percentage,
            random_seed,
            addr_width,
            num_stacked_rows,
            num_wl);

        if (run_verification && !verification_passed) {
            if (!update_bisection_on_failure(
                    base_delay_cnt,
                    max_buffer_cnt,
                    best_pass_buffer,
                    low_buffer,
                    high_buffer,
                    force_best_run,
                    "verification failed")) {
                return 1;
            }
            continue;
        }

        if (!run_verification) {
            LOGW << "Skipping SiliconSmart (run_verification=0).";
            sis_passed = true;
            break;
        }

        bool sis_ok = run_siliconsmart_and_check(attempt, test_num_bits, num_stacked_rows, addr_width);
        if (!sis_ok) {
            LOGW << "SiliconSmart reported errors (found 'Error:   Task' in log).";
            if (!update_bisection_on_failure(
                    base_delay_cnt,
                    max_buffer_cnt,
                    best_pass_buffer,
                    low_buffer,
                    high_buffer,
                    force_best_run,
                    "SiliconSmart failed")) {
                return 1;
            }
            continue;
        }

        LOGI << "SiliconSmart log check passed (no 'Error:   Task').";
        best_pass_buffer = base_delay_cnt;
        if (force_best_run) {
            LOGI << "Bisection complete: minimal passing buffer count = " << best_pass_buffer;
            sis_passed = true;
            break;
        }
        if (base_delay_cnt == 0) {
            high_buffer = 0;
        } else {
            high_buffer = base_delay_cnt - 1;
        }
        LOGI << "Bisection update: pass, new range = [" << low_buffer << ", " << high_buffer << "]";

        if (low_buffer > high_buffer) {
            LOGI << "Bisection complete: minimal passing buffer count = " << best_pass_buffer;
            sis_passed = true;
            break;
        }

        continue;
    }

    // ========================================================================
    // 讀取 Innovus 產生的 GDS 檔案並添加 Gate polygons
    // ========================================================================
    LOGI << "========================================================================";
    LOGI << "Reading Innovus generated GDS file and adding Gate polygons";
    LOGI << "========================================================================";
    
    std::string gds_path = join_path(get_current_dir_name(), "tmp/innovus/ctrl_decode.gds");
    LOGI << "Reading GDS file: " << gds_path;
    
    // 讀取 GDS 檔案
    gdstk::ErrorCode gds_error_code = gdstk::ErrorCode::NoError;
    gdstk::Library gds_lib = gdstk::read_gds(gds_path.c_str(), 0, 1e-2, nullptr, &gds_error_code);
    
    if (gds_error_code == gdstk::ErrorCode::NoError && gds_lib.cell_array.count > 0) {
        LOGI << "Successfully read GDS file";
        LOGI << "Number of cells in library: " << gds_lib.cell_array.count;
        
        // 取得 ctrl_decode cell
        gdstk::Cell* ctrl_decode_cell = gds_lib.get_cell("ctrl_decode");
        
        if (ctrl_decode_cell == nullptr) {
            LOGW << "ctrl_decode cell not found, using first cell";
            ctrl_decode_cell = gds_lib.cell_array[0];
        }
        
        LOGI << "Working with cell: " << ctrl_decode_cell->name;
        
        // 取得 BOUNDARY layer 的尺寸
        OpenFinRAM::CellSize cell_size = OpenFinRAM::get_cell_size(ctrl_decode_cell, g_layer_map);
        
        if (!cell_size.valid) {
            LOGW << "Cannot get cell size from BOUNDARY, using bounding box";
            gdstk::Vec2 bb_min, bb_max;
            ctrl_decode_cell->bounding_box(bb_min, bb_max);
            cell_size.min = bb_min;
            cell_size.max = bb_max;
            cell_size.width = bb_max.x - bb_min.x;
            cell_size.height = bb_max.y - bb_min.y;
            cell_size.valid = true;
        }
        
        LOGI << "========================================================================";
        LOGI << "Core Boundary Information:";
        LOGI << "  Lower-left corner (min):  (" << cell_size.min.x << ", " << cell_size.min.y << ")";
        LOGI << "  Upper-right corner (max): (" << cell_size.max.x << ", " << cell_size.max.y << ")";
        LOGI << "  Width:  " << cell_size.width;
        LOGI << "  Height: " << cell_size.height;
        LOGI << "========================================================================";
        
        // ================================================================
        // 創建帶有 Gate polygons 的新 cell
        // ================================================================
        LOGI << "========================================================================";
        LOGI << "Creating parameterized Gate polygons on left and right sides";
        LOGI << "========================================================================";
        
        // 取得 Gate layer 的定義
        const OpenFinRAM::LayerDef* gate_layer = g_layer_map.get_layer("Gate", OpenFinRAM::LayerPurpose::Drawing);
        
        if (gate_layer == nullptr) {
            LOGW << "Cannot find Gate drawing layer definition, skipping Gate polygon creation";
        } else {
            LOGI << "Found Gate layer: layer=" << gate_layer->layer_number 
                 << ", datatype=" << gate_layer->datatype;
            
            // 創建新的 ctrl_decode_with_filler cell
            const char* new_cell_name = "ctrl_decode_with_filler";
            
            // 先檢查是否已存在
            if (gds_lib.get_cell(new_cell_name) != nullptr) {
                LOGW << "Cell '" << new_cell_name << "' already exists, removing it first";
                // 移除舊的 cell
                for (uint64_t i = 0; i < gds_lib.cell_array.count; i++) {
                    if (strcmp(gds_lib.cell_array[i]->name, new_cell_name) == 0) {
                        gds_lib.cell_array[i]->free_all();
                        gds_lib.cell_array.remove(i);
                        break;
                    }
                }
            }
            
            LOGI << "Creating new cell: " << new_cell_name;
            gdstk::Cell* gate_wrapper_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
            gate_wrapper_cell->init(new_cell_name);
            
            // 在新 cell 中添加對原始 ctrl_decode 的 reference
            LOGI << "Adding reference to original ctrl_decode cell...";
            gdstk::Reference* ctrl_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ctrl_ref->init(ctrl_decode_cell);
            ctrl_ref->origin = {0.0, 0.0};
            ctrl_ref->magnification = 1.0;
            gate_wrapper_cell->reference_array.append(ctrl_ref);
            
            // Gate polygon 參數
            const double gate_spacing = 0.017;  // 與 ctrl_decoder 的間距
            const double gate_width = 0.020;    // Gate 的寬度（可調整）
            
            LOGI << "Gate polygon parameters:";
            LOGI << "  Spacing from core: " << gate_spacing << " um";
            LOGI << "  Gate width: " << gate_width << " um";
            LOGI << "  Gate height: " << cell_size.height << " um (matches core height)";
            
            // 創建左側 Gate polygon
            LOGI << "Creating left Gate polygon...";
            gdstk::Polygon* left_gate = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
            
            // 左側 Gate 的四個頂點（矩形）
            // x 坐標: cell_size.min.x - gate_spacing - gate_width 到 cell_size.min.x - gate_spacing
            // y 坐標: cell_size.min.y 到 cell_size.max.y
            gdstk::Vec2 left_points[4] = {
                {cell_size.min.x - gate_spacing - gate_width, cell_size.min.y},
                {cell_size.min.x - gate_spacing, cell_size.min.y},
                {cell_size.min.x - gate_spacing, cell_size.max.y},
                {cell_size.min.x - gate_spacing - gate_width, cell_size.max.y}
            };
            
            left_gate->point_array.extend({.capacity = 0, .count = 4, .items = left_points});
            left_gate->tag = gate_layer->tag();
            gate_wrapper_cell->polygon_array.append(left_gate);
            
            LOGI << "  Left gate position: x=[" 
                 << (cell_size.min.x - gate_spacing - gate_width) << ", " 
                 << (cell_size.min.x - gate_spacing) << "]";
            
            // 創建右側 Gate polygon
            LOGI << "Creating right Gate polygon...";
            gdstk::Polygon* right_gate = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
            
            // 右側 Gate 的四個頂點（矩形）
            // x 坐標: cell_size.max.x + gate_spacing 到 cell_size.max.x + gate_spacing + gate_width
            // y 坐標: cell_size.min.y 到 cell_size.max.y
            gdstk::Vec2 right_points[4] = {
                {cell_size.max.x + gate_spacing, cell_size.min.y},
                {cell_size.max.x + gate_spacing + gate_width, cell_size.min.y},
                {cell_size.max.x + gate_spacing + gate_width, cell_size.max.y},
                {cell_size.max.x + gate_spacing, cell_size.max.y}
            };
            
            right_gate->point_array.extend({.capacity = 0, .count = 4, .items = right_points});
            right_gate->tag = gate_layer->tag();
            gate_wrapper_cell->polygon_array.append(right_gate);
            
            LOGI << "  Right gate position: x=[" 
                 << (cell_size.max.x + gate_spacing) << ", " 
                 << (cell_size.max.x + gate_spacing + gate_width) << "]";
            
            // ================================================================
            // 創建 Fin polygons
            // ================================================================
            LOGI << "========================================================================";
            LOGI << "Adding Fin polygons on left and right sides";
            LOGI << "========================================================================";
            
            // 取得 fin layer 的定義
            const OpenFinRAM::LayerDef* fin_layer = g_layer_map.get_layer("fin", OpenFinRAM::LayerPurpose::Drawing);
            
            if (fin_layer == nullptr) {
                LOGW << "Cannot find fin drawing layer definition, skipping Fin polygon creation";
            } else {
                LOGI << "Found fin layer: layer=" << fin_layer->layer_number 
                     << ", datatype=" << fin_layer->datatype;
                
                // Fin polygon 參數
                const double fin_start_y = 0.010;        // 從 y=0.10 開始
                const double fin_spacing = 0.027;        // 兩根 fin 的間距
                const double fin_height = 0.007;        // fin 的高度
                const double fin_width = 0.054;         // fin 的寬度
                
                // 計算需要多少根 fin
                double available_height = cell_size.max.y - fin_start_y;
                uint64_t num_fins = (uint64_t)std::floor(available_height / fin_spacing) + 1;
                
                LOGI << "Fin polygon parameters:";
                LOGI << "  Start Y position: " << fin_start_y << " um";
                LOGI << "  Fin spacing: " << fin_spacing << " um";
                LOGI << "  Fin height: " << fin_height << " um";
                LOGI << "  Fin width: " << fin_width << " um";
                LOGI << "  Number of fins: " << num_fins;
                
                // 創建左側的所有 fin polygons
                LOGI << "Creating left fin polygons...";
                for (uint64_t i = 0; i < num_fins; i++) {
                    double fin_y_start = fin_start_y + i * fin_spacing;
                    double fin_y_end = fin_y_start + fin_height;
                    
                    // 確保不超過 cell 的上邊界
                    if (fin_y_end > cell_size.max.y) {
                        break;
                    }
                    
                    gdstk::Polygon* left_fin = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                    
                    // 左側 fin 的四個頂點（矩形）
                    // x 坐標: 緊貼 ctrl_decode 左側，寬度 0.054
                    // y 坐標: fin_y_start 到 fin_y_end
                    gdstk::Vec2 left_fin_points[4] = {
                        {cell_size.min.x - fin_width, fin_y_start},
                        {cell_size.min.x, fin_y_start},
                        {cell_size.min.x, fin_y_end},
                        {cell_size.min.x - fin_width, fin_y_end}
                    };
                    
                    left_fin->point_array.extend({.capacity = 0, .count = 4, .items = left_fin_points});
                    left_fin->tag = fin_layer->tag();
                    gate_wrapper_cell->polygon_array.append(left_fin);
                }
                
                LOGI << "  Created " << num_fins << " left fin polygons";
                
                // 創建右側的所有 fin polygons
                LOGI << "Creating right fin polygons...";
                for (uint64_t i = 0; i < num_fins; i++) {
                    double fin_y_start = fin_start_y + i * fin_spacing;
                    double fin_y_end = fin_y_start + fin_height;
                    
                    // 確保不超過 cell 的上邊界
                    if (fin_y_end > cell_size.max.y) {
                        break;
                    }
                    
                    gdstk::Polygon* right_fin = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                    
                    // 右側 fin 的四個頂點（矩形）
                    // x 坐標: 緊貼 ctrl_decode 右側，寬度 0.054
                    // y 坐標: fin_y_start 到 fin_y_end
                    gdstk::Vec2 right_fin_points[4] = {
                        {cell_size.max.x, fin_y_start},
                        {cell_size.max.x + fin_width, fin_y_start},
                        {cell_size.max.x + fin_width, fin_y_end},
                        {cell_size.max.x, fin_y_end}
                    };
                    
                    right_fin->point_array.extend({.capacity = 0, .count = 4, .items = right_fin_points});
                    right_fin->tag = fin_layer->tag();
                    gate_wrapper_cell->polygon_array.append(right_fin);
                }
                
                LOGI << "  Created " << num_fins << " right fin polygons";
                LOGI << "Fin polygons created successfully";
            }
            
            // ================================================================
            // 創建上下兩行的 Gate polygons (filler)
            // ================================================================
            LOGI << "========================================================================";
            LOGI << "Adding Gate filler rows on top and bottom";
            LOGI << "========================================================================";
            
            // Gate filler 參數
            const double gate_filler_height = 0.8;      // Gate 的高度
            const double gate_filler_width = 0.02;      // Gate 的寬度
            const double gate_filler_spacing = 0.034;   // Gate 之間的間距
            
            // 從左側 gate 的最左邊開始，到右側 gate 的最右邊結束
            double row_start_x = cell_size.min.x - gate_spacing - gate_width;
            double row_end_x = cell_size.max.x + gate_spacing + gate_width;
            double row_width = row_end_x - row_start_x;
            
            // 計算需要多少個 gate
            double pitch = gate_filler_width + gate_filler_spacing;  // 一個 gate 的 pitch
            uint64_t num_gates = (uint64_t)std::ceil(row_width / pitch);
            
            LOGI << "Gate filler parameters:";
            LOGI << "  Gate height: " << gate_filler_height << " um";
            LOGI << "  Gate width: " << gate_filler_width << " um";
            LOGI << "  Gate spacing: " << gate_filler_spacing << " um";
            LOGI << "  Row start X: " << row_start_x << " um (left gate)";
            LOGI << "  Row end X: " << row_end_x << " um (right gate)";
            LOGI << "  Number of gates per row: " << num_gates;
            
            // 創建下方的 gate row
            LOGI << "Creating bottom gate row...";
            for (uint64_t i = 0; i < num_gates; i++) {
                double gate_x_start = row_start_x + i * pitch;
                double gate_x_end = gate_x_start + gate_filler_width;
                
                // 確保不超過右邊界
                if (gate_x_end > row_end_x) {
                    gate_x_end = row_end_x;
                    if (gate_x_end <= gate_x_start) {
                        break;
                    }
                }
                
                gdstk::Polygon* bottom_gate = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                
                // 下方 gate 的四個頂點（矩形）
                // x 坐標: gate_x_start 到 gate_x_end
                // y 坐標: 緊貼 ctrl_decode 下邊界，向下延伸 gate_filler_height
                gdstk::Vec2 bottom_gate_points[4] = {
                    {gate_x_start, cell_size.min.y - gate_filler_height},
                    {gate_x_end, cell_size.min.y - gate_filler_height},
                    {gate_x_end, cell_size.min.y + 0.02},
                    {gate_x_start, cell_size.min.y + 0.02}
                };
                
                bottom_gate->point_array.extend({.capacity = 0, .count = 4, .items = bottom_gate_points});
                bottom_gate->tag = gate_layer->tag();
                gate_wrapper_cell->polygon_array.append(bottom_gate);
            }
            
            LOGI << "  Created " << num_gates << " bottom gate polygons";
            
            // 創建上方的 gate row
            LOGI << "Creating top gate row...";
            for (uint64_t i = 0; i < num_gates; i++) {
                double gate_x_start = row_start_x + i * pitch;
                double gate_x_end = gate_x_start + gate_filler_width;
                
                // 確保不超過右邊界
                if (gate_x_end > row_end_x) {
                    gate_x_end = row_end_x;
                    if (gate_x_end <= gate_x_start) {
                        break;
                    }
                }
                
                gdstk::Polygon* top_gate = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                
                // 上方 gate 的四個頂點（矩形）
                // x 坐標: gate_x_start 到 gate_x_end
                // y 坐標: 緊貼 ctrl_decode 上邊界，向上延伸 gate_filler_height
                gdstk::Vec2 top_gate_points[4] = {
                    {gate_x_start, cell_size.max.y - 0.02},
                    {gate_x_end, cell_size.max.y - 0.02},
                    {gate_x_end, cell_size.max.y + gate_filler_height},
                    {gate_x_start, cell_size.max.y + gate_filler_height}
                };
                
                top_gate->point_array.extend({.capacity = 0, .count = 4, .items = top_gate_points});
                top_gate->tag = gate_layer->tag();
                gate_wrapper_cell->polygon_array.append(top_gate);
            }
            
            LOGI << "  Created " << num_gates << " top gate polygons";
            LOGI << "Gate filler rows created successfully";
            
            // ================================================================
            // 創建上下各兩根 Fin polygons
            // ================================================================
            LOGI << "========================================================================";
            LOGI << "Adding Fin rows on top and bottom";
            LOGI << "========================================================================";
            
            if (fin_layer == nullptr) {
                LOGW << "Cannot find fin drawing layer definition, skipping top/bottom Fin creation";
            } else {
                // Fin 參數
                const double fin_height = 0.007;        // fin 的高度
                const double fin_spacing = 0.020;        // 兩根 fin 的間距
                const double fin_row_width = cell_size.width + 0.054 * 2;  // ctrl_decode 寬度 + 左右各延伸 0.054
                const double fin_row_start_x = cell_size.min.x - 0.054;
                const double fin_row_end_x = cell_size.max.x + 0.054;
                
                LOGI << "Fin row parameters:";
                LOGI << "  Fin row width: " << fin_row_width << " um";
                LOGI << "  Fin row X range: [" << fin_row_start_x << ", " << fin_row_end_x << "]";
                LOGI << "  Fin height: " << fin_height << " um";
                LOGI << "  Fin spacing: " << fin_spacing << " um";

                // 創建下方的三根 fin
                LOGI << "Creating bottom fin rows...";
                // 從最下面的 fin 開始，向上布置，確保每根 fin 之間間距為 fin_spacing
                double base_y = cell_size.min.y - 3 * fin_spacing - 0.011;  // 最下面的 fin 起始位置
                for (int i = 0; i < 3; i++) {
                    double fin_y_start = base_y + i * (fin_height + fin_spacing);
                    double fin_y_end = fin_y_start + fin_height;
                    
                    gdstk::Polygon* bottom_fin = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                    
                    // 下方 fin 的四個頂點（矩形）
                    gdstk::Vec2 bottom_fin_points[4] = {
                        {fin_row_start_x, fin_y_start},
                        {fin_row_end_x, fin_y_start},
                        {fin_row_end_x, fin_y_end},
                        {fin_row_start_x, fin_y_end}
                    };
                    
                    bottom_fin->point_array.extend({.capacity = 0, .count = 4, .items = bottom_fin_points});
                    bottom_fin->tag = fin_layer->tag();
                    gate_wrapper_cell->polygon_array.append(bottom_fin);
                }
                
                LOGI << "  Created 3 bottom fin rows";
                
                // 創建上方的三根 fin
                LOGI << "Creating top fin rows...";
                double base_top_y = cell_size.max.y + 0.01;  // 最下面的 fin 起始位置
                for (int i = 0; i < 3; i++) {
                    double fin_y_start = base_top_y + i * (fin_height + fin_spacing);
                    double fin_y_end = fin_y_start + fin_height;
                    
                    gdstk::Polygon* top_fin = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                    
                    // 上方 fin 的四個頂點（矩形）
                    gdstk::Vec2 top_fin_points[4] = {
                        {fin_row_start_x, fin_y_start},
                        {fin_row_end_x, fin_y_start},
                        {fin_row_end_x, fin_y_end},
                        {fin_row_start_x, fin_y_end}
                    };
                    
                    top_fin->point_array.extend({.capacity = 0, .count = 4, .items = top_fin_points});
                    top_fin->tag = fin_layer->tag();
                    gate_wrapper_cell->polygon_array.append(top_fin);
                }
                
                LOGI << "  Created 3 top fin rows";
                LOGI << "Top and bottom fin rows created successfully";
            }
            
            // 將新 cell 加入 library
            gds_lib.cell_array.append(gate_wrapper_cell);
            LOGI << "Added new cell to library: " << new_cell_name;
            LOGI << "Gate and Fin polygons created successfully";
        }
        
        // ================================================================
        // 建立 Filler Top 和 Bottom cells
        // ================================================================
        LOGI << "========================================================================";
        LOGI << "Creating SRAM Filler Cells (Top and Bottom)";
        LOGI << "========================================================================";
        
        // 載入 filler cells 從 sram_filler_lib
        OpenFinRAM::FillerCellLibrary filler_lib;
        if (OpenFinRAM::load_filler_cells_from_library(sram_filler_lib, filler_lib)) {
            LOGI << "Successfully loaded filler cells from library";
            
            // 取得 ctrl_decode 尺寸用於 filler 配置
            gdstk::Cell* ctrl_decode_for_filler = gds_lib.get_cell("ctrl_decode");
            double filler_ctrl_width = 1.782;  // 預設值
            double filler_ctrl_height = 0.297;
            
            if (ctrl_decode_for_filler != nullptr) {
                OpenFinRAM::CellSize ctrl_size = OpenFinRAM::get_cell_size(ctrl_decode_for_filler, g_layer_map);
                if (ctrl_size.valid) {
                    filler_ctrl_width = ctrl_size.width;
                    filler_ctrl_height = ctrl_size.height;
                } else {
                    gdstk::Vec2 bb_min, bb_max;
                    ctrl_decode_for_filler->bounding_box(bb_min, bb_max);
                    filler_ctrl_width = bb_max.x - bb_min.x;
                    filler_ctrl_height = bb_max.y - bb_min.y;
                }
            }
            
            LOGI << "Filler configuration:";
            LOGI << "  test_num_bits: " << test_num_bits;
            LOGI << "  ctrl_decode width: " << filler_ctrl_width;
            LOGI << "  ctrl_decode height: " << filler_ctrl_height;
            
            // 建立 filler 配置
            OpenFinRAM::FillerConfig filler_config;
            filler_config.test_num_bits = test_num_bits;
            filler_config.ctrl_decode_width = filler_ctrl_width + 0.054 * 2;
            filler_config.ctrl_decode_height = filler_ctrl_height;
            
            // 建立 TOP filler
            filler_config.is_top = true;
            gdstk::Cell* filler_top = OpenFinRAM::create_filler_top(filler_lib, filler_config, g_layer_map);
            
            // 建立 BOTTOM filler
            filler_config.is_top = false;
            gdstk::Cell* filler_bottom = OpenFinRAM::create_filler_bottom(filler_lib, filler_config, g_layer_map);
            
            // 將 filler cells 加入 library
            if (filler_top != nullptr || filler_bottom != nullptr) {
                OpenFinRAM::add_filler_cells_to_library(gds_lib, filler_top, filler_bottom, filler_lib);
                
                LOGI << "========================================================================";
                LOGI << "Filler cells created successfully!";
                if (filler_top) LOGI << "  - " << filler_top->name;
                if (filler_bottom) LOGI << "  - " << filler_bottom->name;
                LOGI << "========================================================================";
            } else {
                LOGW << "Failed to create filler cells";
            }
        } else {
            LOGW << "Failed to load filler cells from library";
            LOGW << "Skipping filler cell generation";
        }

        // ================================================================
        // 創建 SRAM cell：組合 stacked_colgrp + ctrl_decode_with_filler + stacked_colgrp
        // ================================================================
        LOGI << "========================================================================";
        LOGI << "Creating integrated SRAM cell";
        LOGI << "========================================================================";
        
        // 讀取包含 stacked_colgrp 的 GDS 檔案
        const char* sram_array_gds_path = "sram_array_test.gds";
        LOGI << "Reading SRAM array GDS file: " << sram_array_gds_path;
        
        gdstk::ErrorCode sram_array_error = gdstk::ErrorCode::NoError;
        gdstk::Library sram_array_lib = gdstk::read_gds(sram_array_gds_path, 0, 1e-2, nullptr, &sram_array_error);
        
        if (sram_array_error == gdstk::ErrorCode::NoError && sram_array_lib.cell_array.count > 0) {
            LOGI << "Successfully read SRAM array GDS file";
            
            // 尋找 stacked_colgrp cell
            gdstk::Cell* stacked_colgrp_cell = nullptr;
            for (uint64_t i = 0; i < sram_array_lib.cell_array.count; i++) {
                if (strncmp(sram_array_lib.cell_array[i]->name, "stacked_colgrp_x", 16) == 0) {
                    stacked_colgrp_cell = sram_array_lib.cell_array[i];
                    LOGI << "Found stacked_colgrp cell: " << stacked_colgrp_cell->name;
                    break;
                }
            }
            
            if (stacked_colgrp_cell == nullptr) {
                LOGW << "Cannot find stacked_colgrp cell in SRAM array GDS";
            } else {
                // 取得 stacked_colgrp 的尺寸
                OpenFinRAM::CellSize stacked_size = OpenFinRAM::get_cell_size(stacked_colgrp_cell, g_layer_map);
                if (!stacked_size.valid) {
                    gdstk::Vec2 bb_min, bb_max;
                    stacked_colgrp_cell->bounding_box(bb_min, bb_max);
                    stacked_size.min = bb_min;
                    stacked_size.max = bb_max;
                    stacked_size.width = bb_max.x - bb_min.x;
                    stacked_size.height = bb_max.y - bb_min.y;
                    stacked_size.valid = true;
                }
                
                // 取得 ctrl_decode_with_filler 的尺寸
                gdstk::Cell* ctrl_filler_cell = gds_lib.get_cell("ctrl_decode");
                OpenFinRAM::CellSize ctrl_filler_size;
                ctrl_filler_size.valid = false;
                
                if (ctrl_filler_cell != nullptr) {
                    ctrl_filler_size = OpenFinRAM::get_cell_size(ctrl_filler_cell, g_layer_map);
                    LOGD << ctrl_filler_size.height;
                    if (!ctrl_filler_size.valid) {
                        gdstk::Vec2 bb_min, bb_max;
                        ctrl_filler_cell->bounding_box(bb_min, bb_max);
                        ctrl_filler_size.min = bb_min;
                        ctrl_filler_size.max = bb_max;
                        ctrl_filler_size.width = bb_max.x - bb_min.x;
                        ctrl_filler_size.height = bb_max.y - bb_min.y;
                        ctrl_filler_size.valid = true;
                    }
                } else {
                    LOGW << "Cannot find ctrl_decode_with_filler cell";
                }

                ctrl_filler_cell = gds_lib.get_cell("ctrl_decode_with_filler");
                
                // 取得 filler_top 和 filler_bottom 的尺寸
                // 名稱格式: FILLER_{test_num_bits*2}x2_top/bottom
                char filler_top_name[128];
                char filler_bottom_name[128];
                snprintf(filler_top_name, sizeof(filler_top_name), "FILLER_%lux%lu_top", 
                         (unsigned long)(test_num_bits * 2), 2UL);
                snprintf(filler_bottom_name, sizeof(filler_bottom_name), "FILLER_%lux%lu_bottom", 
                         (unsigned long)(test_num_bits * 2), 2UL);
                
                LOGI << "Looking for filler cells:";
                LOGI << "  Top: " << filler_top_name;
                LOGI << "  Bottom: " << filler_bottom_name;
                
                gdstk::Cell* filler_top_cell = gds_lib.get_cell(filler_top_name);
                gdstk::Cell* filler_bottom_cell = gds_lib.get_cell(filler_bottom_name);
                
                OpenFinRAM::CellSize filler_top_size = {};
                OpenFinRAM::CellSize filler_bottom_size = {};
                
                if (filler_top_cell != nullptr) {
                    filler_top_size = OpenFinRAM::get_cell_size(filler_top_cell, g_layer_map);
                    if (!filler_top_size.valid) {
                        gdstk::Vec2 bb_min, bb_max;
                        filler_top_cell->bounding_box(bb_min, bb_max);
                        filler_top_size.min = bb_min;
                        filler_top_size.max = bb_max;
                        filler_top_size.width = bb_max.x - bb_min.x;
                        filler_top_size.height = bb_max.y - bb_min.y;
                        filler_top_size.valid = true;
                    }
                } else {
                    LOGW << "filler_top cell not found";
                }
                
                if (filler_bottom_cell != nullptr) {
                    filler_bottom_size = OpenFinRAM::get_cell_size(filler_bottom_cell, g_layer_map);
                    if (!filler_bottom_size.valid) {
                        gdstk::Vec2 bb_min, bb_max;
                        filler_bottom_cell->bounding_box(bb_min, bb_max);
                        filler_bottom_size.min = bb_min;
                        filler_bottom_size.max = bb_max;
                        filler_bottom_size.width = bb_max.x - bb_min.x;
                        filler_bottom_size.height = bb_max.y - bb_min.y;
                        filler_bottom_size.valid = true;
                    }
                } else {
                    LOGW << "filler_bottom cell not found";
                }
                
                LOGI << "Component dimensions:";
                LOGI << "  stacked_colgrp: " << stacked_size.width << " x " << stacked_size.height << " um";
                LOGI << "  ctrl_decode_with_filler: " << ctrl_filler_size.width << " x " << ctrl_filler_size.height << " um";
                if (filler_top_cell != nullptr) {
                    LOGI << "  filler_top: " << filler_top_size.width << " x " << filler_top_size.height << " um";
                }
                if (filler_bottom_cell != nullptr) {
                    LOGI << "  filler_bottom: " << filler_bottom_size.width << " x " << filler_bottom_size.height << " um";
                }
                
                // 使用 get_dependencies 取得 stacked_colgrp 的所有依賴 cells
                LOGI << "Getting dependencies of stacked_colgrp...";
                gdstk::Map<gdstk::Cell*> dependencies = {};
                stacked_colgrp_cell->get_dependencies(true, dependencies);
                
                LOGI << "Found " << dependencies.capacity << " dependent cells, copying to target library...";
                
                // 先複製所有依賴的 cells
                for (gdstk::MapItem<gdstk::Cell*>* item = dependencies.next(NULL); item; item = dependencies.next(item)) {
                    gdstk::Cell* dep_cell = item->value;
                    
                    // 檢查是否已存在於目標 library
                    if (gds_lib.get_cell(dep_cell->name) == nullptr) {
                        gdstk::Cell* new_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
                        new_cell->copy_from(*dep_cell, nullptr, true);
                        gds_lib.cell_array.append(new_cell);
                        LOGI << "  Copied dependency: " << dep_cell->name;
                    }
                }
                
                // 最後複製 stacked_colgrp 本身
                gdstk::Cell* stacked_colgrp_copy = nullptr;
                if (gds_lib.get_cell(stacked_colgrp_cell->name) == nullptr) {
                    stacked_colgrp_copy = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
                    stacked_colgrp_copy->copy_from(*stacked_colgrp_cell, nullptr, true);
                    gds_lib.cell_array.append(stacked_colgrp_copy);
                    LOGI << "  Copied main cell: " << stacked_colgrp_cell->name;
                } else {
                    stacked_colgrp_copy = gds_lib.get_cell(stacked_colgrp_cell->name);
                }
                
                // 清理 dependencies map
                dependencies.clear();
                
                if (stacked_colgrp_copy == nullptr) {
                    LOGW << "Failed to copy stacked_colgrp cell";
                } else {
                    LOGI << "Successfully copied stacked_colgrp and all dependencies";
                
                    // 創建新的 SRAM cell
                    std::string sram_cell_name_str = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows);
                    const char* sram_cell_name = sram_cell_name_str.c_str();
                    
                    // 先檢查是否已存在
                    if (gds_lib.get_cell(sram_cell_name) != nullptr) {
                        LOGW << "Cell '" << sram_cell_name << "' already exists, removing it first";
                        for (uint64_t i = 0; i < gds_lib.cell_array.count; i++) {
                            if (strcmp(gds_lib.cell_array[i]->name, sram_cell_name) == 0) {
                                gds_lib.cell_array[i]->free_all();
                                gds_lib.cell_array.remove(i);
                                break;
                            }
                        }
                    }
                    
                    LOGI << "Creating integrated SRAM cell: " << sram_cell_name;
                    gdstk::Cell* sram_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
                    sram_cell->init(sram_cell_name);
                    
                    // 計算每個 component 的 Y 位置和最大寬度
                    double y_offset = 0.0;
                    double max_width = 0.0;
                    
                    // // 1. 底部：filler_bottom
                    // if (filler_bottom_cell != nullptr && filler_bottom_size.valid) {
                    //     LOGI << "Adding filler_bottom at y=" << y_offset;
                    //     gdstk::Reference* filler_bottom_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                    //     filler_bottom_ref->init(filler_bottom_cell);
                    //     filler_bottom_ref->origin = {-filler_bottom_size.min.x - 0.054, y_offset - filler_bottom_size.min.y};
                    //     filler_bottom_ref->magnification = 1.0;
                    //     sram_cell->reference_array.append(filler_bottom_ref);
                        
                    //     y_offset += filler_bottom_size.height - 0.054;
                    //     max_width = std::max(max_width, filler_bottom_size.width);
                    // }
                    
                    // 2. 底部：stacked_colgrp
                    LOGI << "Adding bottom stacked_colgrp at y=" << y_offset;
                    gdstk::Reference* bottom_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                    bottom_ref->init(stacked_colgrp_copy);
                    bottom_ref->origin = {-0.054, y_offset + stacked_size.height};
                    // bottom_ref->rotation = M_PI;  // 旋轉 180 度
                    bottom_ref->x_reflection = true;
                    bottom_ref->magnification = 1.0;
                    sram_cell->reference_array.append(bottom_ref);
                    
                    y_offset += stacked_size.height + 0.0675;
                    max_width = std::max(max_width, stacked_size.width + 0.054);
                    
                    // 3. 中間：ctrl_decode_with_filler
                    if (ctrl_filler_cell != nullptr) {
                        LOGI << "Adding ctrl_decode_with_filler at y=" << y_offset;
                        gdstk::Reference* ctrl_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                        ctrl_ref->init(ctrl_filler_cell);
                        ctrl_ref->origin = {0.0, y_offset};
                        ctrl_ref->magnification = 1.0;
                        sram_cell->reference_array.append(ctrl_ref);
                        
                        y_offset += ctrl_filler_size.height;
                        max_width = std::max(max_width, ctrl_filler_size.width);
                        LOGD << ctrl_filler_size.height;
                    }
                    
                    // 4. 頂部：stacked_colgrp（再次引用）
                    LOGI << "Adding top stacked_colgrp at y=" << y_offset;
                    gdstk::Reference* top_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                    top_ref->init(stacked_colgrp_copy);
                    top_ref->origin = {-0.054, y_offset + 0.0675};
                    top_ref->magnification = 1.0;
                    sram_cell->reference_array.append(top_ref);
                    
                    y_offset += stacked_size.height + 0.0675 - 0.027;
                    
                    // // 5. 頂部：filler_top
                    // if (filler_top_cell != nullptr && filler_top_size.valid) {
                    //     LOGI << "Adding filler_top at y=" << y_offset;
                    //     gdstk::Reference* filler_top_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                    //     filler_top_ref->init(filler_top_cell);
                    //     filler_top_ref->origin = {-filler_top_size.min.x - 0.054, y_offset - filler_top_size.min.y - 0.027};
                    //     filler_top_ref->magnification = 1.0;
                    //     sram_cell->reference_array.append(filler_top_ref);
                        
                    //     y_offset += filler_top_size.height;
                    //     max_width = std::max(max_width, filler_top_size.width);
                    // }
                    
                    // ================================================================
                    // 6. 添加 SRAM Top Level Pins
                    // Pins: vdd, vss, clk, rst_n, ce_n, we_n, A[addr_width-1:0], D[num_stacked_rows-1:0], Q[num_stacked_rows-1:0]
                    // ================================================================
                    LOGI << "========================================================================";
                    LOGI << "Adding SRAM Top Level Pins";
                    LOGI << "  addr_width = " << addr_width;
                    LOGI << "  data_bits (num_stacked_rows) = " << num_stacked_rows;
                    LOGI << "========================================================================";
                    
                    // 取得 M3 pin layer 用於添加 pins
                    const OpenFinRAM::LayerDef* sram_m3_pin_layer = g_layer_map.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
                    
                    if (sram_m3_pin_layer != nullptr) {
                        gdstk::Tag pin_tag = gdstk::make_tag(sram_m3_pin_layer->layer_number, sram_m3_pin_layer->datatype);
                        
                        // 首先，使用 flatten 將 sram_cell 展平以獲取所有 labels 的絕對位置
                        // 創建一個臨時的 sram_cell 副本用於 flatten
                        gdstk::Cell* temp_sram = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
                        temp_sram->copy_from(*sram_cell, "temp_sram_flatten", true);
                        
                        // Flatten 所有 references
                        gdstk::Array<gdstk::Reference*> removed_refs = {};
                        temp_sram->flatten(true, removed_refs);
                        
                        LOGI << "Flattened SRAM cell, found " << temp_sram->label_array.count << " labels";
                        
                        // 用於儲存找到的 pin 位置
                        struct PinLocation {
                            const char* name;
                            double x;
                            double y;
                            bool found;
                        };
                        
                        // 定義需要搜尋的 pins (vdd, vss 從 flattened labels 中尋找)
                        PinLocation fixed_pins[] = {
                            {"VDD", 0.0, 0.0, false},
                            {"VSS", 0.0, 0.0, false}
                        };
                        
                        // 從 flattened labels 中尋找 VDD 和 VSS 的位置
                        for (uint64_t i = 0; i < temp_sram->label_array.count; ++i) {
                            gdstk::Label* label = temp_sram->label_array[i];
                            if (label->text == nullptr) continue;
                            
                            if (strcasecmp(label->text, "VDD") == 0 && !fixed_pins[0].found) {
                                fixed_pins[0].x = label->origin.x;
                                fixed_pins[0].y = label->origin.y;
                                fixed_pins[0].found = true;
                                LOGI << "  Found VDD at (" << label->origin.x << ", " << label->origin.y << ")";
                            } else if (strcasecmp(label->text, "VSS") == 0 && !fixed_pins[1].found) {
                                fixed_pins[1].x = label->origin.x;
                                fixed_pins[1].y = label->origin.y;
                                fixed_pins[1].found = true;
                                LOGI << "  Found VSS at (" << label->origin.x << ", " << label->origin.y << ")";
                            }
                        }
                        
                        // 添加 VDD pin
                        if (fixed_pins[0].found) {
                            gdstk::Label* vdd_pin = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                            vdd_pin->init("vdd");
                            vdd_pin->origin = {fixed_pins[0].x, fixed_pins[0].y};
                            vdd_pin->tag = pin_tag;
                            sram_cell->label_array.append(vdd_pin);
                            LOGI << "  Added vdd pin at (" << fixed_pins[0].x << ", " << fixed_pins[0].y << ")";
                        } else {
                            // 使用預設位置
                            gdstk::Label* vdd_pin = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                            vdd_pin->init("vdd");
                            vdd_pin->origin = {0.125, y_offset / 2.0};
                            vdd_pin->tag = pin_tag;
                            sram_cell->label_array.append(vdd_pin);
                            LOGW << "  VDD not found, using default position";
                        }
                        
                        // 添加 VSS pin
                        if (fixed_pins[1].found) {
                            gdstk::Label* vss_pin = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                            vss_pin->init("vss");
                            vss_pin->origin = {fixed_pins[1].x, fixed_pins[1].y};
                            vss_pin->tag = pin_tag;
                            sram_cell->label_array.append(vss_pin);
                            LOGI << "  Added vss pin at (" << fixed_pins[1].x << ", " << fixed_pins[1].y << ")";
                        } else {
                            // 使用預設位置
                            gdstk::Label* vss_pin = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                            vss_pin->init("vss");
                            vss_pin->origin = {0.162, y_offset / 2.0};
                            vss_pin->tag = pin_tag;
                            sram_cell->label_array.append(vss_pin);
                            LOGW << "  VSS not found, using default position";
                        }
                        
                        // 從 ctrl_decode cell 中尋找 clk, rst_n, ce_n, we_n 的位置
                        // 首先從 flattened labels 中搜尋
                        struct CtrlPinInfo {
                            const char* search_name;  // 在 flattened labels 中搜尋的名稱
                            const char* pin_name;     // 最終 pin 的名稱
                            double x;
                            double y;
                            bool found;
                        };
                        
                        CtrlPinInfo ctrl_pins[] = {
                            {"clk", "clk", 0.0, 0.0, false},
                            {"rst_n", "rst_n", 0.0, 0.0, false},
                            {"ce_n", "ce_n", 0.0, 0.0, false},
                            {"oe_n", "oe_n", 0.0, 0.0, false},
                            {"we_n", "we_n", 0.0, 0.0, false}
                        };
                        
                        // 搜尋 ctrl pins
                        for (uint64_t i = 0; i < temp_sram->label_array.count; ++i) {
                            gdstk::Label* label = temp_sram->label_array[i];
                            if (label->text == nullptr) continue;
                            
                            for (int j = 0; j < 5; ++j) {
                                if (!ctrl_pins[j].found && strcasecmp(label->text, ctrl_pins[j].search_name) == 0) {
                                    ctrl_pins[j].x = label->origin.x;
                                    ctrl_pins[j].y = label->origin.y;
                                    ctrl_pins[j].found = true;
                                    LOGI << "  Found " << ctrl_pins[j].search_name << " at (" 
                                         << label->origin.x << ", " << label->origin.y << ")";
                                    break;
                                }
                            }
                        }
                        
                        // 添加 ctrl pins
                        double ctrl_pin_x = max_width * 0.5;  // 預設 x 位置
                        double ctrl_pin_y_base = y_offset / 2.0;  // 預設 y 基準位置
                        double ctrl_pin_spacing = 0.05;
                        
                        for (int j = 0; j < 5; ++j) {
                            gdstk::Label* ctrl_pin = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                            ctrl_pin->init(ctrl_pins[j].pin_name);
                            
                            if (ctrl_pins[j].found) {
                                ctrl_pin->origin = {ctrl_pins[j].x, ctrl_pins[j].y};
                            } else {
                                // 使用預設位置
                                ctrl_pin->origin = {ctrl_pin_x, ctrl_pin_y_base + j * ctrl_pin_spacing};
                                LOGW << "  " << ctrl_pins[j].search_name << " not found, using default position";
                            }
                            
                            ctrl_pin->tag = pin_tag;
                            sram_cell->label_array.append(ctrl_pin);
                            LOGI << "  Added " << ctrl_pins[j].pin_name << " pin at (" 
                                 << ctrl_pin->origin.x << ", " << ctrl_pin->origin.y << ")";
                        }
                        
                        // 添加 Address pins: A[0] to A[addr_width-1]
                        // 從 flattened labels 中搜尋 A[i] 的位置
                        LOGI << "  Adding Address pins A[0:" << (addr_width - 1) << "]";
                        
                        double addr_pin_x_base = max_width * 0.3;  // 預設 x 位置
                        double addr_pin_y_base = y_offset * 0.8;   // 預設 y 基準位置
                        double addr_pin_spacing = 0.03;
                        
                        for (uint64_t a = 0; a < addr_width; ++a) {
                            char addr_name[32];
                            snprintf(addr_name, sizeof(addr_name), "A[%lu]", (unsigned long)a);
                            
                            // 在 flattened labels 中搜尋
                            bool found = false;
                            double ax = addr_pin_x_base;
                            double ay = addr_pin_y_base - a * addr_pin_spacing;
                            
                            for (uint64_t i = 0; i < temp_sram->label_array.count; ++i) {
                                gdstk::Label* label = temp_sram->label_array[i];
                                if (label->text != nullptr && strcmp(label->text, addr_name) == 0) {
                                    ax = label->origin.x;
                                    ay = label->origin.y;
                                    found = true;
                                    break;
                                }
                            }
                            
                            gdstk::Label* addr_pin = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                            addr_pin->init(addr_name);
                            addr_pin->origin = {ax, ay};
                            addr_pin->tag = pin_tag;
                            sram_cell->label_array.append(addr_pin);
                            
                            if (found) {
                                LOGI << "    Added " << addr_name << " at (" << ax << ", " << ay << ") [from label]";
                            } else {
                                LOGD << "    Added " << addr_name << " at (" << ax << ", " << ay << ") [default]";
                            }
                        }
                        
                        // ================================================================
                        // 添加 Data Input/Output pins: D[0] to D[num_stacked_rows-1], Q[0] to Q[num_stacked_rows-1]
                        // 
                        // 由於設計使用兩個 stacked_colgrp（底部和頂部），每個都有 D[0:num_stacked_rows/2-1] 和 Q[0:num_stacked_rows/2-1]
                        // 需要區分它們：
                        // - 底部 stacked_colgrp 的 D[i]/Q[i] → 對應 top cell 的 D[i]/Q[i]
                        // - 頂部 stacked_colgrp 的 D[i]/Q[i] → 對應 top cell 的 D[i+num_stacked_rows/2]/Q[i+num_stacked_rows/2]
                        // 
                        // 使用 y 座標來區分：ctrl_decode 的 y 位置作為分界線
                        // ================================================================
                        
                        uint64_t total_data_bits = num_stacked_rows;
                        uint64_t half_data_bits = num_stacked_rows / 2;
                        LOGI << "  Adding Data pins: D[0:" << (total_data_bits - 1) << "] and Q[0:" << (total_data_bits - 1) << "]";
                        LOGI << "    Bottom stacked_colgrp: D[0:" << (half_data_bits - 1) << "], Q[0:" << (half_data_bits - 1) << "]";
                        LOGI << "    Top stacked_colgrp: D[" << half_data_bits << ":" << (total_data_bits - 1) << "], Q[" << half_data_bits << ":" << (total_data_bits - 1) << "]";
                        
                        // 計算 ctrl_decode 的 y 位置作為底部/頂部的分界線
                        // 底部 stacked_colgrp 在 ctrl_decode 下方，頂部在上方
                        double bottom_stacked_y_start = (filler_bottom_cell != nullptr && filler_bottom_size.valid) 
                                                        ? (filler_bottom_size.height - 0.054) : 0.0;
                        double bottom_stacked_y_end = bottom_stacked_y_start + stacked_size.height;
                        double ctrl_y_start = bottom_stacked_y_end + 0.0675;
                        double ctrl_y_end = ctrl_y_start + ctrl_filler_size.height;
                        double top_stacked_y_start = ctrl_y_end + 0.0675;
                        
                        LOGI << "    Y boundaries: bottom=[" << bottom_stacked_y_start << "," << bottom_stacked_y_end 
                             << "], ctrl=[" << ctrl_y_start << "," << ctrl_y_end 
                             << "], top_start=" << top_stacked_y_start;
                        
                        // 收集所有 D 和 Q labels，按 y 座標分類
                        struct DataPinInfo {
                            double x;
                            double y;
                            uint64_t original_index;  // 原始 index（從 label 名稱解析）
                            bool is_bottom;           // true = 底部 stacked_colgrp
                            bool found;
                        };
                        
                        // 為 D 和 Q 各創建 num_stacked_rows 個 pin 位置
                        std::vector<DataPinInfo> d_pins(total_data_bits);
                        std::vector<DataPinInfo> q_pins(total_data_bits);
                        
                        // 初始化
                        double data_pin_x_base = max_width * 0.6;
                        double q_pin_x_base = max_width * 0.7;
                        double data_pin_y_base = y_offset * 0.2;
                        double data_pin_spacing = 0.03;
                        
                        for (uint64_t i = 0; i < total_data_bits; ++i) {
                            d_pins[i] = {data_pin_x_base, data_pin_y_base + i * data_pin_spacing, i, (i < half_data_bits), false};
                            q_pins[i] = {q_pin_x_base, data_pin_y_base + i * data_pin_spacing, i, (i < half_data_bits), false};
                        }
                        
                        // 從 flattened labels 中搜尋 D[i] 和 Q[i]，根據 y 座標分類
                        for (uint64_t i = 0; i < temp_sram->label_array.count; ++i) {
                            gdstk::Label* label = temp_sram->label_array[i];
                            if (label->text == nullptr) continue;
                            
                            // 檢查是否為 D[n] 格式
                            if (strncmp(label->text, "D[", 2) == 0) {
                                // 解析 index
                                int idx = atoi(label->text + 2);
                                if (idx >= 0 && idx < (int)half_data_bits) {
                                    // 根據 y 座標判斷是底部還是頂部
                                    bool is_bottom = (label->origin.y < ctrl_y_start);
                                    uint64_t target_idx = is_bottom ? idx : (idx + half_data_bits);
                                    
                                    if (!d_pins[target_idx].found) {
                                        d_pins[target_idx].x = label->origin.x;
                                        d_pins[target_idx].y = label->origin.y;
                                        d_pins[target_idx].found = true;
                                        LOGD << "    Found D[" << idx << "] at y=" << label->origin.y 
                                             << " -> " << (is_bottom ? "bottom" : "top") << " -> D[" << target_idx << "]";
                                    }
                                }
                            }
                            // 檢查是否為 Q[n] 格式
                            else if (strncmp(label->text, "Q[", 2) == 0) {
                                // 解析 index
                                int idx = atoi(label->text + 2);
                                if (idx >= 0 && idx < (int)half_data_bits) {
                                    // 根據 y 座標判斷是底部還是頂部
                                    bool is_bottom = (label->origin.y < ctrl_y_start);
                                    uint64_t target_idx = is_bottom ? idx : (idx + half_data_bits);
                                    
                                    if (!q_pins[target_idx].found) {
                                        q_pins[target_idx].x = label->origin.x;
                                        q_pins[target_idx].y = label->origin.y;
                                        q_pins[target_idx].found = true;
                                        LOGD << "    Found Q[" << idx << "] at y=" << label->origin.y 
                                             << " -> " << (is_bottom ? "bottom" : "top") << " -> Q[" << target_idx << "]";
                                    }
                                }
                            }
                        }
                        
                        // 添加 D pins
                        LOGI << "  Adding Data Input pins D[0:" << (total_data_bits - 1) << "]";
                        for (uint64_t d = 0; d < total_data_bits; ++d) {
                            char d_name[32];
                            snprintf(d_name, sizeof(d_name), "D[%lu]", (unsigned long)d);
                            
                            gdstk::Label* d_pin = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                            d_pin->init(d_name);
                            d_pin->origin = {d_pins[d].x, d_pins[d].y};
                            d_pin->tag = pin_tag;
                            sram_cell->label_array.append(d_pin);
                            
                            if (d_pins[d].found) {
                                LOGI << "    Added " << d_name << " at (" << d_pins[d].x << ", " << d_pins[d].y << ") [from label]";
                            } else {
                                LOGD << "    Added " << d_name << " at (" << d_pins[d].x << ", " << d_pins[d].y << ") [default]";
                            }
                        }
                        
                        // 添加 Q pins
                        LOGI << "  Adding Data Output pins Q[0:" << (total_data_bits - 1) << "]";
                        for (uint64_t q = 0; q < total_data_bits; ++q) {
                            char q_name[32];
                            snprintf(q_name, sizeof(q_name), "Q[%lu]", (unsigned long)q);
                            
                            gdstk::Label* q_pin = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                            q_pin->init(q_name);
                            q_pin->origin = {q_pins[q].x, q_pins[q].y};
                            q_pin->tag = pin_tag;
                            sram_cell->label_array.append(q_pin);
                            
                            if (q_pins[q].found) {
                                LOGI << "    Added " << q_name << " at (" << q_pins[q].x << ", " << q_pins[q].y << ") [from label]";
                            } else {
                                LOGD << "    Added " << q_name << " at (" << q_pins[q].x << ", " << q_pins[q].y << ") [default]";
                            }
                        }

                        // ================================================================
                        // 取得並列印所有 M4 metal 的位置 (使用 flattened temp_sram)
                        // ================================================================
                        const OpenFinRAM::LayerDef* m4_drawing_layer = g_layer_map.get_layer("M4", OpenFinRAM::LayerPurpose::Drawing);
                        if (m4_drawing_layer == nullptr) {
                            LOGW << "Cannot find M4 drawing layer definition, skipping M4 metal logging";
                        } else {
                            LOGI << "  Logging all M4 metal polygons (layer=" << m4_drawing_layer->layer_number
                                 << ", datatype=" << m4_drawing_layer->datatype << ")";

                            struct M4Track {
                                double y_bottom;
                                double y_top;
                            };

                            const double kTrackPitch = 0.024;  // M4/V3 routing track pitch
                            const double kEps = 1e-6;
                            std::vector<M4Track> m4_tracks;

                            uint64_t m4_count = 0;
                            for (uint64_t i = 0; i < temp_sram->polygon_array.count; ++i) {
                                gdstk::Polygon* poly = temp_sram->polygon_array[i];
                                if (poly == nullptr) continue;

                                if (gdstk::get_layer(poly->tag) == m4_drawing_layer->layer_number) {
                                    gdstk::Vec2 bb_min, bb_max;
                                    poly->bounding_box(bb_min, bb_max);
                                    LOGD << "    M4 metal #" << m4_count
                                         << " bbox=(" << bb_min.x << ", " << bb_min.y << ")-"
                                         << "(" << bb_max.x << ", " << bb_max.y << ")"
                                         << " size=(" << (bb_max.x - bb_min.x) << " x " << (bb_max.y - bb_min.y) << ")"
                                         << " datatype=" << gdstk::get_type(poly->tag);

                                    // Record unique M4 track (by y_bottom/y_top)
                                    bool exists = false;
                                    for (const auto& t : m4_tracks) {
                                        if (std::fabs(t.y_bottom - bb_min.y) < kEps && std::fabs(t.y_top - bb_max.y) < kEps) {
                                            exists = true;
                                            break;
                                        }
                                    }
                                    if (!exists) {
                                        m4_tracks.push_back({bb_min.y, bb_max.y});
                                    }

                                    m4_count++;
                                }
                            }

                            LOGI << "  Total M4 metal polygons found: " << m4_count;

                            // ================================================================
                            // 在 SRAM top cell 上加入 VDD/VSS 專用的橫向 M4 與 V3
                            // 位置需對齊既有 M4 routing tracks (0.024 pitch)
                            // ================================================================
                            const OpenFinRAM::LayerDef* v3_drawing_layer = g_layer_map.get_layer("V3", OpenFinRAM::LayerPurpose::Drawing);
                            if (v3_drawing_layer == nullptr) {
                                LOGW << "Cannot find V3 drawing layer definition, skipping V3 creation";
                            } else if (m4_tracks.empty()) {
                                LOGW << "No M4 tracks found, skipping VDD/VSS M4/V3 creation";
                            } else {
                                // SRAM 寬度 (用目前已組裝的 sram_cell bounding box)
                                gdstk::Vec2 sram_bb_min, sram_bb_max;
                                sram_cell->bounding_box(sram_bb_min, sram_bb_max);
                                const double sram_x_left = sram_bb_min.x;
                                const double sram_x_right = sram_bb_max.x;

                                // ------------------------------------------------------------
                                // V3 x-positions (offset-based method, same as colgrp V3 logic)
                                // ------------------------------------------------------------
                                std::vector<double> vdd_via_xs;
                                std::vector<double> vss_via_xs;
                                struct ExtraVia {
                                    double x;
                                    double width;
                                };
                                std::vector<ExtraVia> vdd_extra_vias;
                                std::vector<ExtraVia> vss_extra_vias;
                                auto dedupe_xs = [&](std::vector<double>& xs) {
                                    std::vector<double> unique;
                                    for (double x : xs) {
                                        bool exists = false;
                                        for (double u : unique) {
                                            if (std::fabs(u - x) < 1e-6) {
                                                exists = true;
                                                break;
                                            }
                                        }
                                        if (!exists) unique.push_back(x);
                                    }
                                    xs.swap(unique);
                                };

                                // Use geometric offsets only if required cells are available
                                if (sram_array != nullptr && filler_cgedge != nullptr && io_colgrp != nullptr) {
                                    OpenFinRAM::CellSize array_size = OpenFinRAM::get_cell_size(sram_array, g_layer_map);
                                    OpenFinRAM::CellSize filler_size = OpenFinRAM::get_cell_size(filler_cgedge, g_layer_map);
                                    OpenFinRAM::CellSize io_size = OpenFinRAM::get_cell_size(io_colgrp, g_layer_map);

                                    if (!array_size.valid) {
                                        gdstk::Vec2 bb_min, bb_max;
                                        sram_array->bounding_box(bb_min, bb_max);
                                        array_size.min = bb_min;
                                        array_size.max = bb_max;
                                        array_size.width = bb_max.x - bb_min.x;
                                        array_size.height = bb_max.y - bb_min.y;
                                        array_size.valid = true;
                                    }
                                    if (!filler_size.valid) {
                                        gdstk::Vec2 bb_min, bb_max;
                                        filler_cgedge->bounding_box(bb_min, bb_max);
                                        filler_size.min = bb_min;
                                        filler_size.max = bb_max;
                                        filler_size.width = bb_max.x - bb_min.x;
                                        filler_size.height = bb_max.y - bb_min.y;
                                        filler_size.valid = true;
                                    }
                                    if (!io_size.valid) {
                                        gdstk::Vec2 bb_min, bb_max;
                                        io_colgrp->bounding_box(bb_min, bb_max);
                                        io_size.min = bb_min;
                                        io_size.max = bb_max;
                                        io_size.width = bb_max.x - bb_min.x;
                                        io_size.height = bb_max.y - bb_min.y;
                                        io_size.valid = true;
                                    }

                                    const double cell_width = sram_cell_size.width > 0 ? sram_cell_size.width : 0.108;
                                    const double dummy_width = cell_width;
                                    const double left_array_x_start = filler_size.width;
                                    const double io_x_offset = left_array_x_start + array_size.width;
                                    const double right_array_x_start = io_x_offset + io_size.width;

                                    const double vss_x_in_dummy = 0.0735;
                                    const double vdd_x_in_dummy = 0.082;
                                    const double power_via_width = 0.018;

                                    auto add_x_left = [&](std::vector<double>& xs, double x_left) {
                                        xs.push_back(x_left + power_via_width / 2.0);
                                    };

                                    // 左側 array 左邊 dummy
                                    add_x_left(vss_via_xs, left_array_x_start + vss_x_in_dummy - 0.0195 - power_via_width / 2.0);
                                    add_x_left(vdd_via_xs, left_array_x_start + vdd_x_in_dummy - 0.064 - power_via_width / 2.0);

                                    // 右側 array 右邊 dummy
                                    add_x_left(vss_via_xs, right_array_x_start + array_size.width - vss_x_in_dummy + 0.0195 - power_via_width / 2.0);
                                    add_x_left(vdd_via_xs, right_array_x_start + array_size.width - vdd_x_in_dummy + 0.064 - power_via_width / 2.0);

                                    // 左側 array 右邊 dummy (sramcol 右側)
                                    const double left_array_right_dummy_x = left_array_x_start + test_num_bits * cell_width;
                                    add_x_left(vss_via_xs, left_array_right_dummy_x + vss_x_in_dummy - 0.0195 - power_via_width / 2.0 + dummy_width);
                                    add_x_left(vdd_via_xs, left_array_right_dummy_x + vdd_x_in_dummy - 0.064 - power_via_width / 2.0 + 0.198);

                                    // 右側 array 左邊 dummy (Y-flipped)
                                    const double right_array_left_dummy_x = right_array_x_start + dummy_width;
                                    add_x_left(vss_via_xs, right_array_left_dummy_x + dummy_width - vss_x_in_dummy + 0.0195 - power_via_width / 2.0);
                                    add_x_left(vdd_via_xs, right_array_left_dummy_x + dummy_width - vdd_x_in_dummy + 0.064 - power_via_width / 2.0 - 0.09);

                                    // Apply stacked_colgrp x offset (matches reference placement)
                                    for (double& x : vdd_via_xs) x += sram_x_left;
                                    for (double& x : vss_via_xs) x += sram_x_left;

                                    dedupe_xs(vdd_via_xs);
                                    dedupe_xs(vss_via_xs);

                                    std::sort(vdd_via_xs.begin(), vdd_via_xs.end());
                                    std::sort(vss_via_xs.begin(), vss_via_xs.end());

                                    if (vdd_via_xs.size() >= 2) {
                                        double second_vdd = vdd_via_xs[1];
                                        vdd_extra_vias.push_back({second_vdd + 0.684, 0.09});
                                    }
                                    if (vss_via_xs.size() >= 2) {
                                        double second_vss = vss_via_xs[1];
                                        vss_extra_vias.push_back({second_vss + 1.246, 0.026});
                                        // vss_extra_vias.push_back({second_vss + 1.2465 + 0.1575, 0.036});
                                    }

                                    // Expand via locations across all muxed colgrps
                                    if (num_mux > 1) {
                                        double colgrp_width = 0.0;
                                        if (filler_size.valid && array_size.valid && io_size.valid) {
                                            colgrp_width = filler_size.width * 2.0 + array_size.width * 2.0 + io_size.width;
                                        } else if (stacked_size.width > kEps) {
                                            colgrp_width = stacked_size.width / (double)num_mux;
                                        }

                                        if (colgrp_width > kEps) {
                                            std::vector<double> expanded_vdd_via_xs;
                                            std::vector<double> expanded_vss_via_xs;
                                            std::vector<ExtraVia> expanded_vdd_extra_vias;
                                            std::vector<ExtraVia> expanded_vss_extra_vias;

                                            expanded_vdd_via_xs.reserve(vdd_via_xs.size() * num_mux);
                                            expanded_vss_via_xs.reserve(vss_via_xs.size() * num_mux);
                                            expanded_vdd_extra_vias.reserve(vdd_extra_vias.size() * num_mux);
                                            expanded_vss_extra_vias.reserve(vss_extra_vias.size() * num_mux);

                                            for (uint64_t mux = 0; mux < num_mux; ++mux) {
                                                double mux_offset = (double)mux * colgrp_width;

                                                for (double x : vdd_via_xs) {
                                                    expanded_vdd_via_xs.push_back(x + mux_offset);
                                                }
                                                for (double x : vss_via_xs) {
                                                    expanded_vss_via_xs.push_back(x + mux_offset);
                                                }
                                                for (const auto& extra : vdd_extra_vias) {
                                                    expanded_vdd_extra_vias.push_back({extra.x + mux_offset, extra.width});
                                                }
                                                for (const auto& extra : vss_extra_vias) {
                                                    expanded_vss_extra_vias.push_back({extra.x + mux_offset, extra.width});
                                                }
                                            }

                                            vdd_via_xs.swap(expanded_vdd_via_xs);
                                            vss_via_xs.swap(expanded_vss_via_xs);
                                            vdd_extra_vias.swap(expanded_vdd_extra_vias);
                                            vss_extra_vias.swap(expanded_vss_extra_vias);
                                        } else {
                                            LOGW << "  Cannot determine colgrp width for mux expansion; skipping extra V3 replication";
                                        }
                                    }
                                }

                                // 取得 VDD/VSS label 位置 (from flattened temp_sram)
                                struct PowerLabel {
                                    const char* name;
                                    double x;
                                    double y;
                                };
                                std::vector<PowerLabel> power_labels;

                                for (uint64_t i = 0; i < temp_sram->label_array.count; ++i) {
                                    gdstk::Label* label = temp_sram->label_array[i];
                                    if (label == nullptr || label->text == nullptr) continue;
                                    if (strcasecmp(label->text, "VDD") == 0 || strcasecmp(label->text, "VSS") == 0) {
                                        power_labels.push_back({label->text, label->origin.x, label->origin.y});
                                    }
                                }

                                // helper: snap to M4 track grid (base M4 + n * 0.024) within region
                                auto snap_track_in_region = [&](double target_y, double region_y_min, double region_y_max, M4Track& out_track) -> bool {
                                    bool found = false;
                                    double best_dist = DBL_MAX;

                                    for (const auto& base : m4_tracks) {
                                        double base_center = 0.5 * (base.y_bottom + base.y_top);
                                        double height = base.y_top - base.y_bottom;
                                        if (height <= kEps) continue;

                                        // snap by integer multiples of pitch
                                        long n = lround((target_y - base_center) / kTrackPitch);
                                        double snapped_center = base_center + n * kTrackPitch;

                                        // adjust to fit within region if needed
                                        if (snapped_center < region_y_min - kEps) {
                                            n = (long)std::ceil((region_y_min - base_center) / kTrackPitch);
                                            snapped_center = base_center + n * kTrackPitch;
                                        } else if (snapped_center > region_y_max + kEps) {
                                            n = (long)std::floor((region_y_max - base_center) / kTrackPitch);
                                            snapped_center = base_center + n * kTrackPitch;
                                        }

                                        double y_bottom = snapped_center - height / 2.0;
                                        double y_top = snapped_center + height / 2.0;

                                        if (y_bottom < region_y_min - kEps || y_top > region_y_max + kEps) {
                                            continue;
                                        }

                                        double dist = std::fabs(snapped_center - target_y);
                                        if (dist < best_dist) {
                                            best_dist = dist;
                                            out_track = {y_bottom, y_top};
                                            found = true;
                                        }
                                    }

                                    return found;
                                };

                                // helper: snap to M4 track grid while avoiding a nearby track center
                                auto snap_track_in_region_avoiding = [&](double target_y,
                                                                          double region_y_min,
                                                                          double region_y_max,
                                                                          double avoid_center,
                                                                          double min_center_delta,
                                                                          M4Track& out_track) -> bool {
                                    bool found = false;
                                    double best_dist = DBL_MAX;
                                    const double region_height = region_y_max - region_y_min;
                                    const int max_steps = (region_height > kEps) ? (int)std::ceil(region_height / kTrackPitch) + 2 : 4;

                                    for (int step = 0; step <= max_steps; ++step) {
                                        for (int sign_idx = 0; sign_idx < 3; ++sign_idx) {
                                            double sign = (sign_idx == 0) ? 0.0 : (sign_idx == 1 ? 1.0 : -1.0);
                                            if (step == 0 && sign_idx > 0) continue;
                                            double candidate_target = target_y + sign * step * kTrackPitch;

                                            M4Track candidate;
                                            if (!snap_track_in_region(candidate_target, region_y_min, region_y_max, candidate)) {
                                                continue;
                                            }

                                            double candidate_center = 0.5 * (candidate.y_bottom + candidate.y_top);
                                            if (std::fabs(candidate_center - avoid_center) < min_center_delta) {
                                                continue;
                                            }

                                            double dist = std::fabs(candidate_center - target_y);
                                            if (dist < best_dist) {
                                                best_dist = dist;
                                                out_track = candidate;
                                                found = true;
                                            }
                                        }
                                        if (found) break;
                                    }

                                    return found;
                                };

                                // helper: check if M4 already exists across full stacked width
                                auto has_existing_m4 = [&](double y_bottom, double y_top) -> bool {
                                    for (uint64_t i = 0; i < sram_cell->polygon_array.count; ++i) {
                                        gdstk::Polygon* poly = sram_cell->polygon_array[i];
                                        if (poly == nullptr) continue;
                                        if (gdstk::get_layer(poly->tag) != m4_drawing_layer->layer_number) {
                                            continue;
                                        }
                                        gdstk::Vec2 bb_min, bb_max;
                                        poly->bounding_box(bb_min, bb_max);
                                        if (std::fabs(bb_min.y - y_bottom) < kEps && std::fabs(bb_max.y - y_top) < kEps) {
                                            if (bb_min.x <= sram_x_left + kEps && bb_max.x >= sram_x_right - kEps) {
                                                return true;
                                            }
                                        }
                                    }
                                    return false;
                                };

                                // helper: add horizontal M4 + multiple V3s per stacked row in a region
                                auto add_power_m4_v3_in_region = [&](double region_y_min, double region_y_max, const char* region_name) {
                                    LOGI << "  Adding VDD/VSS M4/V3 in " << region_name
                                         << " region y=[" << region_y_min << ", " << region_y_max << "]";
                                    const uint64_t rows_per_region = num_stacked_rows / 2;
                                    const double row_pitch = (rows_per_region > 0) ? (stacked_size.height / rows_per_region) : 1.08;

                                    auto pick_label_in_row = [&](const char* target, double row_min, double row_max, double row_center) -> const PowerLabel* {
                                        const PowerLabel* best = nullptr;
                                        double best_dist = DBL_MAX;
                                        for (const auto& p : power_labels) {
                                            if (strcasecmp(p.name, target) != 0) continue;
                                            if (p.y < row_min - kEps || p.y > row_max + kEps) continue;
                                            double dist = std::fabs(p.y - row_center);
                                            if (dist < best_dist) {
                                                best_dist = dist;
                                                best = &p;
                                            }
                                        }
                                        return best;
                                    };

                                    auto add_power_with_multiple_vias_on_track = [&](const char* net_name, const M4Track& track, double row_min, double row_max) {
                                        double track_height = track.y_top - track.y_bottom;
                                        if (track_height <= kEps) {
                                            LOGW << "    Invalid M4 track height, skipping " << net_name;
                                            return;
                                        }

                                        if (!has_existing_m4(track.y_bottom, track.y_top)) {
                                            gdstk::Polygon* m4_rect = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                                            gdstk::Vec2 points[4] = {
                                                {sram_x_left, track.y_bottom},
                                                {sram_x_right, track.y_bottom},
                                                {sram_x_right, track.y_top},
                                                {sram_x_left, track.y_top}
                                            };
                                            m4_rect->point_array.extend({.capacity = 0, .count = 4, .items = points});
                                            m4_rect->tag = gdstk::make_tag(m4_drawing_layer->layer_number, m4_drawing_layer->datatype);
                                            sram_cell->polygon_array.append(m4_rect);
                                            LOGD << "    Added M4 (" << net_name << ") at y=[" << track.y_bottom << ", " << track.y_top
                                                 << "] x=[" << sram_x_left << ", " << sram_x_right << "]";
                                        } else {
                                            LOGD << "    M4 track already exists at y=[" << track.y_bottom << ", " << track.y_top << "]";
                                        }

                                        const double via_width = 0.018;
                                        double via_height = std::min(kTrackPitch, track_height);
                                        double via_y_bottom = track.y_bottom;
                                        double via_y_top = via_y_bottom + via_height;
                                        const bool is_vdd = (strcasecmp(net_name, "VDD") == 0);
                                        const std::vector<double>& via_xs = is_vdd ? vdd_via_xs : vss_via_xs;
                                        const std::vector<ExtraVia>& extra_vias = is_vdd ? vdd_extra_vias : vss_extra_vias;

                                        if (!via_xs.empty()) {
                                            for (double x : via_xs) {
                                                double via_x_left = x - via_width / 2.0;
                                                double via_x_right = x + via_width / 2.0;

                                                gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                                                gdstk::Vec2 via_points[4] = {
                                                    {via_x_left, via_y_bottom},
                                                    {via_x_right, via_y_bottom},
                                                    {via_x_right, via_y_top},
                                                    {via_x_left, via_y_top}
                                                };
                                                via->point_array.extend({.capacity = 0, .count = 4, .items = via_points});
                                                via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
                                                sram_cell->polygon_array.append(via);
                                            }

                                            for (const auto& extra : extra_vias) {
                                                double extra_x_left = extra.x - extra.width / 2.0;
                                                double extra_x_right = extra.x + extra.width / 2.0;

                                                gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                                                gdstk::Vec2 via_points[4] = {
                                                    {extra_x_left, via_y_bottom},
                                                    {extra_x_right, via_y_bottom},
                                                    {extra_x_right, via_y_top},
                                                    {extra_x_left, via_y_top}
                                                };
                                                via->point_array.extend({.capacity = 0, .count = 4, .items = via_points});
                                                via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
                                                sram_cell->polygon_array.append(via);
                                            }
                                        } else {
                                            for (const auto& p : power_labels) {
                                                if (strcasecmp(p.name, net_name) != 0) continue;
                                                if (p.y < row_min - kEps || p.y > row_max + kEps) continue;

                                                double via_x_left = p.x - via_width / 2.0;
                                                double via_x_right = p.x + via_width / 2.0;

                                                gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                                                gdstk::Vec2 via_points[4] = {
                                                    {via_x_left, via_y_bottom},
                                                    {via_x_right, via_y_bottom},
                                                    {via_x_right, via_y_top},
                                                    {via_x_left, via_y_top}
                                                };
                                                via->point_array.extend({.capacity = 0, .count = 4, .items = via_points});
                                                via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
                                                sram_cell->polygon_array.append(via);
                                            }
                                        }
                                    };

                                    for (uint64_t row = 0; row < rows_per_region; ++row) {
                                        double row_min = region_y_min + row * row_pitch;
                                        double row_max = row_min + row_pitch;
                                        double row_center = 0.5 * (row_min + row_max);

                                        const PowerLabel* vdd = pick_label_in_row("VDD", row_min, row_max, row_center);
                                        const PowerLabel* vss = pick_label_in_row("VSS", row_min, row_max, row_center);

                                        double vdd_target_y = vdd ? vdd->y : row_center;
                                        double vss_target_y = vss ? vss->y : row_center;

                                        const double min_center_delta = 2.0 * kTrackPitch - kEps; // avoid adjacent tracks (touching)
                                        auto track_center = [&](const M4Track& t) { return 0.5 * (t.y_bottom + t.y_top); };

                                        M4Track vdd_track;
                                        M4Track vss_track;
                                        bool vdd_ok = snap_track_in_region(vdd_target_y, region_y_min, region_y_max, vdd_track);
                                        bool vss_ok = false;

                                        if (vdd_ok) {
                                            double vdd_center = track_center(vdd_track);
                                            vss_ok = snap_track_in_region_avoiding(vss_target_y, region_y_min, region_y_max,
                                                                                   vdd_center, min_center_delta, vss_track);
                                        }

                                        if (!vss_ok) {
                                            bool vss_first_ok = snap_track_in_region(vss_target_y, region_y_min, region_y_max, vss_track);
                                            if (vss_first_ok) {
                                                double vss_center = track_center(vss_track);
                                                vdd_ok = snap_track_in_region_avoiding(vdd_target_y, region_y_min, region_y_max,
                                                                                       vss_center, min_center_delta, vdd_track);
                                            }
                                            vss_ok = vss_first_ok;
                                        }

                                        if (vdd_ok) {
                                            add_power_with_multiple_vias_on_track("VDD", vdd_track, row_min, row_max);
                                        } else {
                                            LOGW << "    No M4 track found near VDD at y=" << vdd_target_y;
                                        }

                                        if (vss_ok) {
                                            add_power_with_multiple_vias_on_track("VSS", vss_track, row_min, row_max);
                                        } else {
                                            LOGW << "    No M4 track found near VSS at y=" << vss_target_y;
                                        }
                                    }
                                };

                                // bottom / top stacked_colgrp regions
                                const double bottom_region_min = bottom_stacked_y_start;
                                const double bottom_region_max = bottom_stacked_y_end;
                                const double top_region_min = top_stacked_y_start;
                                const double top_region_max = top_stacked_y_start + stacked_size.height;

                                add_power_m4_v3_in_region(bottom_region_min, bottom_region_max, "bottom stacked_colgrp");
                                add_power_m4_v3_in_region(top_region_min, top_region_max, "top stacked_colgrp");

                                // =============================================================
                                // Add horizontal D/Q straps on M4 when muxed (num_mux >= 2)
                                // =============================================================
                                if (num_mux >= 2) {
                                    struct DqLabel {
                                        bool is_d;
                                        double x;
                                        double y;
                                    };
                                    std::vector<DqLabel> d_labels;
                                    std::vector<DqLabel> q_labels;
                                    d_labels.reserve(temp_sram->label_array.count);
                                    q_labels.reserve(temp_sram->label_array.count);

                                    for (uint64_t i = 0; i < temp_sram->label_array.count; ++i) {
                                        gdstk::Label* label = temp_sram->label_array[i];
                                        if (label == nullptr || label->text == nullptr) continue;
                                        const char* text = label->text;
                                        if (text[1] != '[') continue;
                                        if (text[0] == 'D') {
                                            d_labels.push_back({true, label->origin.x, label->origin.y});
                                        } else if (text[0] == 'Q') {
                                            q_labels.push_back({false, label->origin.x + 0.0025, label->origin.y});
                                        }
                                    }

                                    auto snap_track_in_region_avoiding_existing = [&](double target_y,
                                                                                       double region_y_min,
                                                                                       double region_y_max,
                                                                                       M4Track& out_track) -> bool {
                                        const double region_height = region_y_max - region_y_min;
                                        const int max_steps = (region_height > kEps) ? (int)std::ceil(region_height / kTrackPitch) + 2 : 4;

                                        for (int step = 0; step <= max_steps; ++step) {
                                            for (int sign_idx = 0; sign_idx < 3; ++sign_idx) {
                                                double sign = (sign_idx == 0) ? 0.0 : (sign_idx == 1 ? 1.0 : -1.0);
                                                if (step == 0 && sign_idx > 0) continue;
                                                double candidate_target = target_y + sign * step * kTrackPitch;

                                                M4Track candidate;
                                                if (!snap_track_in_region(candidate_target, region_y_min, region_y_max, candidate)) {
                                                    continue;
                                                }

                                                if (has_existing_m4(candidate.y_bottom, candidate.y_top)) {
                                                    continue;
                                                }

                                                out_track = candidate;
                                                return true;
                                            }
                                        }

                                        return false;
                                    };

                                    auto snap_track_in_region_avoiding_existing_and_center = [&](double target_y,
                                                                                                  double region_y_min,
                                                                                                  double region_y_max,
                                                                                                  double avoid_center,
                                                                                                  double min_center_delta,
                                                                                                  M4Track& out_track) -> bool {
                                        const double region_height = region_y_max - region_y_min;
                                        const int max_steps = (region_height > kEps) ? (int)std::ceil(region_height / kTrackPitch) + 2 : 4;

                                        for (int step = 0; step <= max_steps; ++step) {
                                            for (int sign_idx = 0; sign_idx < 3; ++sign_idx) {
                                                double sign = (sign_idx == 0) ? 0.0 : (sign_idx == 1 ? 1.0 : -1.0);
                                                if (step == 0 && sign_idx > 0) continue;
                                                double candidate_target = target_y + sign * step * kTrackPitch;

                                                M4Track candidate;
                                                if (!snap_track_in_region(candidate_target, region_y_min, region_y_max, candidate)) {
                                                    continue;
                                                }

                                                double candidate_center = 0.5 * (candidate.y_bottom + candidate.y_top);
                                                if (std::fabs(candidate_center - avoid_center) < min_center_delta) {
                                                    continue;
                                                }

                                                if (has_existing_m4(candidate.y_bottom, candidate.y_top)) {
                                                    continue;
                                                }

                                                out_track = candidate;
                                                return true;
                                            }
                                        }

                                        return false;
                                    };

                                    auto add_m4_full_width = [&](const M4Track& track) {
                                        if (has_existing_m4(track.y_bottom, track.y_top)) {
                                            return;
                                        }
                                        gdstk::Polygon* m4_rect = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                                        gdstk::Vec2 points[4] = {
                                            {sram_x_left, track.y_bottom},
                                            {sram_x_right, track.y_bottom},
                                            {sram_x_right, track.y_top},
                                            {sram_x_left, track.y_top}
                                        };
                                        m4_rect->point_array.extend({.capacity = 0, .count = 4, .items = points});
                                        m4_rect->tag = gdstk::make_tag(m4_drawing_layer->layer_number, m4_drawing_layer->datatype);
                                        sram_cell->polygon_array.append(m4_rect);
                                    };

                                    auto add_vias_for_labels = [&](const std::vector<DqLabel>& labels, const M4Track& track,
                                                                   double region_y_min, double region_y_max) {
                                        double track_height = track.y_top - track.y_bottom;
                                        if (track_height <= kEps) return;
                                        const double via_width = 0.018;
                                        double via_height = std::min(kTrackPitch, track_height);
                                        double via_y_bottom = track.y_bottom;
                                        double via_y_top = via_y_bottom + via_height;

                                        for (const auto& label : labels) {
                                            if (label.y < region_y_min - kEps || label.y > region_y_max + kEps) continue;
                                            double via_x_left = label.x - via_width / 2.0;
                                            double via_x_right = label.x + via_width / 2.0;

                                            gdstk::Polygon* via = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                                            gdstk::Vec2 via_points[4] = {
                                                {via_x_left, via_y_bottom},
                                                {via_x_right, via_y_bottom},
                                                {via_x_right, via_y_top},
                                                {via_x_left, via_y_top}
                                            };
                                            via->point_array.extend({.capacity = 0, .count = 4, .items = via_points});
                                            via->tag = gdstk::make_tag(v3_drawing_layer->layer_number, v3_drawing_layer->datatype);
                                            sram_cell->polygon_array.append(via);
                                        }
                                    };

                                    auto add_dq_pair_in_region = [&](double region_y_min, double region_y_max, const char* region_name) {
                                        (void)region_name;
                                        const uint64_t rows_per_region = num_stacked_rows / 2;
                                        const double row_pitch = (rows_per_region > 0) ? (stacked_size.height / rows_per_region) : 1.08;

                                        for (uint64_t row = 0; row < rows_per_region; ++row) {
                                            double row_min = region_y_min + row * row_pitch;
                                            double row_max = row_min + row_pitch;

                                            double d_target_y = 0.0;
                                            double q_target_y = 0.0;
                                            size_t d_count = 0;
                                            size_t q_count = 0;

                                            for (const auto& label : d_labels) {
                                                if (label.y < row_min - kEps || label.y > row_max + kEps) continue;
                                                d_target_y += label.y;
                                                ++d_count;
                                            }
                                            for (const auto& label : q_labels) {
                                                if (label.y < row_min - kEps || label.y > row_max + kEps) continue;
                                                q_target_y += label.y;
                                                ++q_count;
                                            }

                                            if (d_count == 0 || q_count == 0) {
                                                continue;
                                            }

                                            d_target_y /= (double)d_count;
                                            q_target_y /= (double)q_count;

                                            M4Track d_track;
                                            if (!snap_track_in_region_avoiding_existing(d_target_y, row_min, row_max, d_track)) {
                                                continue;
                                            }

                                            const double min_center_delta = 2.0 * kTrackPitch - kEps;
                                            M4Track q_track;
                                            double d_center = 0.5 * (d_track.y_bottom + d_track.y_top);
                                            if (!snap_track_in_region_avoiding_existing_and_center(q_target_y, row_min, row_max,
                                                                                                   d_center, min_center_delta, q_track)) {
                                                continue;
                                            }

                                            LOGD << "  Adding D/Q M4 straps in " << region_name
                                                 << " row " << row
                                                 << " at D y=[" << d_track.y_bottom << ", " << d_track.y_top << "]"
                                                 << " and Q y=[" << q_track.y_bottom << ", " << q_track.y_top << "]";

                                            add_m4_full_width(d_track);
                                            add_m4_full_width(q_track);

                                            add_vias_for_labels(d_labels, d_track, row_min, row_max);
                                            add_vias_for_labels(q_labels, q_track, row_min, row_max);
                                        }
                                    };

                                    LOGD << "Adding D/Q M4 straps for muxed SRAM (num_mux = " << num_mux << ")"
                                         << bottom_region_min << " to " << bottom_region_max
                                         << " and " << top_region_min << " to " << top_region_max;
                                    add_dq_pair_in_region(bottom_region_min, bottom_region_max, "bottom stacked_colgrp");
                                    add_dq_pair_in_region(top_region_min - 0.31, top_region_max, "top stacked_colgrp");
                                }
                            }
                        }
                        
                        // 清理臨時 cell 和 removed references
                        for (uint64_t i = 0; i < removed_refs.count; ++i) {
                            removed_refs[i]->clear();
                            gdstk::free_allocation(removed_refs[i]);
                        }
                        removed_refs.clear();
                        
                        temp_sram->clear();
                        gdstk::free_allocation(temp_sram);
                        
                        // 統計添加的 pins
                        uint64_t total_pins = 2 + 4 + addr_width + total_data_bits * 2;  // vdd, vss + clk, rst_n, ce_n, we_n + A[] + D[] + Q[]
                        LOGI << "========================================================================";
                        LOGI << "Total pins added to SRAM top cell: " << total_pins;
                        LOGI << "  Power: vdd, vss";
                        LOGI << "  Control: clk, rst_n, ce_n, we_n";
                        LOGI << "  Address: A[0:" << (addr_width - 1) << "] (" << addr_width << " bits)";
                        LOGI << "  Data In: D[0:" << (total_data_bits - 1) << "] (" << total_data_bits << " bits)";
                        LOGI << "  Data Out: Q[0:" << (total_data_bits - 1) << "] (" << total_data_bits << " bits)";
                        LOGI << "========================================================================";
                        
                    } else {
                        LOGW << "Cannot find M3 pin layer, skipping SRAM top level pins";
                    }
                    
                    // 加入 BOUNDARY（包住所有 reference 的最外圍）
                    OpenFinRAM::CellSize sram_size = OpenFinRAM::get_cell_size_from_boundary(sram_cell, g_layer_map);
                    if (!sram_size.valid) {
                        LOGW << "Cannot get SRAM size from BOUNDARY, using overall bounding box";
                        gdstk::Vec2 bb_min, bb_max;
                        sram_cell->bounding_box(bb_min, bb_max);
                        sram_size.min = bb_min;
                        sram_size.max = bb_max;
                        sram_size.width = bb_max.x - bb_min.x;
                        sram_size.height = bb_max.y - bb_min.y;
                        sram_size.valid = true;
                    }
                    gdstk::Vec2 boundary_min = sram_size.min;
                    gdstk::Vec2 boundary_max = sram_size.max;
                    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, g_layer_map);
                    sram_cell->polygon_array.append(boundary);

                    // 在最外層 boundary 上下各加一根 full-width fin
                    const OpenFinRAM::LayerDef* sram_fin_layer = g_layer_map.get_layer("fin", OpenFinRAM::LayerPurpose::Drawing);
                    if (sram_fin_layer == nullptr) {
                        LOGW << "Cannot find fin drawing layer definition, skipping SRAM top/bottom full-width fin";
                    } else {
                        const double full_fin_height = 0.007;
                        const double full_fin_bottom_y = boundary_min.y - 0.017;
                        const double full_fin_top_y = boundary_max.y + 0.01;

                        // bottom full-width fin
                        {
                            gdstk::Polygon* bottom_full_fin = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                            gdstk::Vec2 points[4] = {
                                {boundary_min.x, full_fin_bottom_y},
                                {boundary_max.x, full_fin_bottom_y},
                                {boundary_max.x, full_fin_bottom_y + full_fin_height},
                                {boundary_min.x, full_fin_bottom_y + full_fin_height}
                            };
                            bottom_full_fin->point_array.extend({.capacity = 0, .count = 4, .items = points});
                            bottom_full_fin->tag = sram_fin_layer->tag();
                            sram_cell->polygon_array.append(bottom_full_fin);
                        }

                        // top full-width fin
                        {
                            gdstk::Polygon* top_full_fin = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
                            gdstk::Vec2 points[4] = {
                                {boundary_min.x, full_fin_top_y},
                                {boundary_max.x, full_fin_top_y},
                                {boundary_max.x, full_fin_top_y + full_fin_height},
                                {boundary_min.x, full_fin_top_y + full_fin_height}
                            };
                            top_full_fin->point_array.extend({.capacity = 0, .count = 4, .items = points});
                            top_full_fin->tag = sram_fin_layer->tag();
                            sram_cell->polygon_array.append(top_full_fin);
                        }
                    }
                    
                    // 將 SRAM cell 加入 library
                    gds_lib.cell_array.append(sram_cell);
                    
                    LOGI << "========================================================================";
                    LOGI << "SRAM cell created successfully!";
                    LOGI << "  Cell name: " << sram_cell_name;
                    LOGI << "  Total height: " << y_offset << " um";
                    LOGI << "  Total width: " << max_width << " um";
                    LOGI << "  Components:";
                    if (filler_bottom_cell != nullptr) {
                        LOGI << "    - Bottom: filler_bottom (" << filler_bottom_size.height << " um)";
                    }
                    LOGI << "    - Bottom: stacked_colgrp (" << stacked_size.height << " um)";
                    LOGI << "    - Middle: ctrl_decode_with_filler (" << ctrl_filler_size.height << " um)";
                    LOGI << "    - Top: stacked_colgrp (" << stacked_size.height << " um)";
                    if (filler_top_cell != nullptr) {
                        LOGI << "    - Top: filler_top (" << filler_top_size.height << " um)";
                    }
                    LOGI << "========================================================================";
                }
            }
        } else {
            LOGW << "Failed to read SRAM array GDS file: " << sram_array_gds_path;
        }
        
        // 寫回 GDS 檔案（在釋放 sram_array_lib 之前）
        LOGI << "Writing modified GDS back to file...";
        std::string output_gds_path = std::string(gds_path) + ".tmp";
        gdstk::ErrorCode write_error = gds_lib.write_gds(output_gds_path.c_str(), 0, NULL);
        
        if (write_error == gdstk::ErrorCode::NoError) {
            LOGI << "========================================================================";
            LOGI << "Successfully updated: " << output_gds_path;
            LOGI << "  New cells created:";
            LOGI << "    - ctrl_decode_with_filler (original ctrl_decode + fillers)";
            LOGI << "    - sram (integrated: stacked_colgrp + ctrl_decode_with_filler + stacked_colgrp)";
            LOGI << "    - FILLER_*_top (top filler row for SRAM array)";
            LOGI << "    - FILLER_*_bottom (bottom filler row for SRAM array)";
            LOGI << "  Original ctrl_decode cell preserved";
            LOGI << "  Added parameterized Gate polygons on left and right sides";
            LOGI << "  Added parameterized Fin polygons on left and right sides";
            LOGI << "  Added parameterized Gate filler rows on top and bottom";
            LOGI << "  Added parameterized Fin rows on top and bottom";
            LOGI << "========================================================================";
        } else {
            LOGW << "Failed to write GDS file";
        }
        
        // 寫入完成後才清理 libraries
        // 注意：必須在寫入後才釋放 sram_array_lib，因為複製的 cells 中的 references
        //       仍然指向 sram_array_lib 中的 cells
        if (sram_array_error == gdstk::ErrorCode::NoError && sram_array_lib.cell_array.count > 0) {
            LOGI << "Cleaning up SRAM array library...";
            // sram_array_lib.free_all();
        }
        
        // 清理 gds_lib
        // gds_lib.free_all();
    } else {
        LOGW << "Failed to read GDS file or no cells found";
    }

    consolidate_output_artifacts(test_num_bits, num_stacked_rows, num_mux);

    // // ========================================================================
    // // Run LVS (create lvs folder, generate _run_control.svrf, run calibre)
    // // ========================================================================
    // {
    //     std::string sram_cell_name = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows);
    //     std::string lvs_log_path;
    //     std::string lvs_error;

    //     LOGI << "Running LVS for cell: " << sram_cell_name;
    //     bool lvs_ok = OpenFinRAM::run_lvs(".", "../innovus/ctrl_decode.gds.tmp", "../sram.sp", sram_cell_name, &lvs_log_path, &lvs_error);

    //     if (lvs_ok) {
    //         LOGI << "LVS completed. CORRECT.";
    //     } else {
    //         LOGW << "LVS failed or not correct. Log: " << lvs_log_path;
    //         if (!lvs_error.empty()) {
    //             LOGW << "LVS error: " << lvs_error;
    //         }
    //     }
    // }

    // // ========================================================================
    // // Export LEF (create cds.lib, import GDS, run abstract)
    // // ========================================================================
    // {
    //     std::string sram_cell_name = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows);
    //     std::string lef_log_path;
    //     std::string lef_error;

    //     LOGI << "Exporting LEF for cell: " << sram_cell_name;
    //     bool lef_ok = OpenFinRAM::export_lef(".", sram_cell_name, "./innovus/ctrl_decode.gds.tmp", &lef_log_path, &lef_error);

    //     if (lef_ok) {
    //         LOGI << "LEF export completed. Log: " << lef_log_path;
    //     } else {
    //         LOGW << "LEF export failed. Log: " << lef_log_path;
    //         if (!lef_error.empty()) {
    //             LOGW << "LEF export error: " << lef_error;
    //         }
    //     }
    // }

    // 釋放資源
    sram_filler_lib.free_all();

    return 0;
}
