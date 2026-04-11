/*
 * OpenFinRAM - ASAP7 Layer Map Parser Implementation
 * 
 * This file implements the parser for ASAP7 PDK layer map files.
 */

#include "layermap.hpp"

#include <plog/Log.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace OpenFinRAM {

// ============================================================================
// LayerPurpose 輔助函數
// ============================================================================

const char* purpose_to_string(LayerPurpose purpose) {
    switch (purpose) {
        case LayerPurpose::Drawing:  return "drawing";
        case LayerPurpose::Pin:      return "pin";
        case LayerPurpose::Label:    return "label";
        case LayerPurpose::Net:      return "net";
        case LayerPurpose::Blockage: return "blockage";
        default:                     return "unknown";
    }
}

LayerPurpose string_to_purpose(const std::string& str) {
    // 轉換為小寫進行比較
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "drawing")  return LayerPurpose::Drawing;
    if (lower == "pin")      return LayerPurpose::Pin;
    if (lower == "label")    return LayerPurpose::Label;
    if (lower == "net")      return LayerPurpose::Net;
    if (lower == "blockage") return LayerPurpose::Blockage;
    
    return LayerPurpose::Unknown;
}

// ============================================================================
// LayerMap 實作
// ============================================================================

void LayerMap::init_asap7_layermap() {
    clear();
    
    // Drawing layers
    add_layer(LayerDef("well", LayerPurpose::Drawing, 1, 0));
    add_layer(LayerDef("fin", LayerPurpose::Drawing, 2, 0));
    add_layer(LayerDef("Gate", LayerPurpose::Drawing, 7, 0));
    add_layer(LayerDef("Dummy", LayerPurpose::Drawing, 8, 0));
    add_layer(LayerDef("GCut", LayerPurpose::Drawing, 10, 0));
    add_layer(LayerDef("Active", LayerPurpose::Drawing, 11, 0));
    add_layer(LayerDef("Nselect", LayerPurpose::Drawing, 12, 0));
    add_layer(LayerDef("Pselect", LayerPurpose::Drawing, 13, 0));
    add_layer(LayerDef("LIG", LayerPurpose::Drawing, 16, 0));
    add_layer(LayerDef("LISD", LayerPurpose::Drawing, 17, 0));
    add_layer(LayerDef("V0", LayerPurpose::Drawing, 18, 0));
    add_layer(LayerDef("M1", LayerPurpose::Drawing, 19, 0));
    add_layer(LayerDef("M2", LayerPurpose::Drawing, 20, 0));
    add_layer(LayerDef("V1", LayerPurpose::Drawing, 21, 0));
    add_layer(LayerDef("V2", LayerPurpose::Drawing, 25, 0));
    add_layer(LayerDef("M3", LayerPurpose::Drawing, 30, 0));
    add_layer(LayerDef("V3", LayerPurpose::Drawing, 35, 0));
    add_layer(LayerDef("M4", LayerPurpose::Drawing, 40, 0));
    add_layer(LayerDef("V4", LayerPurpose::Drawing, 45, 0));
    add_layer(LayerDef("M5", LayerPurpose::Drawing, 50, 0));
    add_layer(LayerDef("V5", LayerPurpose::Drawing, 55, 0));
    add_layer(LayerDef("M6", LayerPurpose::Drawing, 60, 0));
    add_layer(LayerDef("V6", LayerPurpose::Drawing, 65, 0));
    add_layer(LayerDef("M7", LayerPurpose::Drawing, 70, 0));
    add_layer(LayerDef("V7", LayerPurpose::Drawing, 75, 0));
    add_layer(LayerDef("M8", LayerPurpose::Drawing, 80, 0));
    add_layer(LayerDef("V8", LayerPurpose::Drawing, 85, 0));
    add_layer(LayerDef("SDT", LayerPurpose::Drawing, 88, 0));
    add_layer(LayerDef("M9", LayerPurpose::Drawing, 90, 0));
    add_layer(LayerDef("V9", LayerPurpose::Drawing, 95, 0));
    add_layer(LayerDef("SLVT", LayerPurpose::Drawing, 97, 0));
    add_layer(LayerDef("LVT", LayerPurpose::Drawing, 98, 0));
    add_layer(LayerDef("SRAMDRC", LayerPurpose::Drawing, 99, 0));
    add_layer(LayerDef("BOUNDARY", LayerPurpose::Drawing, 100, 0));
    add_layer(LayerDef("TEXT", LayerPurpose::Drawing, 101, 0));
    add_layer(LayerDef("SRAMVT", LayerPurpose::Drawing, 110, 0));
    
    // Pin layers
    add_layer(LayerDef("well", LayerPurpose::Pin, 1, 251));
    add_layer(LayerDef("P_SUB", LayerPurpose::Pin, 3, 251));
    add_layer(LayerDef("Gate", LayerPurpose::Pin, 7, 251));
    add_layer(LayerDef("LIG", LayerPurpose::Pin, 16, 251));
    add_layer(LayerDef("LISD", LayerPurpose::Pin, 17, 251));
    add_layer(LayerDef("M1", LayerPurpose::Pin, 19, 251));
    add_layer(LayerDef("M2", LayerPurpose::Pin, 20, 251));
    add_layer(LayerDef("M3", LayerPurpose::Pin, 30, 251));
    add_layer(LayerDef("M4", LayerPurpose::Pin, 40, 251));
    add_layer(LayerDef("M5", LayerPurpose::Pin, 50, 251));
    add_layer(LayerDef("M6", LayerPurpose::Pin, 60, 251));
    add_layer(LayerDef("M7", LayerPurpose::Pin, 70, 251));
    add_layer(LayerDef("M8", LayerPurpose::Pin, 80, 251));
    add_layer(LayerDef("M9", LayerPurpose::Pin, 90, 251));
    
    // Label layers
    add_layer(LayerDef("Gate", LayerPurpose::Label, 7, 2));
    add_layer(LayerDef("LIG", LayerPurpose::Label, 16, 2));
    add_layer(LayerDef("LISD", LayerPurpose::Label, 17, 2));
    add_layer(LayerDef("M1", LayerPurpose::Label, 19, 2));
    add_layer(LayerDef("M2", LayerPurpose::Label, 20, 2));
    add_layer(LayerDef("M3", LayerPurpose::Label, 30, 2));
    add_layer(LayerDef("M4", LayerPurpose::Label, 40, 2));
    add_layer(LayerDef("M5", LayerPurpose::Label, 50, 2));
    add_layer(LayerDef("M6", LayerPurpose::Label, 60, 2));
    add_layer(LayerDef("M7", LayerPurpose::Label, 70, 2));
    add_layer(LayerDef("M8", LayerPurpose::Label, 80, 2));
    add_layer(LayerDef("M9", LayerPurpose::Label, 90, 2));
    
    // Net layers
    add_layer(LayerDef("Gate", LayerPurpose::Net, 7, 3));
    add_layer(LayerDef("LIG", LayerPurpose::Net, 16, 3));
    add_layer(LayerDef("LISD", LayerPurpose::Net, 17, 3));
    add_layer(LayerDef("M1", LayerPurpose::Net, 19, 3));
    add_layer(LayerDef("M2", LayerPurpose::Net, 20, 3));
    add_layer(LayerDef("M3", LayerPurpose::Net, 30, 3));
    add_layer(LayerDef("M4", LayerPurpose::Net, 40, 3));
    add_layer(LayerDef("M5", LayerPurpose::Net, 50, 3));
    add_layer(LayerDef("M6", LayerPurpose::Net, 60, 3));
    add_layer(LayerDef("M7", LayerPurpose::Net, 70, 3));
    add_layer(LayerDef("M8", LayerPurpose::Net, 80, 3));
    add_layer(LayerDef("M9", LayerPurpose::Net, 90, 3));
    
    // Blockage layers
    add_layer(LayerDef("LIG", LayerPurpose::Blockage, 16, 4));
    add_layer(LayerDef("LISD", LayerPurpose::Blockage, 17, 4));
    add_layer(LayerDef("V0", LayerPurpose::Blockage, 18, 4));
    add_layer(LayerDef("M1", LayerPurpose::Blockage, 19, 4));
    add_layer(LayerDef("M2", LayerPurpose::Blockage, 20, 4));
    add_layer(LayerDef("V1", LayerPurpose::Blockage, 21, 4));
    add_layer(LayerDef("V2", LayerPurpose::Blockage, 25, 4));
    add_layer(LayerDef("M3", LayerPurpose::Blockage, 30, 4));
    add_layer(LayerDef("V3", LayerPurpose::Blockage, 35, 4));
    add_layer(LayerDef("M4", LayerPurpose::Blockage, 40, 4));
    add_layer(LayerDef("V4", LayerPurpose::Blockage, 45, 4));
    add_layer(LayerDef("M5", LayerPurpose::Blockage, 50, 4));
    add_layer(LayerDef("V5", LayerPurpose::Blockage, 55, 4));
    add_layer(LayerDef("M6", LayerPurpose::Blockage, 60, 4));
    add_layer(LayerDef("V6", LayerPurpose::Blockage, 65, 4));
    add_layer(LayerDef("M7", LayerPurpose::Blockage, 70, 4));
    add_layer(LayerDef("V7", LayerPurpose::Blockage, 75, 4));
    add_layer(LayerDef("M8", LayerPurpose::Blockage, 80, 4));
    add_layer(LayerDef("V8", LayerPurpose::Blockage, 85, 4));
    add_layer(LayerDef("SDT", LayerPurpose::Blockage, 88, 4));
    add_layer(LayerDef("M9", LayerPurpose::Blockage, 90, 4));
    add_layer(LayerDef("V9", LayerPurpose::Blockage, 95, 4));
    
    LOGI << "Initialized ASAP7 layermap with " << layers_.size() << " layers";
}

