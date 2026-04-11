/*
 * OpenFinRAM - SRAM Filler Cell Generator
 * 
 * This file provides utilities for generating top and bottom filler cells
 * for SRAM arrays, similar to FILLER_32x2_top and FILLER_32x2_bottom in
 * the reference design.
 * 
 * The filler structure (from left to right):
 * 1. FILLER_BLANK_6t122 x 2
 * 2. dummy_corner_v2_lr x test_num_bits (with dummy_vertical_6t122_lr pairs)
 * 3. tapcell_dummy_6t122_lr x 1
 * 4. FILLER_BLANK_COLGRP_TOPnBOT (custom, width = ctrl_decode width)
 * 5. tapcell_dummy_6t122_lr x 1
 * 6. dummy_corner_v2_lr x test_num_bits (with dummy_vertical_6t122_lr pairs)
 * 7. FILLER_BLANK_6t122 x 2
 */

#ifndef OPENFINRAM_FILLER_GENERATOR_HPP
#define OPENFINRAM_FILLER_GENERATOR_HPP

#include "layermap.hpp"
#include "cell_utils.hpp"

#include <gdstk/gdstk.hpp>

#include <cstdint>
#include <string>

namespace OpenFinRAM {

// ============================================================================
// Filler Cell Library Structure
// ============================================================================

/**
 * @brief Structure containing all required cells for filler generation
 */
struct FillerCellLibrary {
    gdstk::Cell* filler_blank_6t122;        // FILLER_BLANK_6t122
    gdstk::Cell* dummy_corner_v2_lr;         // dummy_corner_v2_lr
    gdstk::Cell* dummy_vertical_6t122_lr;    // dummy_vertical_6t122_lr
    gdstk::Cell* dummy_corner_lr;            // dummy_corner_lr
    gdstk::Cell* tapcell_dummy_6t122_lr;     // tapcell_dummy_6t122_lr
    
    // Optional - will be created if null
    gdstk::Cell* filler_blank_colgrp_topnbot; // FILLER_BLANK_COLGRP_TOPnBOT
    gdstk::Cell* filler_blank_srambank;       // FILLER_BLANK_srambank_32_v4_221104 (base cell)
    
    FillerCellLibrary()
        : filler_blank_6t122(nullptr)
        , dummy_corner_v2_lr(nullptr)
        , dummy_vertical_6t122_lr(nullptr)
        , dummy_corner_lr(nullptr)
        , tapcell_dummy_6t122_lr(nullptr)
        , filler_blank_colgrp_topnbot(nullptr)
        , filler_blank_srambank(nullptr)
    {}
    
    /**
     * @brief Check if all required cells are loaded
     */
    bool is_valid() const {
        return filler_blank_6t122 != nullptr &&
               dummy_corner_v2_lr != nullptr &&
               dummy_vertical_6t122_lr != nullptr &&
               tapcell_dummy_6t122_lr != nullptr;
    }
};

// ============================================================================
// Filler Configuration
// ============================================================================

/**
 * @brief Configuration for filler cell generation
 */
struct FillerConfig {
    uint64_t test_num_bits;    // Number of bits (determines dummy_corner count)
    double ctrl_decode_width;   // Width of ctrl_decode (for center filler)
    double ctrl_decode_height;  // Height of ctrl_decode 
    bool is_top;                // true = top filler, false = bottom filler
    
    FillerConfig()
        : test_num_bits(2)
        , ctrl_decode_width(1.782)  // Default from reference
        , ctrl_decode_height(0.297)
        , is_top(true)
    {}
    
    FillerConfig(uint64_t bits, double width, double height, bool top)
        : test_num_bits(bits)
        , ctrl_decode_width(width)
        , ctrl_decode_height(height)
        , is_top(top)
    {}
};

// ============================================================================
// Filler Cell Generator Functions
// ============================================================================

/**
 * @brief Create a custom FILLER_BLANK_COLGRP_TOPnBOT cell
 * 
 * This creates a filler cell with Gate and Fin polygons to match the
 * ctrl_decode width. The cell contains:
 * - Gate polygons (vertical lines, layer 7/0)
 * - Fin polygons (horizontal lines, layer 2/0)
 * 
 * @param cell_name Name for the new cell
 * @param width Width of the cell (matches ctrl_decode width)
 * @param height Height of the cell (matches dummy_corner height)
 * @param layer_map Layer map for getting layer definitions
 * @return gdstk::Cell* New cell, nullptr on failure
 */
gdstk::Cell* create_filler_blank_colgrp(
    const char* cell_name,
    double width,
    double height,
    const LayerMap& layer_map);

/**
 * @brief Create a top filler row for SRAM
 * 
 * Creates a FILLER_TOP cell with the following structure (left to right):
 * 1. FILLER_BLANK_6t122 x 2 (stacked vertically)
 * 2. dummy_corner_v2_lr + dummy_vertical_6t122_lr pairs x test_num_bits
 * 3. tapcell_dummy_6t122_lr x 1
 * 4. FILLER_BLANK_COLGRP (custom width)
 * 5. tapcell_dummy_6t122_lr x 1
 * 6. dummy_corner_v2_lr + dummy_vertical_6t122_lr pairs x test_num_bits
 * 7. FILLER_BLANK_6t122 x 2 (stacked vertically)
 * 
 * @param lib Source cell library
 * @param config Filler configuration
 * @param layer_map Layer map
 * @return gdstk::Cell* New top filler cell, nullptr on failure
 */
gdstk::Cell* create_filler_top(
    const FillerCellLibrary& lib,
    const FillerConfig& config,
    const LayerMap& layer_map);

/**
 * @brief Create a bottom filler row for SRAM
 * 
 * Same structure as top but with Y-axis flipped references.
 * 
 * @param lib Source cell library
 * @param config Filler configuration
 * @param layer_map Layer map
 * @return gdstk::Cell* New bottom filler cell, nullptr on failure
 */
gdstk::Cell* create_filler_bottom(
    const FillerCellLibrary& lib,
    const FillerConfig& config,
    const LayerMap& layer_map);

/**
 * @brief Load filler cells from a GDS library
 * 
 * @param gds_lib Source GDS library
 * @param filler_lib Output filler cell library structure
 * @return true if all required cells were found
 */
bool load_filler_cells_from_library(
    gdstk::Library& gds_lib,
    FillerCellLibrary& filler_lib);

/**
 * @brief Add all filler cells and dependencies to a library
 * 
 * @param output_lib Target library
 * @param filler_top Top filler cell
 * @param filler_bottom Bottom filler cell
 * @param filler_lib Filler cell library (for dependencies)
 */
void add_filler_cells_to_library(
    gdstk::Library& output_lib,
    gdstk::Cell* filler_top,
    gdstk::Cell* filler_bottom,
    const FillerCellLibrary& filler_lib);

}  // namespace OpenFinRAM

#endif  // OPENFINRAM_FILLER_GENERATOR_HPP
