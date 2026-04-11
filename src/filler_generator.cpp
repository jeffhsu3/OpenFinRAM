/*
 * OpenFinRAM - SRAM Filler Cell Generator Implementation
 * 
 * This file implements the filler cell generation functions.
 * 
 * Key principle: All cells are placed using BOUNDARY-to-BOUNDARY alignment.
 * The reference origin is calculated such that the cell's BOUNDARY.min aligns
 * with the desired position.
 */

#include "filler_generator.hpp"

#include <plog/Log.h>

#include <cmath>
#include <cstring>
#include <cstdio>

namespace OpenFinRAM {

// ============================================================================
// Helper: Calculate reference origin for boundary-aligned placement
// 
// When placing a cell, we want its BOUNDARY.min to be at the target position.
// Since the reference origin is the cell's local (0,0), we need to offset it.
// 
// If BOUNDARY.min = (bx, by), and we want BOUNDARY.min at (target_x, target_y):
//   ref->origin = (target_x - bx, target_y - by)
// 
// For a flipped cell (rotation = PI, x_reflection = true):
//   The BOUNDARY.max becomes the new min after transformation
//   ref->origin = (target_x + bx + width, target_y - by)
// ============================================================================

struct BoundaryInfo {
    gdstk::Vec2 min;      // BOUNDARY polygon min corner
    gdstk::Vec2 max;      // BOUNDARY polygon max corner
    double width;         // BOUNDARY width
    double height;        // BOUNDARY height
    bool valid;
    