LayerMap::LayerMap() {
}

LayerMap::~LayerMap() {
    clear();
}

void LayerMap::clear() {
    layers_.clear();
    layer_index_map_.clear();
    name_purpose_map_.clear();
}

LayerPurpose LayerMap::parse_purpose(const std::string& purpose_str) {
    return string_to_purpose(purpose_str);
}

bool LayerMap::parse_line(const std::string& line) {
    // 跳過空行
    if (line.empty()) {
        return true;
    }
    
    // 跳過註解行（以 # 開頭）
    size_t first_non_space = line.find_first_not_of(" \t");
    if (first_non_space == std::string::npos || line[first_non_space] == '#') {
        return true;
    }
    
    // 使用 stringstream 解析
    std::istringstream iss(line);
    std::string name, purpose_str;
    uint32_t layer_number, datatype;
    
    // 嘗試讀取四個欄位
    if (!(iss >> name >> purpose_str >> layer_number >> datatype)) {
        LOGW << "Failed to parse line: " << line;
        return false;
    }
    
    // 解析 purpose
    LayerPurpose purpose = parse_purpose(purpose_str);
    if (purpose == LayerPurpose::Unknown) {
        LOGW << "Unknown layer purpose: " << purpose_str << " for layer: " << name;
    }
    
    // 建立並新增 layer
    LayerDef layer(name, purpose, layer_number, datatype);
    add_layer(layer);
    
    return true;
}

