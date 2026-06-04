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
#include "innovus_manager.hpp"
#include "siliconsmart_generator.hpp"
#include "siliconsmart_manager.hpp"
#include "lvs_runner.hpp"
#include "lvs_manager.hpp"
#include "lef_extractor.hpp"
#include "lef_manager.hpp"
#include "layout_generator.hpp"
#include "main_config_helpers.hpp"
#include "main_flow_helpers.hpp"
#include "main_layout_helpers.hpp"
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
// Global layer map instance
// ============================================================================
static OpenFinRAM::LayerMap g_layer_map;


// ============================================================================
// Main
// ============================================================================
int main(int argc, char **argv) {
    // Initialize plog
    static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
    plog::init(plog::debug, &consoleAppender);

    LOGI << "Starting OpenFinRAM application";

    // Parse CLI options
    MainCliOptions cli_options = parseMainCliOptions(argc, argv);

    // Generate SPICE netlist
    LOGI << "=== Generating SPICE Netlist ===";
    OpenFinRAM::SpiceGenerator gen(cli_options);
    if (gen.generate()) {
        LOGI << "SPICE netlist generation completed successfully.";
    } else {
        LOGE << "SPICE netlist generation failed.";
        return 1;
    }

    // Run synthesis flow
    SynthesisManager synth_manager(cli_options);
    if (!synth_manager.run_synthesis()) {
        LOGE << "Synthesis flow failed.";
        return 1;
    }

    // Run Innovus flow
    InnovusManager innovus_manager(cli_options);
    if (!innovus_manager.run_innovus_flow()) {
        LOGE << "Innovus flow failed.";
        return 1;
    }

    SpiceIntegrator integrator(cli_options);
    if (!integrator.integrate_sram()) {
        LOGE << "SRAM integration failed.";
        return 1;
    }

    // Run siliconsmart characterization
    SiliconSmartManager sis_manager(cli_options);
    if (!sis_manager.run_siliconsmart()) {
        LOGE << "SiliconSmart characterization failed.";
        return 1;
    }

    // Initialize ASAP7 Layer Map (hardcoded)
    LOGI << "Initializing ASAP7 layermap (hardcoded)...";
    g_layer_map.init_asap7_layermap();
    
    if (g_layer_map.empty()) {
        LOGE << "Failed to initialize layer map!";
        return 1;
    }

    LayoutGenerator layout_gen(cli_options, g_layer_map);
    if (!layout_gen.gen_layout()) {
        LOGE << "Layout generation failed!";
        return 1;
    }

    // Run LVS (create lvs folder, generate _run_control.svrf, run calibre)
    LvsManager lvs_manager(cli_options);
    if (!lvs_manager.run_lvs()) {
        LOGE << "LVS failed!";
        return 1;
    }

    // Export LEF file for the generated SRAM layout
    LefManager lef_manager(cli_options);
    if (!lef_manager.export_lef()) {
        LOGE << "LEF export failed!";
        return 1;
    }
}