    BoundaryInfo() : min{0,0}, max{0,0}, width(0), height(0), valid(false) {}
};

static BoundaryInfo get_boundary_info(gdstk::Cell* cell, const LayerMap& layer_map) {
    BoundaryInfo info;
    
    if (cell == nullptr) return info;
    
    // Try to get from CellSize (uses BOUNDARY layer)
    CellSize size = get_cell_size(cell, layer_map);
    if (size.valid) {
        info.min = size.min;
        info.max = size.max;
        info.width = size.width;
        info.height = size.height;
        info.valid = true;
        return info;
    }
    
    // Fallback to bounding box
    gdstk::Vec2 bb_min, bb_max;
    cell->bounding_box(bb_min, bb_max);
    info.min = bb_min;
    info.max = bb_max;
    info.width = bb_max.x - bb_min.x;
    info.height = bb_max.y - bb_min.y;
    info.valid = true;
    
    return info;
}

// ============================================================================
// Helper function: Create Gate and Fin filler cell
// ============================================================================
gdstk::Cell* create_filler_blank_colgrp(
    const char* cell_name,
    double width,
    double height,
    const LayerMap& layer_map)
{
    LOGI << "Creating filler cell: " << cell_name;
    LOGI << "  Size: " << width << " x " << height;
    
    // Get layer definitions
    const LayerDef* gate_layer = layer_map.get_layer("Gate", LayerPurpose::Drawing);
    const LayerDef* fin_layer = layer_map.get_layer("fin", LayerPurpose::Drawing);
    const LayerDef* boundary_layer = layer_map.get_layer("BOUNDARY", LayerPurpose::Drawing);
    
    if (gate_layer == nullptr) {
        LOGW << "Gate layer not found, using default layer 7/0";
    }
    if (fin_layer == nullptr) {
        LOGW << "fin layer not found, using default layer 2/0";
    }
    
    gdstk::Tag gate_tag = gate_layer ? gate_layer->tag() : gdstk::make_tag(7, 0);
    gdstk::Tag fin_tag = fin_layer ? fin_layer->tag() : gdstk::make_tag(2, 0);
    gdstk::Tag boundary_tag = boundary_layer ? boundary_layer->tag() : gdstk::make_tag(100, 0);
    
    // Create the cell
    gdstk::Cell* cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    cell->init(cell_name);
    
    // Parameters matching reference design
    const double gate_width = 0.020;      // Gate polygon width
    const double gate_spacing = 0.034;    // Spacing between gates
    const double fin_height = 0.007;      // Fin polygon height
    const double fin_spacing = 0.027;     // Spacing between fins
    
    // Calculate number of gates and fins
    double gate_pitch = gate_width + gate_spacing;
    uint64_t num_gates = (uint64_t)std::ceil(width / gate_pitch);
    uint64_t num_fins = (uint64_t)std::floor(height / fin_spacing) + 1;
    
    LOGI << "  Creating " << num_gates << " gate polygons";
    LOGI << "  Creating " << num_fins << " fin polygons";
    
    // Create Gate polygons (vertical lines across the full height)
    // BOUNDARY starts at (0, 0), so gates also start from y=0
    for (uint64_t i = 0; i < num_gates; i++) {
        double gate_x_start = i * gate_pitch;
        double gate_x_end = gate_x_start + gate_width;
        
        if (gate_x_end > width) {
            gate_x_end = width;
            if (gate_x_end <= gate_x_start) break;
        }
        
        gdstk::Polygon* gate_poly = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
        gdstk::Vec2 gate_points[4] = {
            {gate_x_start, 0.0},
            {gate_x_end, 0.0},
            {gate_x_end, height},
            {gate_x_start, height}
        };
        gate_poly->point_array.extend({.capacity = 0, .count = 4, .items = gate_points});
        gate_poly->tag = gate_tag;
        cell->polygon_array.append(gate_poly);
    }
    
    // Create Fin polygons (horizontal lines)
    for (uint64_t i = 0; i < num_fins; i++) {
        double fin_y_start = i * fin_spacing;
        double fin_y_end = fin_y_start + fin_height;
        
        if (fin_y_end > height) break;
        
        gdstk::Polygon* fin_poly = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
        gdstk::Vec2 fin_points[4] = {
            {0.0, fin_y_start},
            {width, fin_y_start},
            {width, fin_y_end},
            {0.0, fin_y_end}
        };
        fin_poly->point_array.extend({.capacity = 0, .count = 4, .items = fin_points});
        fin_poly->tag = fin_tag;
        cell->polygon_array.append(fin_poly);
    }
    
    // Create BOUNDARY polygon starting at (0, 0)
    gdstk::Polygon* boundary = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    gdstk::Vec2 boundary_points[4] = {
        {0.0, 0.0},
        {width, 0.0},
        {width, height},
        {0.0, height}
    };
    boundary->point_array.extend({.capacity = 0, .count = 4, .items = boundary_points});
    boundary->tag = boundary_tag;
    cell->polygon_array.append(boundary);
    
    LOGI << "  Created filler cell with " << cell->polygon_array.count << " polygons";
    LOGI << "  BOUNDARY: (0, 0) to (" << width << ", " << height << ")";
    
    return cell;
}

// ============================================================================
// Create top filler row
// ============================================================================
gdstk::Cell* create_filler_top(
    const FillerCellLibrary& lib,
    const FillerConfig& config,
    const LayerMap& layer_map)
{
    if (!lib.is_valid()) {
        LOGE << "FillerCellLibrary is not valid, missing required cells";
        return nullptr;
    }
    
    LOGI << "Creating FILLER_TOP cell";
    LOGI << "  test_num_bits: " << config.test_num_bits;
    LOGI << "  ctrl_decode_width: " << config.ctrl_decode_width;
    
    // Get BOUNDARY info for all cells
    BoundaryInfo filler_blank_bnd = get_boundary_info(lib.filler_blank_6t122, layer_map);
    BoundaryInfo dummy_corner_bnd = get_boundary_info(lib.dummy_corner_v2_lr, layer_map);
    BoundaryInfo dummy_vertical_bnd = get_boundary_info(lib.dummy_vertical_6t122_lr, layer_map);
    BoundaryInfo tapcell_bnd = get_boundary_info(lib.tapcell_dummy_6t122_lr, layer_map);
    BoundaryInfo dummy_corner_lr_bnd;
    if (lib.dummy_corner_lr) {
        dummy_corner_lr_bnd = get_boundary_info(lib.dummy_corner_lr, layer_map);
    }
    BoundaryInfo filler_srambank_bnd;
    if (lib.filler_blank_srambank) {
        filler_srambank_bnd = get_boundary_info(lib.filler_blank_srambank, layer_map);
    }
    
    LOGI << "  FILLER_BLANK_6t122 BOUNDARY: min=(" << filler_blank_bnd.min.x << "," << filler_blank_bnd.min.y 
         << "), size=" << filler_blank_bnd.width << "x" << filler_blank_bnd.height;
    LOGI << "  dummy_corner_v2_lr BOUNDARY: min=(" << dummy_corner_bnd.min.x << "," << dummy_corner_bnd.min.y 
         << "), size=" << dummy_corner_bnd.width << "x" << dummy_corner_bnd.height;
    LOGI << "  dummy_vertical_6t122_lr BOUNDARY: min=(" << dummy_vertical_bnd.min.x << "," << dummy_vertical_bnd.min.y 
         << "), size=" << dummy_vertical_bnd.width << "x" << dummy_vertical_bnd.height;
    LOGI << "  tapcell_dummy_6t122_lr BOUNDARY: min=(" << tapcell_bnd.min.x << "," << tapcell_bnd.min.y 
         << "), size=" << tapcell_bnd.width << "x" << tapcell_bnd.height;
    
    // Create cell name
    char cell_name[128];
    snprintf(cell_name, sizeof(cell_name), "FILLER_%lux%lu_top", 
             (unsigned long)(config.test_num_bits * 2), 2UL);
    
    // Create the cell
    gdstk::Cell* filler_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    filler_cell->init(cell_name);
    
    LOGI << "Creating cell: " << cell_name;
    
    // All cells in TOP filler have their BOUNDARY.min.y at y=0
    // current_x tracks where the next cell's BOUNDARY.min.x should be
    double current_x = 0.0;
    const double base_y = 0.0;  // All BOUNDARY.min.y aligned to 0
    
    // ========================================================================
    // 1. Left FILLER_BLANK_6t122 x 2 (placed side-by-side, no rotation)
    // ========================================================================
    // First FILLER_BLANK - normal orientation
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_6t122);
        ref->origin = {current_x - filler_blank_bnd.min.x, base_y - filler_blank_bnd.min.y - 0.027};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    current_x += filler_blank_bnd.width;
    
