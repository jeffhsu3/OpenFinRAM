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
#include "siliconsmart_generator.hpp"
#include "lvs_runner.hpp"
#include "lef_extractor.hpp"
#include "main_config_helpers.hpp"
#include "main_flow_helpers.hpp"
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

    MainCliOptions cli_options = parse_main_cli_options(argc, argv);
    uint64_t test_num_bits = cli_options.test_num_bits;
    uint64_t num_stacked_rows = cli_options.num_stacked_rows;
    uint64_t num_mux = cli_options.num_mux;
    bool run_verification = cli_options.run_verification;

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

    RuntimeDerivedParams derived_params = derive_runtime_params(test_num_bits, num_mux);
    uint64_t num_wl = derived_params.num_wl;
    uint64_t addr_width = derived_params.addr_width;
    log_runtime_derived_params(derived_params);

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
    bool pex_success = run_or_predict_pex(run_actual_pex, test_num_bits, num_stacked_rows);

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
        if (!decide_base_delay_count(
                run_verification,
                force_best_run,
                low_buffer,
                high_buffer,
                best_pass_buffer,
                min_buffer_cnt,
                max_buffer_cnt,
                base_delay_cnt)) {
            return 1;
        }

        if (attempt > 3) {
            LOGE << "Exceeded maximum synthesis attempts (3). Exiting.";
            return 1;
        }

        attempt++; 
        verification_passed = false;

        // delay_prech_cnt = std::max<uint64_t>(1, base_delay_cnt / 10);  // 10 倍差距

        run_synthesis_stage(
            attempt,
            addr_width,
            test_num_bits,
            num_mux,
            delay_prech_cnt,
            base_delay_cnt,
            num_stacked_rows,
            run_verification,
            low_buffer,
            high_buffer,
            pex_success);

        run_spice_conversion_stage();

        run_innovus_stage(
            stacked_colgrp,
            test_num_bits,
            addr_width,
            num_mux,
            g_layer_map);

        run_sram_integration_stage(
            addr_width,
            num_stacked_rows,
            test_num_bits,
            num_mux);

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

        if (!update_bisection_on_success(
                base_delay_cnt,
                low_buffer,
                force_best_run,
                best_pass_buffer,
                high_buffer)) {
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

        add_ctrl_decode_gate_fin_wrappers(gds_lib, g_layer_map);
        
        // ================================================================
        // 建立 Filler Top 和 Bottom cells
        // ================================================================
        create_and_add_sram_filler_cells(gds_lib, sram_filler_lib, test_num_bits, g_layer_map);

        // ================================================================
        run_sram_gds_integration_and_writeback(
            gds_lib,
            gds_path,
            sram_array,
            filler_cgedge,
            io_colgrp,
            sram_cell_size,
            test_num_bits,
            num_stacked_rows,
            addr_width,
            num_mux,
            g_layer_map);
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
