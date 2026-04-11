/*
 * OpenFinRAM - ASAP7 Layer Map Parser
 * 
 * This file provides data structures and parser for ASAP7 PDK layer map files.
 */

#ifndef OPENFINRAM_LAYERMAP_HPP
#define OPENFINRAM_LAYERMAP_HPP

#include <gdstk/gdstk.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <plog/Log.h>

namespace OpenFinRAM {

// ============================================================================
// Layer Purpose enumeration
// ============================================================================
enum class LayerPurpose {
    Drawing,    // Drawing layer
    Pin,        // Pin layer
    Label,      // Label layer
    Net,        // Net layer
    Blockage,   // Blockage layer
    Unknown     // Unknown
};

// ============================================================================
// Single Layer Definition
// ============================================================================
struct LayerDef {
    std::string name;           // Layer name (e.g., "M1", "V0", "BOUNDARY")
    LayerPurpose purpose;       // Layer purpose
    uint32_t layer_number;      // GDS Layer Number
    uint32_t datatype;          // GDS Datatype Number
    
    // Default constructor
    LayerDef()
        : name("")
        , purpose(LayerPurpose::Unknown)
        , layer_number(0)
        , datatype(0)
    {}
    
    // Full constructor
    LayerDef(const std::string& name_, LayerPurpose purpose_, 
             uint32_t layer_, uint32_t datatype_)
        : name(name_)
        , purpose(purpose_)
        , layer_number(layer_)
        , datatype(datatype_)
    {}
    
    // Get gdstk Tag
    gdstk::Tag tag() const {
        return gdstk::make_tag(layer_number, datatype);
    }
    
    // Comparison operator (for map sorting)
    bool operator<(const LayerDef& other) const {
        if (layer_number != other.layer_number) {
            return layer_number < other.layer_number;
        }
        return datatype < other.datatype;
    }
    
    bool operator==(const LayerDef& other) const {
        return name == other.name && 
               purpose == other.purpose && 
               layer_number == other.layer_number && 
               datatype == other.datatype;
    }
};

// ============================================================================
// Layer Map class
// Used to manage the layer mapping for the entire PDK
// ============================================================================
class LayerMap {
public:
    // Constructor
    LayerMap();
    ~LayerMap();
    
    // ========================================================================
    // File operations
    // ========================================================================
    
    // Load layer map from file
    bool load_from_file(const std::string& filename);
    
    // Initialize with hardcoded ASAP7 layer map
    void init_asap7_layermap();
    
    // Clear all data
    void clear();
    
    // ========================================================================
    // Layer queries
    // ========================================================================
    
    // Query layer by name and purpose
    const LayerDef* get_layer(const std::string& name, 
                               LayerPurpose purpose = LayerPurpose::Drawing) const;
    
    // Query layer by layer number and datatype
    const LayerDef* get_layer_by_number(uint32_t layer_number, uint32_t datatype) const;
    
    // Query layer by gdstk Tag
    const LayerDef* get_layer_by_tag(gdstk::Tag tag) const;
    
    // Get all layers with the specified name (different purposes)
    std::vector<const LayerDef*> get_layers_by_name(const std::string& name) const;
    
    // Get all layers with the specified purpose
    std::vector<const LayerDef*> get_layers_by_purpose(LayerPurpose purpose) const;
    
    // ========================================================================
    // Common Layer Quick Access
    // ========================================================================
    
    // Get BOUNDARY layer tag
    gdstk::Tag boundary_tag() const;
    
    // Get specified metal layer drawing tag (e.g., "M1", "M2")
    gdstk::Tag metal_tag(int level) const;
    
    // Get specified via layer drawing tag (e.g., "V0", "V1")
    gdstk::Tag via_tag(int level) const;
    
    // ========================================================================
    // Information Queries
    // ========================================================================
    
    // Get all layers
    const std::vector<LayerDef>& get_all_layers() const { return layers_; }
    
    // Get number of layers
    size_t size() const { return layers_.size(); }
    
    // Check if empty
    bool empty() const { return layers_.empty(); }
    
    // Print all layers (for debugging)
    void print() const;

    bool operator==(const LayerMap& other) const {
        if (layers_.size() != other.layers_.size()) {
            LOGE << "LayerMap size mismatch: " << layers_.size() 
                      << " vs " << other.layers_.size();
            return false;
        }
        for (size_t i = 0; i < layers_.size(); ++i) {
            if (!(layers_[i] == other.layers_[i])) {
                LOGE << "LayerMap layer mismatch at layer " << layers_[i].name << ": "
                          << "{" << layers_[i].layer_number << ", " << layers_[i].datatype << "} vs "
                          << "{" << other.layers_[i].layer_number << ", " << other.layers_[i].datatype << "}";
                return false;
            }
        }
        return true;
    }

private:
    // All layer definitions
    std::vector<LayerDef> layers_;
    
    // Map for quick lookup: (layer_number, datatype) -> index in layers_
    std::map<std::pair<uint32_t, uint32_t>, size_t> layer_index_map_;
    
    // Map for lookup by name and purpose: (name, purpose) -> index in layers_
    std::map<std::pair<std::string, LayerPurpose>, size_t> name_purpose_map_;
    
    // Parse purpose string
    static LayerPurpose parse_purpose(const std::string& purpose_str);
    
    // Parse a single line
    bool parse_line(const std::string& line);
    
    // Add a layer
    void add_layer(const LayerDef& layer);
};

// ============================================================================
// Helper functions
// ============================================================================

// LayerPurpose to string
const char* purpose_to_string(LayerPurpose purpose);

// String to LayerPurpose
LayerPurpose string_to_purpose(const std::string& str);

}  // namespace OpenFinRAM

#endif  // OPENFINRAM_LAYERMAP_HPP