    // Second FILLER_BLANK - normal orientation
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_6t122);
        ref->origin = {current_x - filler_blank_bnd.min.x, base_y - filler_blank_bnd.min.y - 0.027};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    current_x += filler_blank_bnd.width;
    
    LOGI << "  Added 2x FILLER_BLANK_6t122 (left), next_x=" << current_x;
    
    // ========================================================================
    // 2. Left dummy_corner_v2_lr (top position)
    // ========================================================================
    // dummy_corner_v2_lr is placed with rotation=PI (flipped top to bottom)
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.dummy_corner_v2_lr);
        // For rot=PI: boundary min/max swap both x and y
        // To align new min (which was max) at (current_x, base_y):
        // origin = (current_x + max.x, base_y + max.y)
        ref->origin = {current_x + dummy_corner_bnd.max.x, base_y + dummy_corner_bnd.max.y};
        ref->rotation = M_PI;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    current_x += dummy_corner_bnd.width;
    
    LOGI << "  Added left dummy_corner_v2_lr, next_x=" << current_x;
    
    // ========================================================================
    // 3. Left dummy_vertical_6t122_lr pairs x test_num_bits
    // ========================================================================
    // Each "bit" needs a pair of dummy_vertical cells (one normal, one mirrored)
    for (uint64_t bit = 0; bit < config.test_num_bits / 2; bit++) {
        // Normal orientation
        {
            gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ref->init(lib.dummy_vertical_6t122_lr);
            ref->origin = {current_x - dummy_vertical_bnd.min.x, base_y - dummy_vertical_bnd.min.y};
            ref->magnification = 1.0;
            filler_cell->reference_array.append(ref);
        }
        current_x += dummy_vertical_bnd.width;
        
        // Mirrored (rot=PI, x_refl=true)
        {
            gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ref->init(lib.dummy_vertical_6t122_lr);
            ref->origin = {current_x + dummy_vertical_bnd.max.x, -0.0135};
            ref->rotation = M_PI;
            ref->x_reflection = true;
            ref->magnification = 1.0;
            filler_cell->reference_array.append(ref);
        }
        current_x += dummy_vertical_bnd.width;
    }
    
    LOGI << "  Added " << config.test_num_bits / 2 << " dummy_vertical pairs (left), next_x=" << current_x;
    
    // ========================================================================
    // 4. Left dummy_corner_lr
    // ========================================================================
    if (lib.dummy_corner_lr != nullptr) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.dummy_corner_lr);
        ref->origin = {current_x - dummy_corner_lr_bnd.min.x, base_y - dummy_corner_lr_bnd.min.y};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        current_x += dummy_corner_lr_bnd.width;
        
        LOGI << "  Added left dummy_corner_lr, next_x=" << current_x;
    }
    
    // ========================================================================
    // 5. Left tapcell_dummy_6t122_lr
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.tapcell_dummy_6t122_lr);
        ref->origin = {current_x - tapcell_bnd.min.x, base_y - tapcell_bnd.min.y};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    current_x += tapcell_bnd.width;
    
    LOGI << "  Added left tapcell, next_x=" << current_x;
    
    // ========================================================================
    // 6. Center FILLER_BLANK_COLGRP (width = ctrl_decode_with_filler - side components)
    // ========================================================================
    // Calculate side components width (one side):
    // 2x FILLER_BLANK + 1x dummy_corner_v2_lr + test_num_bits*2 dummy_vertical + 1x dummy_corner_lr + 1x tapcell + 1x filler_blank_srambank
    double side_width = 2 * filler_blank_bnd.width 
                       + dummy_corner_bnd.width 
                       + config.test_num_bits * 2 * dummy_vertical_bnd.width
                       + (lib.dummy_corner_lr ? dummy_corner_lr_bnd.width : 0.0)
                       + tapcell_bnd.width
                       + (lib.filler_blank_srambank ? filler_srambank_bnd.width : 0.0);
    
    // Center filler width = total width - left side - right side
    double center_width = config.ctrl_decode_width - 2 * side_width;
    double center_height = dummy_corner_bnd.height;  // Match the row height
    
    LOGI << "  Side components width: " << side_width;
    LOGI << "  Center filler width: " << center_width;
    
    if (center_width <= 0) {
        LOGW << "  Warning: Center filler width is <= 0, ctrl_decode_width may be too small";
        center_width = 0.001;  // Minimum width
    }
    
    gdstk::Cell* center_filler = lib.filler_blank_colgrp_topnbot;
    
    if (center_filler == nullptr) {
        // Create a new center filler cell with BOUNDARY at (0,0)
        char center_name[128];
        snprintf(center_name, sizeof(center_name), "FILLER_BLANK_COLGRP_%lux%lu",
                 (unsigned long)(config.test_num_bits * 2), 2UL);
        center_filler = create_filler_blank_colgrp(center_name, center_width, 
                                                   center_height, layer_map);
    }
    
    // Add left filler_blank_srambank to extend fins
    if (lib.filler_blank_srambank != nullptr) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_srambank);
        ref->origin = {current_x - filler_srambank_bnd.min.x, -0.0135 - 0.027 - filler_srambank_bnd.min.y};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        LOGI << "  Added left filler_blank_srambank at x=" << current_x;
        current_x += filler_srambank_bnd.width;
    }
    
    if (center_filler != nullptr) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(center_filler);
        // Center filler has BOUNDARY at (0,0), so just place at current_x
        ref->origin = {current_x, -0.0135 - 0.027};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        LOGI << "  Added center filler at x=" << current_x;
        current_x += center_width;
    }
    
    // Add right filler_blank_srambank to extend fins
    if (lib.filler_blank_srambank != nullptr) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_srambank);
        ref->origin = {current_x - filler_srambank_bnd.min.x, -0.0135 - 0.027 - filler_srambank_bnd.min.y};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        LOGI << "  Added right filler_blank_srambank at x=" << current_x;
    }
    // Don't advance current_x here - we'll place right side from right edge
    
    // ========================================================================
    // RIGHT SIDE: Place from right edge towards left
    // ========================================================================
    // Total width should be ctrl_decode_width
    // Start from right edge and work backwards
    double right_x = config.ctrl_decode_width;
    
    // ========================================================================
    // 11. Right FILLER_BLANK_6t122 x 2 (rightmost, placed from right edge)
    // ========================================================================
    // Second FILLER_BLANK (rightmost)
    right_x -= filler_blank_bnd.width;
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_6t122);
        ref->origin = {right_x - filler_blank_bnd.min.x, base_y - filler_blank_bnd.min.y - 0.027};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    
    // First FILLER_BLANK
    right_x -= filler_blank_bnd.width;
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_6t122);
        ref->origin = {right_x - filler_blank_bnd.min.x, base_y - filler_blank_bnd.min.y - 0.027};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    
    LOGI << "  Added 2x FILLER_BLANK_6t122 (right), right_x=" << right_x;
    
    // ========================================================================
    // 10. Right dummy_corner_v2_lr (x_reflection only)
    // ========================================================================
    right_x -= dummy_corner_bnd.width;
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.dummy_corner_v2_lr);
        // For x_reflection only: X is mirrored, Y stays same
        // Place at right_x, need to align boundary
        ref->origin = {right_x + dummy_corner_bnd.max.x - 0.108, base_y + dummy_corner_bnd.max.y};
        ref->x_reflection = true;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    
    LOGI << "  Added right dummy_corner_v2_lr, right_x=" << right_x;
    
    // ========================================================================
    // 9. Right dummy_vertical_6t122_lr pairs x test_num_bits (from right to left)
    // ========================================================================
    for (uint64_t bit = 0; bit < config.test_num_bits / 2; bit++) {
        // Mirrored cell first (rightmost of pair)
        right_x -= dummy_vertical_bnd.width;
        {
            gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ref->init(lib.dummy_vertical_6t122_lr);
            ref->origin = {right_x + dummy_vertical_bnd.max.x, -0.0135};
            ref->rotation = M_PI;
            ref->x_reflection = true;
            ref->magnification = 1.0;
            filler_cell->reference_array.append(ref);
        }
        
        // Normal orientation cell
        right_x -= dummy_vertical_bnd.width;
        {
            gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ref->init(lib.dummy_vertical_6t122_lr);
            ref->origin = {right_x - dummy_vertical_bnd.min.x, base_y - dummy_vertical_bnd.min.y};
            ref->magnification = 1.0;
            filler_cell->reference_array.append(ref);
        }
    }
    
    LOGI << "  Added " << config.test_num_bits / 2 << " dummy_vertical pairs (right), right_x=" << right_x;
    
    // ========================================================================
    // 8. Right dummy_corner_lr (mirrored about Y-axis: rot=PI + x_refl)
    // ========================================================================
    if (lib.dummy_corner_lr != nullptr) {
        right_x -= dummy_corner_lr_bnd.width;
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.dummy_corner_lr);
        ref->origin = {right_x + dummy_corner_lr_bnd.max.x, base_y - dummy_corner_lr_bnd.min.y};
        ref->rotation = M_PI;
        ref->x_reflection = true;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        
        LOGI << "  Added right dummy_corner_lr, right_x=" << right_x;
    }
    
    // ========================================================================
    // 7. Right tapcell_dummy_6t122_lr (mirrored about Y-axis: rot=PI + x_refl)
    // ========================================================================
    right_x -= tapcell_bnd.width;
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.tapcell_dummy_6t122_lr);
        ref->origin = {right_x + tapcell_bnd.max.x, base_y - tapcell_bnd.min.y};
        ref->rotation = M_PI;
        ref->x_reflection = true;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    
    LOGI << "  Added right tapcell, right_x=" << right_x;
    LOGI << "  Gap between center filler and right tapcell: " << (right_x - current_x - center_width);
    
    // Update current_x to final width
    current_x = config.ctrl_decode_width;
    
    LOGI << "  Final width=" << current_x;
    LOGI << "Created FILLER_TOP with " << filler_cell->reference_array.count << " references";
    
    // Create BOUNDARY for the entire filler cell
    const LayerDef* boundary_layer = layer_map.get_layer("BOUNDARY", LayerPurpose::Drawing);
    gdstk::Tag boundary_tag = boundary_layer ? boundary_layer->tag() : gdstk::make_tag(100, 0);
    
    // Add GCut layer spanning full width
    const LayerDef* gcut_layer = layer_map.get_layer("GCut", LayerPurpose::Drawing);
    gdstk::Tag gcut_tag = gcut_layer ? gcut_layer->tag() : gdstk::make_tag(21, 0);
    const double gcut_height = 0.044;
    const double gcut_y_top = 0.2345;  // Y start for FILLER_TOP
    
    // Full-width GCut polygon
    {
        gdstk::Polygon* gcut_poly = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
        gdstk::Vec2 gcut_points[4] = {
            {0.0, gcut_y_top},
            {config.ctrl_decode_width, gcut_y_top},
            {config.ctrl_decode_width, gcut_y_top + gcut_height},
            {0.0, gcut_y_top + gcut_height}
        };
        gcut_poly->point_array.extend({.capacity = 0, .count = 4, .items = gcut_points});
        gcut_poly->tag = gcut_tag;
        filler_cell->polygon_array.append(gcut_poly);
    }
    
    LOGI << "  Added GCut polygon (full width)";
    
    // Add LIG drawing layer spanning full width
    const LayerDef* lig_layer = layer_map.get_layer("LIG", LayerPurpose::Drawing);
    gdstk::Tag lig_tag = lig_layer ? lig_layer->tag() : gdstk::make_tag(8, 0);
    const double lig_height = 0.016;
    const double lig_y_top = 0.2485;  // Y start for FILLER_TOP
    
    // Full-width LIG polygon
    {
        gdstk::Polygon* lig_poly = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
        gdstk::Vec2 lig_points[4] = {
            {0.0, lig_y_top},
            {config.ctrl_decode_width, lig_y_top},
            {config.ctrl_decode_width, lig_y_top + lig_height},
            {0.0, lig_y_top + lig_height}
        };
        lig_poly->point_array.extend({.capacity = 0, .count = 4, .items = lig_points});
        lig_poly->tag = lig_tag;
        filler_cell->polygon_array.append(lig_poly);
    }
    
    LOGI << "  Added LIG polygon (full width)";
    
    gdstk::Polygon* boundary = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    gdstk::Vec2 boundary_points[4] = {
        {0.0, 0.0},
        {current_x, 0.0},
        {current_x, dummy_corner_bnd.height},
        {0.0, dummy_corner_bnd.height}
    };
    boundary->point_array.extend({.capacity = 0, .count = 4, .items = boundary_points});
    boundary->tag = boundary_tag;
    filler_cell->polygon_array.append(boundary);
    
    return filler_cell;
}

