#include "layout_generator.hpp"

#include "plog/Log.h"

#include "cell_utils.hpp"
#include "layermap.hpp"
#include "utils.hpp"

LayoutGenerator::LayoutGenerator(const MainCliOptions& options, const OpenFinRAM::LayerMap& layer_map) : cli_options_(options), layer_map_(layer_map) {}

bool LayoutGenerator::load_sram_gds() {
    std::string path = join_path(get_current_dir_name(), "tech/gds/srambank_32b_boundary_2.gds");

    gdstk::ErrorCode error_code;
    sram_lib = gdstk::read_gds(path.c_str(), 0, 1e-2, nullptr, &error_code);
    
    if (error_code != gdstk::ErrorCode::NoError) {
        LOGE << "Error reading SRAM GDS file: " << path;
        LOGE << "Error code: " << static_cast<int>(error_code);
        return false;
    }
    
    LOGI << "Successfully read SRAM GDS file: " << path;
    LOGI << "Number of cells in library: " << sram_lib.cell_array.count;
    
    return true;
}

bool LayoutGenerator::extract_required_cells() {
    const char* cell_names[] = {"FILLER_BLANK_6t122", "sram_cell_6t_122", "dummy_sram_6t122", "tapcell_sram_6t122", 
                                "dummy_topbot_v1", "dummy_topbot_v2", "FILLER_cgedge", "iocolgrp_sram_6t122_v2"};
    for (const char* name : cell_names) {
        gdstk::Cell* cell = sram_lib.get_cell(name);
        if (cell == nullptr) {
            LOGW << "Cannot find cell '" << name << "' in SRAM library!";
            return false;
        } else {
            LOGI << "Found cell: " << name;
            if (strcmp(name, "FILLER_BLANK_6t122") == 0) {
                sram_cells_.filler = cell;
            } else if (strcmp(name, "sram_cell_6t_122") == 0) {
                sram_cells_.bitcell = cell;
            } else if (strcmp(name, "dummy_sram_6t122") == 0) {
                sram_cells_.dummy = cell;
            } else if (strcmp(name, "tapcell_sram_6t122") == 0) {
                sram_cells_.tapcell = cell;
            } else if (strcmp(name, "dummy_topbot_v1") == 0) {
                sram_cells_.dummy_v1 = cell;
            } else if (strcmp(name, "dummy_topbot_v2") == 0) {
                sram_cells_.dummy_v2 = cell;
            } else if (strcmp(name, "FILLER_cgedge") == 0) {
                sram_cells_.cgedge = cell;
            } else if (strcmp(name, "iocolgrp_sram_6t122_v2") == 0) {
                sram_cells_.io_colgrp = cell;
            }
        }
    }

    return true;
}

