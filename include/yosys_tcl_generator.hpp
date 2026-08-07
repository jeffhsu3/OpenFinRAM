#ifndef OPENFINRAM_YOSYS_TCL_GENERATOR_HPP_
#define OPENFINRAM_YOSYS_TCL_GENERATOR_HPP_

#include <string>
#include <vector>

namespace OpenFinRAM {

/**
 * Yosys script generator for single-port ctrl_decode.
 * Produces a .ys script that is functionally equivalent to
 * SynthesisManager::generate_tcl_content() but for Yosys + ASAP7 liberty.
 *
 * Contract:
 *  - input RTL: tech/verilog_sp/sram_control.v + delay_cell.v (single-port)
 *  - params: ADDR_WIDTH, NUM_WL, NUM_BANK via chparam
 *  - liberty: tech/lib/asap7sc7p5t_*.lib or platform/asap7/lib/*.lib fallback
 *  - outputs: netlist.v, timing.sdc, qor_report.txt under tmp/syn_<ts>/
 */
class YosysTclGenerator {
public:
    YosysTclGenerator() = default;
    ~YosysTclGenerator() = default;

    // Resolve liberty list: prefer platform_path/lib, fallback to tech/lib
    static std::vector<std::string> resolve_liberty_files(
        const std::string& platform_path,
        const std::string& tech_lib_path);

    // Build the Yosys synthesis script content
    std::string generate_script(
        const std::string& rtl_path,
        const std::string& syn_path,
        const std::string& param_str,
        int addr_width,
        unsigned num_wls,
        unsigned num_banks,
        unsigned num_wl_buf,
        unsigned num_sae_buf,
        double abc_load_ff,
        double abc_delay_ps,
        const std::string& platform_path,
        const std::string& tech_lib_path) const;
};

} // namespace OpenFinRAM

#endif // OPENFINRAM_YOSYS_TCL_GENERATOR_HPP_