// ============================================================================
// Create bottom filler row
// ============================================================================
gdstk::Cell* create_filler_bottom(
    const FillerCellLibrary& lib,
    const FillerConfig& config,
    const LayerMap& layer_map)
{
    if (!lib.is_valid()) {
        LOGE << "FillerCellLibrary is not valid, missing required cells";
        return nullptr;
    }
    
    LOGI << "Creating FILLER_BOTTOM cell";
    LOGI << "  test_num_bits: " << config.test_num_bits;
    LOGI << "  ctrl_decode_width: " << config.ctrl_decode_width;
    
    // Get BOUNDARY info for all cells
    BoundaryInfo filler_blank_bnd = get_boundary_info(lib.filler_blank_6t122, layer_map);
    BoundaryInfo dummy_corner_bnd = get_boundary_info(lib.dummy_corner_v2_lr, layer_map);
    BoundaryInfo dummy_vertical_bnd = get_boundary_info(lib.dummy_vertical_6t122_lr, layer_map);
    BoundaryInfo tapcell_bnd = get_boundary_info(lib.tapcell_dummy_6t122_lr, layer_map);
    BoundaryInfo dummy_corner_lr_bnd;
    if (lib.dummy_corner_lr) {
        dummy_corner_lr_bnd = get_boundary_info(lib.dummy_corner_lr, layer_map);
    }
    BoundaryInfo filler_srambank_bnd;
    if (lib.filler_blank_srambank) {
        filler_srambank_bnd = get_boundary_info(lib.filler_blank_srambank, layer_map);
    }
    
    // Create cell name
    char cell_name[128];
    snprintf(cell_name, sizeof(cell_name), "FILLER_%lux%lu_bottom", 
             (unsigned long)(config.test_num_bits * 2), 2UL);
    
    // Create the cell
    gdstk::Cell* filler_cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
    filler_cell->init(cell_name);
    
    LOGI << "Creating cell: " << cell_name;
    
    // For BOTTOM filler, cells are Y-flipped compared to TOP
    // This means we use x_reflection for all cells that were normal in TOP,
    // and remove x_reflection from cells that had it in TOP
    double current_x = 0.0;
    const double base_y = 0.0;
    
    // ========================================================================
    // 1. Left FILLER_BLANK_6t122 x 2 (placed side-by-side, no rotation)
    // ========================================================================
    // First - normal orientation
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_6t122);
        ref->origin = {current_x - filler_blank_bnd.min.x, base_y - filler_blank_bnd.min.y + 0.0135};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    current_x += filler_blank_bnd.width;
    
    // Second - normal orientation
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_6t122);
        ref->origin = {current_x - filler_blank_bnd.min.x, base_y - filler_blank_bnd.min.y + 0.0135};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    current_x += filler_blank_bnd.width;
    
    LOGI << "  Added 2x FILLER_BLANK_6t122 (left), next_x=" << current_x;
    
    // ========================================================================
    // 2. Left dummy_corner_v2_lr (Y-flipped: rot=PI + x_refl=true)
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.dummy_corner_v2_lr);
        ref->origin = {current_x + dummy_corner_bnd.max.x, base_y - dummy_corner_bnd.min.y};
        ref->rotation = M_PI;
        ref->x_reflection = true;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    current_x += dummy_corner_bnd.width;
    
    // ========================================================================
    // 3. Left dummy_vertical_6t122_lr pairs (Y-flipped)
    // ========================================================================
    for (uint64_t bit = 0; bit < config.test_num_bits / 2; bit++) {
        // In TOP: normal -> in BOTTOM: x_reflection
        {
            gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ref->init(lib.dummy_vertical_6t122_lr);
            ref->origin = {current_x - dummy_vertical_bnd.min.x, base_y + dummy_vertical_bnd.max.y};
            ref->x_reflection = true;
            ref->magnification = 1.0;
            filler_cell->reference_array.append(ref);
        }
        current_x += dummy_vertical_bnd.width;
        
        // In TOP: (rot=PI, x_refl=true) -> in BOTTOM: rot=PI only
        {
            gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ref->init(lib.dummy_vertical_6t122_lr);
            ref->origin = {current_x + dummy_vertical_bnd.max.x, base_y + dummy_vertical_bnd.max.y};
            ref->rotation = M_PI;
            ref->magnification = 1.0;
            filler_cell->reference_array.append(ref);
        }
        current_x += dummy_vertical_bnd.width;
    }
    
    // ========================================================================
    // 4. Left dummy_corner_lr (Y-flipped)
    // ========================================================================
    if (lib.dummy_corner_lr != nullptr) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.dummy_corner_lr);
        ref->origin = {current_x - dummy_corner_lr_bnd.min.x, base_y + dummy_corner_lr_bnd.max.y};
        ref->x_reflection = true;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        current_x += dummy_corner_lr_bnd.width;
    }
    
    // ========================================================================
    // 5. Left tapcell (Y-flipped)
    // ========================================================================
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.tapcell_dummy_6t122_lr);
        ref->origin = {current_x - tapcell_bnd.min.x, base_y + tapcell_bnd.max.y};
        ref->x_reflection = true;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    current_x += tapcell_bnd.width;
    
    // ========================================================================
    // 6. Center FILLER_BLANK_COLGRP (width = ctrl_decode_with_filler - side components)
    // ========================================================================
    // Calculate side components width (one side):
    // 2x FILLER_BLANK + 1x dummy_corner_v2_lr + test_num_bits*2 dummy_vertical + 1x dummy_corner_lr + 1x tapcell + 1x filler_blank_srambank
    double side_width = 2 * filler_blank_bnd.width 
                       + dummy_corner_bnd.width 
                       + config.test_num_bits * 2 * dummy_vertical_bnd.width
                       + (lib.dummy_corner_lr ? dummy_corner_lr_bnd.width : 0.0)
                       + tapcell_bnd.width
                       + (lib.filler_blank_srambank ? filler_srambank_bnd.width : 0.0);
    
    // Center filler width = total width - left side - right side
    double center_width = config.ctrl_decode_width - 2 * side_width;
    double center_height = dummy_corner_bnd.height;
    
    if (center_width <= 0) {
        LOGW << "  Warning: Center filler width is <= 0, ctrl_decode_width may be too small";
        center_width = 0.001;  // Minimum width
    }
    
    gdstk::Cell* center_filler = lib.filler_blank_colgrp_topnbot;
    
    if (center_filler == nullptr) {
        char center_name[128];
        snprintf(center_name, sizeof(center_name), "FILLER_BLANK_COLGRP_%lux%lu",
                 (unsigned long)(config.test_num_bits * 2), 2UL);
        center_filler = create_filler_blank_colgrp(center_name, center_width, 
                                                   center_height, layer_map);
    }
    
    // Add left filler_blank_srambank to extend fins
    if (lib.filler_blank_srambank != nullptr) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_srambank);
        ref->origin = {current_x - filler_srambank_bnd.min.x, base_y - filler_srambank_bnd.min.y};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        LOGI << "  Added left filler_blank_srambank at x=" << current_x;
        current_x += filler_srambank_bnd.width;
    }
    
    if (center_filler != nullptr) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(center_filler);
        ref->origin = {current_x, base_y};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        LOGI << "  Added center filler at x=" << current_x;
        current_x += center_width;
    }
    
    // Add right filler_blank_srambank to extend fins
    if (lib.filler_blank_srambank != nullptr) {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_srambank);
        ref->origin = {current_x - filler_srambank_bnd.min.x, base_y - filler_srambank_bnd.min.y};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        LOGI << "  Added right filler_blank_srambank at x=" << current_x;
    }
    // Don't advance current_x here - we'll place right side from right edge
    
    // ========================================================================
    // RIGHT SIDE: Place from right edge towards left
    // ========================================================================
    double right_x = config.ctrl_decode_width;
    
    // ========================================================================
    // 11. Right FILLER_BLANK_6t122 x 2 (rightmost, placed from right edge)
    // ========================================================================
    right_x -= filler_blank_bnd.width;
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_6t122);
        ref->origin = {right_x - filler_blank_bnd.min.x, base_y - filler_blank_bnd.min.y + 0.0135};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    
    right_x -= filler_blank_bnd.width;
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.filler_blank_6t122);
        ref->origin = {right_x - filler_blank_bnd.min.x, base_y - filler_blank_bnd.min.y + 0.0135};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    
    LOGI << "  Added 2x FILLER_BLANK_6t122 (right), right_x=" << right_x;
    
    // ========================================================================
    // 10. Right dummy_corner_v2_lr (Y-flipped + X-mirrored = normal)
    // ========================================================================
    right_x -= dummy_corner_bnd.width;
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.dummy_corner_v2_lr);
        ref->origin = {right_x - dummy_corner_bnd.min.x, base_y - dummy_corner_bnd.min.y};
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    
    LOGI << "  Added right dummy_corner_v2_lr, right_x=" << right_x;
    
    // ========================================================================
    // 9. Right dummy_vertical pairs (Y-flipped + X-mirrored) - from right to left
    // ========================================================================
    for (uint64_t bit = 0; bit < config.test_num_bits / 2; bit++) {
        // rot=PI cell first (rightmost of pair)
        right_x -= dummy_vertical_bnd.width;
        {
            gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ref->init(lib.dummy_vertical_6t122_lr);
            ref->origin = {right_x + dummy_vertical_bnd.max.x, base_y + dummy_vertical_bnd.max.y};
            ref->rotation = M_PI;
            ref->magnification = 1.0;
            filler_cell->reference_array.append(ref);
        }
        
        // x_reflection cell
        right_x -= dummy_vertical_bnd.width;
        {
            gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
            ref->init(lib.dummy_vertical_6t122_lr);
            ref->origin = {right_x - dummy_vertical_bnd.min.x, base_y + dummy_vertical_bnd.max.y};
            ref->x_reflection = true;
            ref->magnification = 1.0;
            filler_cell->reference_array.append(ref);
        }
    }
    
    LOGI << "  Added " << config.test_num_bits / 2 << " dummy_vertical pairs (right), right_x=" << right_x;
    
    // ========================================================================
    // 8. Right dummy_corner_lr (Y-flipped + X-mirrored = rot=PI only)
    // ========================================================================
    if (lib.dummy_corner_lr != nullptr) {
        right_x -= dummy_corner_lr_bnd.width;
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.dummy_corner_lr);
        ref->origin = {right_x + dummy_corner_lr_bnd.max.x, base_y + dummy_corner_lr_bnd.max.y};
        ref->rotation = M_PI;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
        
        LOGI << "  Added right dummy_corner_lr, right_x=" << right_x;
    }
    
    // ========================================================================
    // 7. Right tapcell (Y-flipped + X-mirrored = rot=PI only)
    // ========================================================================
    right_x -= tapcell_bnd.width;
    {
        gdstk::Reference* ref = (gdstk::Reference*)gdstk::allocate_clear(sizeof(gdstk::Reference));
        ref->init(lib.tapcell_dummy_6t122_lr);
        ref->origin = {right_x + tapcell_bnd.max.x, base_y + tapcell_bnd.max.y};
        ref->rotation = M_PI;
        ref->magnification = 1.0;
        filler_cell->reference_array.append(ref);
    }
    
    LOGI << "  Added right tapcell, right_x=" << right_x;
    
    // Update current_x to final width
    current_x = config.ctrl_decode_width;
    
    LOGI << "Created FILLER_BOTTOM with " << filler_cell->reference_array.count << " references";
    LOGI << "  Final width: " << current_x;
    
    // Create BOUNDARY for the entire filler cell
    const LayerDef* boundary_layer = layer_map.get_layer("BOUNDARY", LayerPurpose::Drawing);
    gdstk::Tag boundary_tag = boundary_layer ? boundary_layer->tag() : gdstk::make_tag(100, 0);
    
    // Add GCut layer spanning full width
    const LayerDef* gcut_layer = layer_map.get_layer("GCut", LayerPurpose::Drawing);
    gdstk::Tag gcut_tag = gcut_layer ? gcut_layer->tag() : gdstk::make_tag(21, 0);
    const double gcut_height = 0.044;
    const double gcut_y_bottom = -0.022;  // Y start for FILLER_BOTTOM
    
    // Full-width GCut polygon
    {
        gdstk::Polygon* gcut_poly = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
        gdstk::Vec2 gcut_points[4] = {
            {0.0, gcut_y_bottom},
            {config.ctrl_decode_width, gcut_y_bottom},
            {config.ctrl_decode_width, gcut_y_bottom + gcut_height},
            {0.0, gcut_y_bottom + gcut_height}
        };
        gcut_poly->point_array.extend({.capacity = 0, .count = 4, .items = gcut_points});
        gcut_poly->tag = gcut_tag;
        filler_cell->polygon_array.append(gcut_poly);
    }
    
    LOGI << "  Added GCut polygon (full width)";
    
    // Add LIG drawing layer spanning full width
    const LayerDef* lig_layer = layer_map.get_layer("LIG", LayerPurpose::Drawing);
    gdstk::Tag lig_tag = lig_layer ? lig_layer->tag() : gdstk::make_tag(8, 0);
    const double lig_height = 0.016;
    const double lig_y_bottom = -0.008;  // Y start for FILLER_BOTTOM
    
    // Full-width LIG polygon
    {
        gdstk::Polygon* lig_poly = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
        gdstk::Vec2 lig_points[4] = {
            {0.0, lig_y_bottom},
            {config.ctrl_decode_width, lig_y_bottom},
            {config.ctrl_decode_width, lig_y_bottom + lig_height},
            {0.0, lig_y_bottom + lig_height}
        };
        lig_poly->point_array.extend({.capacity = 0, .count = 4, .items = lig_points});
        lig_poly->tag = lig_tag;
        filler_cell->polygon_array.append(lig_poly);
    }
    
    LOGI << "  Added LIG polygon (full width)";
    
    gdstk::Polygon* boundary = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));
    gdstk::Vec2 boundary_points[4] = {
        {0.0, 0.0},
        {current_x, 0.0},
        {current_x, dummy_corner_bnd.height},
        {0.0, dummy_corner_bnd.height}
    };
    boundary->point_array.extend({.capacity = 0, .count = 4, .items = boundary_points});
    boundary->tag = boundary_tag;
    filler_cell->polygon_array.append(boundary);
    
    return filler_cell;
}