void LayerMap::add_layer(const LayerDef& layer) {
    size_t index = layers_.size();
    layers_.push_back(layer);
    
    // 更新查詢 map
    auto number_key = std::make_pair(layer.layer_number, layer.datatype);
    layer_index_map_[number_key] = index;
    
    auto name_key = std::make_pair(layer.name, layer.purpose);
    name_purpose_map_[name_key] = index;
}

bool LayerMap::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        LOGE << "Failed to open layer map file: " << filename;
        return false;
    }
    
    LOGI << "Loading layer map from: " << filename;
    
    // 清除現有資料
    clear();
    
    std::string line;
    int line_number = 0;
    int parsed_count = 0;
    
    while (std::getline(file, line)) {
        line_number++;
        
        // 移除行尾的空白和換行
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || 
                                  line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        
        if (parse_line(line)) {
            // 只計算非空、非註解行
            size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line[first] != '#') {
                parsed_count++;
            }
        } else {
            LOGW << "Warning at line " << line_number << ": " << line;
        }
    }
    
    file.close();
    
    LOGI << "Loaded " << layers_.size() << " layer definitions";
    
    return true;
}

const LayerDef* LayerMap::get_layer(const std::string& name, LayerPurpose purpose) const {
    auto key = std::make_pair(name, purpose);
    auto it = name_purpose_map_.find(key);
    
    if (it != name_purpose_map_.end()) {
        return &layers_[it->second];
    }
    
    return nullptr;
}

