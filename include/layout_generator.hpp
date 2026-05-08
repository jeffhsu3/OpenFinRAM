#ifndef LAYOUT_GENERATOR_HPP
#define LAYOUT_GENERATOR_HPP

#include <string>

#include "gdstk/gdstk.hpp"

#include "main_config_helpers.hpp"
#include "layermap.hpp"

struct SramCellSet {
    gdstk::Cell* filler      = nullptr;
    gdstk::Cell* bitcell     = nullptr;
    gdstk::Cell* dummy       = nullptr;
    gdstk::Cell* tapcell     = nullptr;
    gdstk::Cell* dummy_v1    = nullptr;
    gdstk::Cell* dummy_v2    = nullptr;
    gdstk::Cell* cgedge      = nullptr;
    gdstk::Cell* io_colgrp   = nullptr;

    gdstk::Cell* sram_column = nullptr;
    gdstk::Cell* array_cell  = nullptr;
    gdstk::Cell* colgrp      = nullptr;
    gdstk::Cell* stacked_colgrp = nullptr;
    gdstk::Cell* muxed_colgrp = nullptr;
};

class LayoutGenerator {
public:
    LayoutGenerator(const MainCliOptions& options, const OpenFinRAM::LayerMap& layer_map);

    bool load_sram_gds();

    bool extract_required_cells();

    bool gen_layout();

    bool create_sram_column();

    bool create_sram_array();

    bool create_colgrp();

    bool create_stacked_colgrp();

    bool create_muxed_colgrp();

    bool write_layout();

    bool add_ctrl_decode_gate_fin_wrappers();

    bool create_and_add_sram_filler_cells();

    bool run_sram_gds_integration_and_writeback();

    // 將 cell 及其相依 cell 複製到目標 Library（避免重複加入）
    void add_cell_with_deps(gdstk::Library& target_lib, gdstk::Cell* source_cell);

private:
    MainCliOptions cli_options_;

    OpenFinRAM::LayerMap layer_map_;

    gdstk::Library sram_lib;

    SramCellSet sram_cells_;

    gdstk::Library ctrl_decode_gds;
};

#endif // LAYOUT_GENERATOR_HPP