// ============================================================================
// Load filler cells from library
// ============================================================================
bool load_filler_cells_from_library(
    gdstk::Library& gds_lib,
    FillerCellLibrary& filler_lib)
{
    LOGI << "Loading filler cells from library...";
    
    filler_lib.filler_blank_6t122 = gds_lib.get_cell("FILLER_BLANK_6t122");
    filler_lib.dummy_corner_v2_lr = gds_lib.get_cell("dummy_corner_v2_lr");
    filler_lib.dummy_vertical_6t122_lr = gds_lib.get_cell("dummy_vertical_6t122_lr");
    filler_lib.dummy_corner_lr = gds_lib.get_cell("dummy_corner_lr");
    filler_lib.tapcell_dummy_6t122_lr = gds_lib.get_cell("tapcell_dummy_6t122_lr");
    filler_lib.filler_blank_colgrp_topnbot = gds_lib.get_cell("FILLER_BLANK_COLGRP_TOPnBOT");
    filler_lib.filler_blank_srambank = gds_lib.get_cell("FILLER_BLANK_srambank_32_v4_221104");
    
    // Log what we found
    if (filler_lib.filler_blank_6t122) LOGI << "  Found: FILLER_BLANK_6t122";
    else LOGW << "  Missing: FILLER_BLANK_6t122";
    
    if (filler_lib.dummy_corner_v2_lr) LOGI << "  Found: dummy_corner_v2_lr";
    else LOGW << "  Missing: dummy_corner_v2_lr";
    
    if (filler_lib.dummy_vertical_6t122_lr) LOGI << "  Found: dummy_vertical_6t122_lr";
    else LOGW << "  Missing: dummy_vertical_6t122_lr";
    
    if (filler_lib.dummy_corner_lr) LOGI << "  Found: dummy_corner_lr";
    else LOGW << "  Missing: dummy_corner_lr (optional)";
    
    if (filler_lib.tapcell_dummy_6t122_lr) LOGI << "  Found: tapcell_dummy_6t122_lr";
    else LOGW << "  Missing: tapcell_dummy_6t122_lr";
    
    if (filler_lib.filler_blank_colgrp_topnbot) LOGI << "  Found: FILLER_BLANK_COLGRP_TOPnBOT";
    else LOGI << "  Missing: FILLER_BLANK_COLGRP_TOPnBOT (will be created)";
    
    return filler_lib.is_valid();
}