const LayerDef* LayerMap::get_layer_by_number(uint32_t layer_number, uint32_t datatype) const {
    auto key = std::make_pair(layer_number, datatype);
    auto it = layer_index_map_.find(key);
    
    if (it != layer_index_map_.end()) {
        return &layers_[it->second];
    }
    
    return nullptr;
}

const LayerDef* LayerMap::get_layer_by_tag(gdstk::Tag tag) const {
    uint32_t layer = gdstk::get_layer(tag);
    uint32_t datatype = gdstk::get_type(tag);
    return get_layer_by_number(layer, datatype);
}

std::vector<const LayerDef*> LayerMap::get_layers_by_name(const std::string& name) const {
    std::vector<const LayerDef*> result;
    
    for (const auto& layer : layers_) {
        if (layer.name == name) {
            result.push_back(&layer);
        }
    }
    
    return result;
}

std::vector<const LayerDef*> LayerMap::get_layers_by_purpose(LayerPurpose purpose) const {
    std::vector<const LayerDef*> result;
    
    for (const auto& layer : layers_) {
        if (layer.purpose == purpose) {
            result.push_back(&layer);
        }
    }
    
    return result;
}

gdstk::Tag LayerMap::boundary_tag() const {
    const LayerDef* layer = get_layer("BOUNDARY", LayerPurpose::Drawing);
    if (layer) {
        return layer->tag();
    }
    
    // 如果找不到，回傳預設值 (100, 0)
    LOGW << "BOUNDARY layer not found in layer map, using default (100, 0)";
    return gdstk::make_tag(100, 0);
}

gdstk::Tag LayerMap::metal_tag(int level) const {
    std::string name = "M" + std::to_string(level);
    const LayerDef* layer = get_layer(name, LayerPurpose::Drawing);
    
    if (layer) {
        return layer->tag();
    }
    
    LOGE << "Metal layer " << name << " not found in layer map";
    return gdstk::make_tag(0, 0);
}

gdstk::Tag LayerMap::via_tag(int level) const {
    std::string name = "V" + std::to_string(level);
    const LayerDef* layer = get_layer(name, LayerPurpose::Drawing);
    
    if (layer) {
        return layer->tag();
    }
    
    LOGE << "Via layer " << name << " not found in layer map";
    return gdstk::make_tag(0, 0);
}

void LayerMap::print() const {
    LOGI << "=== Layer Map (" << layers_.size() << " layers) ===";
    LOGI << "Name\t\tPurpose\t\tLayer\tDatatype";
    LOGI << "----\t\t-------\t\t-----\t--------";
    
    for (const auto& layer : layers_) {
        LOGI << layer.name << "\t\t" 
             << purpose_to_string(layer.purpose) << "\t\t"
             << layer.layer_number << "\t"
             << layer.datatype;
    }
}

}  // namespace OpenFinRAM