bool LayoutGenerator::create_sram_column() {
    int num_bits = cli_options_.num_wls;

    // Get SRAM cell size
    OpenFinRAM::CellSize cell_size = OpenFinRAM::get_cell_size(sram_cells_.bitcell, layer_map_);
    LOGD << cell_size.height;
    
    if (!cell_size.valid) {
        LOGW << "Cannot get SRAM cell size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        sram_cells_.bitcell->bounding_box(bb_min, bb_max);
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
    ref_normal->init(sram_cells_.bitcell);
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
    ref_flipped->init(sram_cells_.bitcell);
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
    if (sram_cells_.dummy != nullptr) {
        OpenFinRAM::CellSize dummy_size = OpenFinRAM::get_cell_size(sram_cells_.dummy, layer_map_);
        if (!dummy_size.valid) {
            gdstk::Vec2 bb_min, bb_max;
            sram_cells_.dummy->bounding_box(bb_min, bb_max);
            dummy_size.min = bb_min;
            dummy_size.max = bb_max;
            dummy_size.width = bb_max.x - bb_min.x;
            dummy_size.height = bb_max.y - bb_min.y;
        }
        dummy_width = dummy_size.width;
        
        gdstk::Reference* ref_dummy = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref_dummy->init(sram_cells_.dummy);
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
    if (sram_cells_.tapcell != nullptr) {
        OpenFinRAM::CellSize tapcell_size = OpenFinRAM::get_cell_size(sram_cells_.tapcell, layer_map_);
        if (!tapcell_size.valid) {
            gdstk::Vec2 bb_min, bb_max;
            sram_cells_.tapcell->bounding_box(bb_min, bb_max);
            tapcell_size.min = bb_min;
            tapcell_size.max = bb_max;
            tapcell_size.width = bb_max.x - bb_min.x;
            tapcell_size.height = bb_max.y - bb_min.y;
        }
        tapcell_width = tapcell_size.width;
        
        gdstk::Reference* ref_tapcell = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref_tapcell->init(sram_cells_.tapcell);
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
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map_);
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
    const OpenFinRAM::LayerDef* m3_pin_layer = layer_map_.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
    
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
            wl_label->magnification = 0.02; 
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
    
    const OpenFinRAM::LayerDef* m2_pin_layer = layer_map_.get_layer("M2", OpenFinRAM::LayerPurpose::Pin);
    
    if (m2_pin_layer == nullptr) {
        LOGW << "Cannot find M2 pin layer definition, skipping BL/BLN/VDD/VSS pins";
    } else if (sram_cells_.dummy != nullptr) {
        double sram_total_width = num_bits * cell_width;  // sram_cells_.dummy 的 x 位置
        
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
            label->magnification = 0.02;
            column_cell->label_array.append(label);
        }
        
        LOGI << "  Added BL/BLN/VDD/VSS pins on M2 pin layer (" 
             << m2_pin_layer->layer_number << ", " << m2_pin_layer->datatype << ")";
    }
    
    LOGI << "Created SRAM column '" << cell_name << "'";
    LOGI << "  Normal cells: " << num_normal << " (at x = 0, 2w, 4w, ...)";
    LOGI << "  Flipped cells: " << num_flipped << " (at x = w, 3w, 5w, ...)";
    LOGI << "  Dummy cell: " << (sram_cells_.dummy ? sram_cells_.dummy->name : "none");
    LOGI << "  Tapcell: " << (sram_cells_.tapcell ? sram_cells_.tapcell->name : "none");
    LOGI << "  Total size: " << total_width << " x " << cell_height;

    sram_cells_.sram_column = column_cell;
    
    return true;
}

bool LayoutGenerator::create_sram_array() {
    int num_rows = 4;  // 預設堆疊 4 層
    if (sram_cells_.sram_column == nullptr || num_rows == 0) {
        LOGE << "Invalid parameters for create_sram_array";
        return false;
    }
    
    // 取得 sramcol 尺寸
    OpenFinRAM::CellSize col_size = OpenFinRAM::get_cell_size(sram_cells_.sram_column, layer_map_);
    
    if (!col_size.valid) {
        LOGW << "Cannot get SRAM column size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        sram_cells_.sram_column->bounding_box(bb_min, bb_max);
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
    if (sram_cells_.dummy_v1 != nullptr) {
        dummy_v1_size = OpenFinRAM::get_cell_size(sram_cells_.dummy_v1, layer_map_);
        if (!dummy_v1_size.valid) {
            gdstk::Vec2 bb_min, bb_max;
            sram_cells_.dummy_v1->bounding_box(bb_min, bb_max);
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
    if (sram_cells_.dummy_v2 != nullptr) {
        dummy_v2_size = OpenFinRAM::get_cell_size(sram_cells_.dummy_v2, layer_map_);
        if (!dummy_v2_size.valid) {
            gdstk::Vec2 bb_min, bb_max;
            sram_cells_.dummy_v2->bounding_box(bb_min, bb_max);
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
    const char* col_name = sram_cells_.sram_column->name;
    int bits = 0;
    const char* x_pos = strstr(col_name, "_x");
    if (x_pos != nullptr) {
        bits = atoi(x_pos + 2);
    }
    
    char array_name[64];
    snprintf(array_name, sizeof(array_name), "array_x%dx%lu", (unsigned long)num_rows, (unsigned long)bits);
    
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
        ref->init(sram_cells_.sram_column);
        ref->magnification = 1.0;
        
        if (row % 2 == 0) {
            // 偶數層：正著擺
            ref->origin = {sramcol_x_offset - col_size.min.x, y_pos - col_size.min.y};
            ref->x_reflection = false;
            LOGI << "  Row " << row << ": sramcol normal at x=" << sramcol_x_offset << ", y=" << y_pos;
            
            // 放置 dummy_topbot_v1 在最左邊 (Y軸翻轉: rotation = PI + x_reflection = true)
            if (sram_cells_.dummy_v1 != nullptr) {
                gdstk::Reference* dummy_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                dummy_ref->init(sram_cells_.dummy_v1);
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
            if (sram_cells_.dummy_v2 != nullptr) {
                gdstk::Reference* dummy_ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
                dummy_ref->init(sram_cells_.dummy_v2);
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
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map_);
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
    const OpenFinRAM::LayerDef* m3_pin_layer = layer_map_.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
    const OpenFinRAM::LayerDef* m2_pin_layer = layer_map_.get_layer("M2", OpenFinRAM::LayerPurpose::Pin);
    
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
            wl_label->magnification = 0.02;
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
            bln_label->magnification = 0.02;
            array_cell->label_array.append(bln_label);
            
            // BL[row]
            char bl_name[32];
            snprintf(bl_name, sizeof(bl_name), "BL[%lu]", (unsigned long)row);
            
            gdstk::Label* bl_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
            bl_label->init(bl_name);
            bl_label->origin = {bl_pin_x, bl_pin_y};
            bl_label->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
            bl_label->magnification = 0.02;
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
        vdd_label->magnification = 0.02;
        array_cell->label_array.append(vdd_label);
        
        // VSS pin (底部)
        gdstk::Label* vss_label_bottom = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vss_label_bottom->init("VSS");
        vss_label_bottom->origin = {sramcol_x_offset + vss_x_offset, vss_y_bottom};
        vss_label_bottom->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
        vss_label_bottom->magnification = 0.02;
        array_cell->label_array.append(vss_label_bottom);
        
        // VSS pin (頂部) 
        gdstk::Label* vss_label_top = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vss_label_top->init("VSS");
        vss_label_top->origin = {sramcol_x_offset + vss_x_offset, vss_y_top};
        vss_label_top->tag = gdstk::make_tag(m2_pin_layer->layer_number, m2_pin_layer->datatype);
        vss_label_top->magnification = 0.02;
        array_cell->label_array.append(vss_label_top);
        
        LOGI << "  Added VDD and VSS pins on M2 pin layer";
    }

    sram_cells_.array_cell = array_cell;
    
    LOGI << "Created SRAM array '" << array_name << "'";
    LOGI << "  Rows: " << num_rows;
    LOGI << "  Total size: " << total_width << " x " << total_height;
    
    return true;
}

bool LayoutGenerator::create_colgrp() {
    if (sram_cells_.array_cell == nullptr || sram_cells_.cgedge == nullptr || sram_cells_.io_colgrp == nullptr) {
        LOGE << "Invalid parameters for create_colgrp";
        return false;
    }
    
    // 取得各 cell 尺寸
    OpenFinRAM::CellSize array_size = OpenFinRAM::get_cell_size(sram_cells_.array_cell, layer_map_);
    OpenFinRAM::CellSize filler_size = OpenFinRAM::get_cell_size(sram_cells_.cgedge, layer_map_);
    OpenFinRAM::CellSize io_size = OpenFinRAM::get_cell_size(sram_cells_.io_colgrp, layer_map_);
    
    if (!array_size.valid) {
        gdstk::Vec2 bb_min, bb_max;
        sram_cells_.array_cell->bounding_box(bb_min, bb_max);
        array_size.min = bb_min;
        array_size.max = bb_max;
        array_size.width = bb_max.x - bb_min.x;
        array_size.height = bb_max.y - bb_min.y;
        array_size.valid = true;
    }
    
    if (!filler_size.valid) {
        gdstk::Vec2 bb_min, bb_max;
        sram_cells_.cgedge->bounding_box(bb_min, bb_max);
        filler_size.min = bb_min;
        filler_size.max = bb_max;
        filler_size.width = bb_max.x - bb_min.x;
        filler_size.height = bb_max.y - bb_min.y;
        filler_size.valid = true;
    }
    
    if (!io_size.valid) {
        gdstk::Vec2 bb_min, bb_max;
        sram_cells_.io_colgrp->bounding_box(bb_min, bb_max);
        io_size.min = bb_min;
        io_size.max = bb_max;
        io_size.width = bb_max.x - bb_min.x;
        io_size.height = bb_max.y - bb_min.y;
        io_size.valid = true;
    }
    
    int bits = cli_options_.num_data_bits;
    LOGI << "Creating column group with " << bits << " bits per array";
    LOGI << "  SRAM array size: " << array_size.width << " x " << array_size.height;
    LOGI << "  FILLER_cgedge size: " << filler_size.width << " x " << filler_size.height;
    LOGI << "  io_colgrp size: " << io_size.width << " x " << io_size.height;
    
    // 建立 cell 名稱: colgrp_x{num_wls}x{num_data_bits/2}
    char colgrp_name[64];
    snprintf(colgrp_name, sizeof(colgrp_name), "colgrp_x%dx%d", cli_options_.num_wls, cli_options_.num_data_bits / 2);
    
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
        ref->init(sram_cells_.cgedge);
        ref->origin = {current_x - filler_size.min.x, 0.0 - filler_size.min.y};
        ref->magnification = 1.0;
        colgrp_cell->reference_array.append(ref);
        LOGI << "  Added left FILLER_cgedge at x=" << current_x;
        current_x += filler_size.width;
    }
    
    // ========================================================================
    // 2. 放置左側 array_x4_x{bit} (正常放置)
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(sram_cells_.array_cell);
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
        ref->init(sram_cells_.io_colgrp);
        ref->origin = {current_x - io_size.min.x, 0.0 - io_size.min.y + 0.0135};
        ref->magnification = 1.0;
        colgrp_cell->reference_array.append(ref);
        LOGI << "  Added io_colgrp at x=" << current_x;
        current_x += io_size.width;
    }
    
    // ========================================================================
    // 4. 放置右側 array_x4_x{bit} (Y軸翻轉)
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(sram_cells_.array_cell);
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
        ref->init(sram_cells_.cgedge);
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
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map_);
    colgrp_cell->polygon_array.append(boundary);
    
    // ========================================================================
    // 加入 WL Pins for colgrp_x{bit*2}x4
    // 左側 array: WLT[bits-1:0]
    // 右側 array: WLB[bits-1:0]
    // ========================================================================
    const OpenFinRAM::LayerDef* m3_pin_layer = layer_map_.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
    
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
        
        // 左側 array: WLT[num_wls-1:0]
        LOGI << "  Adding " << cli_options_.num_wls << " WLT pins (WLT[0] to WLT[" << (cli_options_.num_wls-1) << "])";
        LOGI << "  Left sramcol starts at x=" << left_sramcol_x;
        for (int i = 0; i < cli_options_.num_wls; ++i) {
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
            label->magnification = 0.02;
            colgrp_cell->label_array.append(label);
        }
        
        // 右側 array (Y軸翻轉): WLB[num_wls-1:0]
        // Y軸翻轉 = rotation 180° + x_reflection
        // 翻轉後，原本在左側的 bitcell 變到右側
        LOGI << "  Adding " << cli_options_.num_wls << " WLB pins (WLB[0] to WLB[" << (cli_options_.num_wls-1) << "])";
        LOGI << "  Right sramcol starts at x=" << right_sramcol_x;
        for (int i = 0; i < cli_options_.num_wls; ++i) {
            // 右側 array Y軸翻轉後
            // sramcol 寬度（不含 dummy）= num_wls * cell_width + dummy_sram_width + tapcell_width
            // 但 WL 只在 bitcell 區域，所以是 num_wls * cell_width
            const double sramcol_bitcell_width = cli_options_.num_wls * cell_width;
            
            // 原本 bitcell i 在相對座標 (i * cell_width + cell_width/2, 0)
            // Y軸翻轉後：x' = sramcol_bitcell_width - (i+0.5) * cell_width
            double wl_x = right_sramcol_x + sramcol_bitcell_width - (i + 0.5) * cell_width;
            
            if (i == 0 || i == cli_options_.num_wls - 1) {
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
            label->magnification = 0.02;
            colgrp_cell->label_array.append(label);
        }
        
        LOGI << "  Added " << (cli_options_.num_wls * 2) << " WL pins on M3 pin layer (30, 251)";
    }
    
    // ========================================================================
    // 加入 iocolgrp 的信號 Pins
    // blprechtn, blprechbn, yselt[3:0], yseltn[3:0], yselb[3:0], yselbn[3:0]
    // 這些信號在 iocolgrp_sram_6t122_v2 中，需要加上 iocolgrp 的 x 偏移
    // ========================================================================
    if (m3_pin_layer != nullptr && sram_cells_.io_colgrp != nullptr) {
        LOGI << "  Adding iocolgrp signal pins";
        
        // iocolgrp 在 colgrp 中的 x 起始位置
        const double io_x_offset = filler_size.width + array_size.width;
        
        // 從 iocolgrp cell 中取得所有 M3 pin layer 的 labels
        for (uint64_t i = 0; i < sram_cells_.io_colgrp->label_array.count; ++i) {
            gdstk::Label* orig_label = sram_cells_.io_colgrp->label_array[i];
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
                    label->magnification = 0.02;
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
    const OpenFinRAM::LayerDef* m3_drawing_layer = layer_map_.get_layer("M3", OpenFinRAM::LayerPurpose::Drawing);
    
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
    
    sram_cells_.colgrp = colgrp_cell;

    LOGI << "Created column group '" << colgrp_name << "'";
    LOGI << "  Total size: " << total_width << " x " << cell_height;
    
    return true;
}

bool LayoutGenerator::create_stacked_colgrp() {
    int num_rows = cli_options_.num_data_bits / 2;
    if (sram_cells_.colgrp == nullptr || num_rows == 0) {
        LOGE << "Invalid parameters for create_stacked_colgrp";
        return false;
    }
    
    // 取得 colgrp 尺寸
    OpenFinRAM::CellSize colgrp_size = OpenFinRAM::get_cell_size(sram_cells_.colgrp, layer_map_);
    
    if (!colgrp_size.valid) {
        LOGW << "Cannot get colgrp size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        sram_cells_.colgrp->bounding_box(bb_min, bb_max);
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
    char stacked_name[64];
    snprintf(stacked_name, sizeof(stacked_name), "stacked_colgrp_x%dx%lu", cli_options_.num_wls, cli_options_.num_data_bits / 2);
    gdstk::Cell* stacked_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    stacked_cell->init(stacked_name);
    
    // 垂直堆疊 colgrp
    // 使用 repetition 來高效堆疊
    gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
    ref->init(sram_cells_.colgrp);
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
    const OpenFinRAM::LayerDef* m3_pin_layer = layer_map_.get_layer("M3", OpenFinRAM::LayerPurpose::Pin);
    
    if (m3_pin_layer != nullptr) {
        // VDD pin - x=0.125, y=0
        gdstk::Label* vdd_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vdd_label->init("VDD");
        vdd_label->origin = {0.125, 0.0};
        vdd_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
        vdd_label->magnification = 0.02;
        stacked_cell->label_array.append(vdd_label);
        
        LOGI << "  Added VDD pin at (" << vdd_label->origin.x << ", " << vdd_label->origin.y 
             << ") on M3 pin layer";
        
        // VSS pin - x=0.162, y=0
        gdstk::Label* vss_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
        vss_label->init("VSS");
        vss_label->origin = {0.162, 0.0};
        vss_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
        vss_label->magnification = 0.02;
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
        for (uint64_t i = 0; i < sram_cells_.colgrp->label_array.count; ++i) {
            gdstk::Label* orig_label = sram_cells_.colgrp->label_array[i];
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
                    new_label->magnification = 0.02;
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
        
        for (uint64_t i = 0; i < sram_cells_.colgrp->label_array.count; ++i) {
            gdstk::Label* orig_label = sram_cells_.colgrp->label_array[i];
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
                d_label->magnification = 0.02;
                stacked_cell->label_array.append(d_label);
                
                // 加入 Q[i]
                char q_name[32];
                snprintf(q_name, sizeof(q_name), "Q[%lu]", (unsigned long)row);
                gdstk::Label* q_label = (gdstk::Label*)gdstk::allocate_clear(sizeof(gdstk::Label));
                q_label->init(q_name);
                q_label->origin = {q_x, q_y + y_offset};
                q_label->tag = gdstk::make_tag(m3_pin_layer->layer_number, m3_pin_layer->datatype);
                q_label->magnification = 0.02;
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
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map_);
    stacked_cell->polygon_array.append(boundary);
    
    sram_cells_.stacked_colgrp = stacked_cell;
    LOGI << "Created stacked column group '" << stacked_name << "'";
    LOGI << "  Total size: " << colgrp_width << " x " << total_height;
    
    return true;
}

bool LayoutGenerator::create_muxed_colgrp() {
    if (sram_cells_.stacked_colgrp == nullptr || cli_options_.num_banks == 0) {
        LOGE << "Invalid parameters for create_muxed_colgrp";
        return false;
    }

    // 取得 stacked_colgrp 尺寸
    OpenFinRAM::CellSize stacked_size = OpenFinRAM::get_cell_size(sram_cells_.stacked_colgrp, layer_map_);

    if (!stacked_size.valid) {
        LOGW << "Cannot get stacked_colgrp size from BOUNDARY, using bounding box";
        gdstk::Vec2 bb_min, bb_max;
        sram_cells_.stacked_colgrp->bounding_box(bb_min, bb_max);
        stacked_size.min = bb_min;
        stacked_size.max = bb_max;
        stacked_size.width = bb_max.x - bb_min.x;
        stacked_size.height = bb_max.y - bb_min.y;
        stacked_size.valid = true;
    }

    double stacked_width = stacked_size.width;
    double stacked_height = stacked_size.height;

    LOGI << "Creating muxed column group with " << cli_options_.num_banks << " columns";
    LOGI << "  stacked_colgrp size: " << stacked_width << " x " << stacked_height;

    // 建立新的 Cell
    char muxed_name[96];
    snprintf(muxed_name, sizeof(muxed_name), "stacked_colgrp_x%dx%lux%lu", cli_options_.num_wls, cli_options_.num_data_bits / 2, cli_options_.num_banks);
    gdstk::Cell* muxed_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    muxed_cell->init(muxed_name);

    // 水平堆疊 stacked_colgrp
    gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
    ref->init(sram_cells_.stacked_colgrp);
    ref->origin = {0.0 - stacked_size.min.x, 0.0 - stacked_size.min.y};
    ref->magnification = 1.0;

    if (cli_options_.num_banks > 1) {
        ref->repetition.type = gdstk::RepetitionType::Rectangular;
        ref->repetition.columns = cli_options_.num_banks;
        ref->repetition.rows = 1;
        ref->repetition.spacing = {stacked_width, 0.0};  // 水平方向間距為 stacked_colgrp 寬度
    }

    muxed_cell->reference_array.append(ref);

    double total_width = cli_options_.num_banks * stacked_width;

    // ========================================================================
    // 加入 BOUNDARY
    // ========================================================================
    gdstk::Vec2 boundary_min = {0.0, 0.0};
    gdstk::Vec2 boundary_max = {total_width, stacked_height};
    gdstk::Polygon* boundary = OpenFinRAM::create_boundary_polygon(boundary_min, boundary_max, layer_map_);
    muxed_cell->polygon_array.append(boundary);

    sram_cells_.muxed_colgrp = muxed_cell;

    LOGI << "Created muxed column group '" << muxed_name << "'";
    LOGI << "  Total size: " << total_width << " x " << stacked_height;

    return true;
}

bool LayoutGenerator::write_layout() {
    gdstk::Library output_sram_lib = {};
    output_sram_lib.init("SRAM_LIB", sram_lib.unit, sram_lib.precision);

    const char* sram_output = "sram_array_test.gds";
    LOGD << "Writing SRAM library to: " << sram_output;
    
    add_cell_with_deps(output_sram_lib, sram_cells_.bitcell);
    add_cell_with_deps(output_sram_lib, sram_cells_.dummy);
    add_cell_with_deps(output_sram_lib, sram_cells_.tapcell);
    add_cell_with_deps(output_sram_lib, sram_cells_.dummy_v1);
    add_cell_with_deps(output_sram_lib, sram_cells_.dummy_v2);
    add_cell_with_deps(output_sram_lib, sram_cells_.cgedge);
    add_cell_with_deps(output_sram_lib, sram_cells_.io_colgrp);
    add_cell_with_deps(output_sram_lib, sram_cells_.filler);

    add_cell_with_deps(output_sram_lib, sram_cells_.sram_column);
    add_cell_with_deps(output_sram_lib, sram_cells_.array_cell);
    add_cell_with_deps(output_sram_lib, sram_cells_.colgrp);
    add_cell_with_deps(output_sram_lib, sram_cells_.stacked_colgrp);
    add_cell_with_deps(output_sram_lib, sram_cells_.muxed_colgrp);
    
    gdstk::ErrorCode error_code = output_sram_lib.write_gds(sram_output, 0, nullptr);
    
    if (error_code != gdstk::ErrorCode::NoError) {
        LOGE << "Error writing SRAM GDS!";
        return false;
    }
    
    LOGI << "Successfully created SRAM library: " << sram_output;
    return true;
}

void LayoutGenerator::add_cell_with_deps(gdstk::Library& target_lib, gdstk::Cell* source_cell) {
    if (source_cell == nullptr) return;

    // 1. 加入 cell 本身
    if (target_lib.get_cell(source_cell->name) == nullptr) {
        gdstk::Cell* cell_copy = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
        
        // 【修正重點】這裡必須設為 false
        // 因為我們稍後會手動處理相依性，且傳遞 nullptr 給 copy_from 會導致遞迴時崩潰
        cell_copy->copy_from(*source_cell, nullptr, false); 
        
        target_lib.cell_array.append(cell_copy);
    }

    gdstk::Map<gdstk::Cell*> deps = {};
    source_cell->get_dependencies(true, deps);
    for (auto* item = deps.next(nullptr); item != nullptr; item = deps.next(item)) {
        LOGD << "  Cell '" << source_cell->name << "' depends on '" << item->value->name << "'";
        if (target_lib.get_cell(item->value->name) == nullptr) {
            gdstk::Cell* dep_copy = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
            dep_copy->copy_from(*item->value, nullptr, true);
            target_lib.cell_array.append(dep_copy);
        }
    }
    deps.clear();
}

bool LayoutGenerator::gen_layout() {
    LOGD << "Generating layout...";

    if (!load_sram_gds()) {
        LOGE << "Failed to load SRAM GDS!";
        return false;
    }
    if (!extract_required_cells()) {
        LOGE << "Failed to extract required cells!";
        return false;
    }
    if (!create_sram_column()) {
        LOGE << "Failed to create SRAM column!";
        return false;
    }
    if (!create_sram_array()) {
        LOGE << "Failed to create SRAM array!";
        return false;
    }
    if (!create_colgrp()) {
        LOGE << "Failed to create column group!";
        return false;
    }
    if (!create_stacked_colgrp()) {
        LOGE << "Failed to create stacked column group!";
        return false;
    }
    if (!create_muxed_colgrp()) {
        LOGE << "Failed to create muxed column group!";
        return false;
    }
    if (!write_layout()) {
        LOGE << "Failed to write layout!";
        return false;
    }

    return true;
}