// ============================================================================
// Add filler cells to library with dependencies
// ============================================================================
void add_filler_cells_to_library(
    gdstk::Library& output_lib,
    gdstk::Cell* filler_top,
    gdstk::Cell* filler_bottom,
    const FillerCellLibrary& filler_lib)
{
    LOGI << "Adding filler cells to output library...";
    
    // Helper to add cell and dependencies
    auto add_cell_with_deps = [&output_lib](gdstk::Cell* cell) {
        if (cell == nullptr) return;
        
        // Add dependencies first
        gdstk::Map<gdstk::Cell*> deps = {};
        cell->get_dependencies(true, deps);
        
        for (auto* item = deps.next(nullptr); item != nullptr; item = deps.next(item)) {
            if (output_lib.get_cell(item->value->name) == nullptr) {
                gdstk::Cell* dep_copy = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
                dep_copy->copy_from(*item->value, nullptr, true);
                output_lib.cell_array.append(dep_copy);
            }
        }
        deps.clear();
        
        // Add cell itself
        if (output_lib.get_cell(cell->name) == nullptr) {
            output_lib.cell_array.append(cell);
        }
    };
    
    // Add base cells
    add_cell_with_deps(filler_lib.filler_blank_6t122);
    add_cell_with_deps(filler_lib.dummy_corner_v2_lr);
    add_cell_with_deps(filler_lib.dummy_vertical_6t122_lr);
    add_cell_with_deps(filler_lib.dummy_corner_lr);
    add_cell_with_deps(filler_lib.tapcell_dummy_6t122_lr);
    add_cell_with_deps(filler_lib.filler_blank_colgrp_topnbot);
    add_cell_with_deps(filler_lib.filler_blank_srambank);
    
    // Add filler cells
    if (filler_top != nullptr) {
        add_cell_with_deps(filler_top);
        LOGI << "  Added: " << filler_top->name;
    }
    
    if (filler_bottom != nullptr) {
        add_cell_with_deps(filler_bottom);
        LOGI << "  Added: " << filler_bottom->name;
    }
}

}  // namespace OpenFinRAM
