/*
 * OpenFinRAM - Cell Utilities Implementation
 * 
 * This file implements utilities for creating and manipulating GDS cells.
 */

#include "cell_utils.hpp"

#include <plog/Log.h>

#include <cfloat>

namespace OpenFinRAM {

// ============================================================================
// PlacementConfig 實作
// ============================================================================

PlacementConfig::PlacementConfig()
    : count(1)
    , direction(Direction::Horizontal)
    , spacing(0.0)
    , origin{0.0, 0.0}
    , mirror_x(false)
    , rotation(0.0)
    , magnification(1.0)
{}

// ============================================================================
// CellSize 實作
// ============================================================================

CellSize::CellSize()
    : width(0)
    , height(0)
    , min{0, 0}
    , max{0, 0}
    , valid(false)
{}

// ============================================================================
// Cell 尺寸計算函數實作
// ============================================================================

CellSize get_cell_size_from_boundary(const gdstk::Cell* cell, const LayerMap& layer_map) {
    CellSize size;
    
    if (cell == nullptr) {
        return size;
    }
    
    gdstk::Tag boundary_tag = layer_map.boundary_tag();
    
    // 使用 get_polygons 遞迴獲取所有 polygon（包括 reference 中的）
    // apply_repetitions=true: 展開重複
    // include_paths=false: 不包含路徑
    // depth=-1: 無限遞迴深度
    // filter=true: 只獲取特定 tag
    gdstk::Array<gdstk::Polygon*> all_polygons = {};
    cell->get_polygons(true, false, -1, true, boundary_tag, all_polygons);
    
    bool found_boundary = false;
    gdstk::Vec2 boundary_min = {DBL_MAX, DBL_MAX};
    gdstk::Vec2 boundary_max = {-DBL_MAX, -DBL_MAX};
    
    // 遍歷所有獲取到的 boundary polygon
    for (uint64_t i = 0; i < all_polygons.count; i++) {
        gdstk::Polygon* polygon = all_polygons[i];
        
        found_boundary = true;
        
        // 取得這個 polygon 的 bounding box
        gdstk::Vec2 pmin, pmax;
        polygon->bounding_box(pmin, pmax);
        
        // 更新整體邊界
        if (pmin.x < boundary_min.x) boundary_min.x = pmin.x;
        if (pmin.y < boundary_min.y) boundary_min.y = pmin.y;
        if (pmax.x > boundary_max.x) boundary_max.x = pmax.x;
        if (pmax.y > boundary_max.y) boundary_max.y = pmax.y;
    }
    
    // 清理臨時創建的 polygon（get_polygons 會創建副本）
    for (uint64_t i = 0; i < all_polygons.count; i++) {
        all_polygons[i]->clear();
        gdstk::free_allocation(all_polygons[i]);
    }
    all_polygons.clear();
    
    if (found_boundary) {
        size.min = boundary_min;
        size.max = boundary_max;
        size.width = boundary_max.x - boundary_min.x;
        size.height = boundary_max.y - boundary_min.y;
        size.valid = true;
        LOGI << "Found BOUNDARY layer for cell '" << cell->name << "'";
    } else {
        // 回退：使用整體 bounding box
        LOGW << "No BOUNDARY layer found for cell '" << cell->name 
             << "', using overall bounding box";
        cell->bounding_box(size.min, size.max);
        
        if (size.min.x <= size.max.x) {
            size.width = size.max.x - size.min.x;
            size.height = size.max.y - size.min.y;
            size.valid = true;
        }
    }
    
    return size;
}

CellSize get_cell_size(const gdstk::Cell* cell, const LayerMap& layer_map) {
    return get_cell_size_from_boundary(cell, layer_map);
}

// ============================================================================
// Polygon 建立函數實作
// ============================================================================

gdstk::Polygon* create_boundary_polygon(gdstk::Vec2 min, gdstk::Vec2 max, 
                                         const LayerMap& layer_map) {
    gdstk::Polygon* boundary = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    boundary->tag = layer_map.boundary_tag();
    
    // 建立矩形的四個頂點（逆時針方向）
    boundary->point_array.ensure_slots(4);
    boundary->point_array.append({min.x, min.y});
    boundary->point_array.append({max.x, min.y});
    boundary->point_array.append({max.x, max.y});
    boundary->point_array.append({min.x, max.y});
    
    return boundary;
}

// ============================================================================
// Cell Array 建立函數實作
// ============================================================================

gdstk::Cell* create_cell_array(gdstk::Cell* source_cell,
                                const char* new_cell_name,
                                const PlacementConfig& config,
                                const LayerMap& layer_map) {
    if (source_cell == nullptr || new_cell_name == nullptr || config.count == 0) {
        LOGE << "Invalid parameters for create_cell_array";
        return nullptr;
    }
    
    // 取得來源 cell 的尺寸
    CellSize size = get_cell_size(source_cell, layer_map);
    if (!size.valid) {
        LOGE << "Cannot get bounding box for cell: " << source_cell->name;
        return nullptr;
    }
    
    LOGI << "Source cell '" << source_cell->name << "' size: " 
         << size.width << " x " << size.height;
    
    // 建立新的 Cell
    gdstk::Cell* new_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    new_cell->init(new_cell_name);
    
    // 計算每個 instance 的間距
    double step_x = 0.0;
    double step_y = 0.0;
    
    if (config.direction == Direction::Horizontal) {
        step_x = size.width + config.spacing;
    } else {
        step_y = size.height + config.spacing;
    }
    
    // 使用 Repetition（更有效率，適合大量重複）
    if (config.count > 1) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(source_cell);
        
        // 設定起始位置（考慮 bounding box 的 offset）
        ref->origin = {
            config.origin.x - size.min.x,
            config.origin.y - size.min.y
        };
        ref->rotation = config.rotation;
        ref->magnification = config.magnification;
        ref->x_reflection = config.mirror_x;
        
        // 設定 Repetition
        ref->repetition.type = gdstk::RepetitionType::Rectangular;
        if (config.direction == Direction::Horizontal) {
            ref->repetition.columns = config.count;
            ref->repetition.rows = 1;
            ref->repetition.spacing = {step_x, 0.0};
        } else {
            ref->repetition.columns = 1;
            ref->repetition.rows = config.count;
            ref->repetition.spacing = {0.0, step_y};
        }
        
        new_cell->reference_array.append(ref);
        
        LOGI << "Created cell array with " << config.count << " instances using Repetition";
    } else {
        // 只有一個 instance
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(source_cell);
        ref->origin = {
            config.origin.x - size.min.x,
            config.origin.y - size.min.y
        };
        ref->rotation = config.rotation;
        ref->magnification = config.magnification;
        ref->x_reflection = config.mirror_x;
        
        new_cell->reference_array.append(ref);
        
        LOGI << "Created single cell instance";
    }
    
