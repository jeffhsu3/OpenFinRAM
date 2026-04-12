#pragma once

#include <cstdint>

#include "gdstk/gdstk.hpp"
#include "layermap.hpp"

gdstk::Cell* create_sram_column(
    gdstk::Cell* sram_cell,
    gdstk::Cell* dummy_cell,
    gdstk::Cell* tapcell,
    uint64_t num_bits,
    const OpenFinRAM::LayerMap& layer_map);

gdstk::Cell* create_sram_array(
    gdstk::Cell* sram_col,
    gdstk::Cell* dummy_topbot_v1,
    gdstk::Cell* dummy_topbot_v2,
    uint64_t num_rows,
    const OpenFinRAM::LayerMap& layer_map);

gdstk::Cell* create_colgrp(
    gdstk::Cell* sram_array,
    gdstk::Cell* filler_cgedge,
    gdstk::Cell* io_colgrp,
    int bits,
    const OpenFinRAM::LayerMap& layer_map);

gdstk::Cell* create_stacked_colgrp(
    gdstk::Cell* colgrp,
    uint64_t num_rows,
    const char* stacked_name,
    const OpenFinRAM::LayerMap& layer_map);

gdstk::Cell* create_muxed_colgrp(
    gdstk::Cell* stacked_colgrp,
    uint64_t num_mux,
    const char* muxed_name,
    const OpenFinRAM::LayerMap& layer_map);
