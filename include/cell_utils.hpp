/*
 * OpenFinRAM - Cell Utilities
 * 
 * This file provides utilities for creating and manipulating GDS cells,
 * including cell arrays and placement functions.
 */

#ifndef OPENFINRAM_CELL_UTILS_HPP
#define OPENFINRAM_CELL_UTILS_HPP

#include "layermap.hpp"

#include <gdstk/gdstk.hpp>

#include <cstdint>
#include <vector>

namespace OpenFinRAM {

// ============================================================================
// 方向列舉
// ============================================================================
enum class Direction {
    Horizontal,  // 沿 X 軸排列
    Vertical     // 沿 Y 軸排列
};

// ============================================================================
// Cell 擺放配置結構
// ============================================================================
struct PlacementConfig {
    uint64_t count;              // 擺放數量
    Direction direction;         // 擺放方向
    double spacing;              // 額外間距（預設為 0，即緊密排列）
    gdstk::Vec2 origin;         // 起始位置
    bool mirror_x;              // X 鏡射
    double rotation;            // 旋轉角度（弧度）
    double magnification;       // 放大倍率
    
    // 建構子提供預設值
    PlacementConfig();
};

// ============================================================================
// Cell 尺寸結構（基於 BOUNDARY layer）
// ============================================================================
struct CellSize {
    double width;
    double height;
    gdstk::Vec2 min;
    gdstk::Vec2 max;
    bool valid;
    
    CellSize();
};

// ============================================================================
// Cell 尺寸計算函數
// ============================================================================

/**
 * @brief 從 BOUNDARY layer 取得 Cell 的真實邊界
 * 
 * 如果沒有 BOUNDARY layer，則回退到使用整體 bounding box
 * 
 * @param cell 要取得尺寸的 cell
 * @param layer_map 用於查詢 BOUNDARY layer 的 layer map
 * @return CellSize 包含 cell 尺寸資訊的結構
 */
CellSize get_cell_size_from_boundary(const gdstk::Cell* cell, const LayerMap& layer_map);

/**
 * @brief 取得 Cell 尺寸（別名函數，向後相容）
 */
CellSize get_cell_size(const gdstk::Cell* cell, const LayerMap& layer_map);

// ============================================================================
// Polygon 建立函數
// ============================================================================

/**
 * @brief 建立 BOUNDARY polygon（矩形）
 * 
 * @param min 左下角座標
 * @param max 右上角座標
 * @param layer_map 用於取得 BOUNDARY layer tag 的 layer map
 * @return gdstk::Polygon* 新建立的 polygon（需要由呼叫者管理記憶體）
 */
gdstk::Polygon* create_boundary_polygon(gdstk::Vec2 min, gdstk::Vec2 max, 
                                         const LayerMap& layer_map);

// ============================================================================
// Cell Array 建立函數
// ============================================================================

/**
 * @brief 建立一個包含重複擺放 cell 的新 Cell
 * 
 * 使用 gdstk 的 Repetition 功能，一個 Reference 即可表示多個重複的 cell，
 * 非常有效率，適合大量重複擺放。
 * 
 * @param source_cell 要擺放的來源 cell
 * @param new_cell_name 新建立的 cell 名稱
 * @param config 擺放配置
 * @param layer_map 用於取得 layer 資訊的 layer map
 * @return gdstk::Cell* 新建立的 Cell 指標（需要由呼叫者負責記憶體管理），
 *         如果失敗則回傳 nullptr
 */
gdstk::Cell* create_cell_array(gdstk::Cell* source_cell,
                                const char* new_cell_name,
                                const PlacementConfig& config,
                                const LayerMap& layer_map);

/**
 * @brief 建立多個獨立的 Reference（較彈性，每個可以有不同屬性）
 * 
 * @param source_cell 要擺放的來源 cell
 * @param new_cell_name 新建立的 cell 名稱
 * @param positions 每個 instance 的位置
 * @param layer_map 用於取得 layer 資訊的 layer map
 * @param rotation 旋轉角度（弧度），預設為 0
 * @param magnification 放大倍率，預設為 1.0
 * @param mirror_x X 鏡射，預設為 false
 * @return gdstk::Cell* 新建立的 Cell 指標，如果失敗則回傳 nullptr
 */
gdstk::Cell* create_cell_array_individual(gdstk::Cell* source_cell,
                                           const char* new_cell_name,
                                           const std::vector<gdstk::Vec2>& positions,
                                           const LayerMap& layer_map,
                                           double rotation = 0.0,
                                           double magnification = 1.0,
                                           bool mirror_x = false);

// ============================================================================
// Library 輔助函數
// ============================================================================

/**
 * @brief 將 cell 及其所有相依 cells 加入 Library
 * 
 * @param lib 目標 Library
 * @param cell 要加入的 cell
 * @param source_cell 來源 cell（用於取得相依關係）
 */
void add_cell_with_dependencies(gdstk::Library& lib, 
                                 gdstk::Cell* cell, 
                                 gdstk::Cell* source_cell);

}  // namespace OpenFinRAM

#endif  // OPENFINRAM_CELL_UTILS_HPP