    // ========================================================================
    // 計算並加入整體的 BOUNDARY polygon
    // ========================================================================
    double total_width, total_height;
    if (config.direction == Direction::Horizontal) {
        total_width = size.width * config.count + config.spacing * (config.count - 1);
        total_height = size.height;
    } else {
        total_width = size.width;
        total_height = size.height * config.count + config.spacing * (config.count - 1);
    }
    
    gdstk::Vec2 boundary_min = config.origin;
    gdstk::Vec2 boundary_max = {
        config.origin.x + total_width,
        config.origin.y + total_height
    };
    
    gdstk::Polygon* boundary = create_boundary_polygon(boundary_min, boundary_max, layer_map);
    new_cell->polygon_array.append(boundary);
    
    LOGI << "Added BOUNDARY layer: (" << boundary_min.x << ", " << boundary_min.y 
         << ") to (" << boundary_max.x << ", " << boundary_max.y << ")";
    
    return new_cell;
}

gdstk::Cell* create_cell_array_individual(gdstk::Cell* source_cell,
                                           const char* new_cell_name,
                                           const std::vector<gdstk::Vec2>& positions,
                                           const LayerMap& layer_map,
                                           double rotation,
                                           double magnification,
                                           bool mirror_x) {
    if (source_cell == nullptr || new_cell_name == nullptr || positions.empty()) {
        LOGE << "Invalid parameters for create_cell_array_individual";
        return nullptr;
    }
    
    CellSize size = get_cell_size(source_cell, layer_map);
    if (!size.valid) {
        LOGE << "Cannot get bounding box for cell: " << source_cell->name;
        return nullptr;
    }
    
    gdstk::Cell* new_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    new_cell->init(new_cell_name);
    
    for (const auto& pos : positions) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(source_cell);
        ref->origin = {pos.x - size.min.x, pos.y - size.min.y};
        ref->rotation = rotation;
        ref->magnification = magnification;
        ref->x_reflection = mirror_x;
        
        new_cell->reference_array.append(ref);
    }
    
    LOGI << "Created " << positions.size() << " individual cell instances";
    return new_cell;
}

// ============================================================================
// Library 輔助函數實作
// ============================================================================

void add_cell_with_dependencies(gdstk::Library& lib, 
                                 gdstk::Cell* cell, 
                                 gdstk::Cell* source_cell) {
    // 加入主要 cell
    lib.cell_array.append(cell);
    
    // 取得並複製所有相依 cells
    gdstk::Map<gdstk::Cell*> deps = {};
    source_cell->get_dependencies(true, deps);
    
    for (auto* item = deps.next(nullptr); item != nullptr; item = deps.next(item)) {
        // 檢查是否已經存在於 library 中
        if (lib.get_cell(item->value->name) == nullptr) {
            gdstk::Cell* dep_copy = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
            dep_copy->copy_from(*item->value, nullptr, true);
            lib.cell_array.append(dep_copy);
        }
    }
    deps.clear();
}

}  // namespace OpenFinRAM
