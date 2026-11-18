#ifndef MAIN_CONFIG_HELPERS_HPP
#define MAIN_CONFIG_HELPERS_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct MainCliOptions {
    unsigned num_wls = 2;
    unsigned num_data_bits = 2;
    unsigned num_banks = 1;
    bool single_port = false;
    // The generated single-port macro (--bitcell 6t): the released 6T array
    // with chipforge_asap7's 270 nm column IO, built by the two-port machinery
    // with port A alone.  `single_port` stays the legacy srambank flow.
    bool bitcell_6t = false;
    // Divided wordlines (--segment-bits): each stack of data bits cut into
    // segments of this many bits, a mid strip pair between two.  0: whole
    // stacks.
    unsigned segment_bits = 0;
    // The bits of one wordline segment, and the segments in a stack.
    unsigned wordline_segment_bits() const {
        return segment_bits ? segment_bits : num_data_bits / 2;
    }
    unsigned wordline_segments() const {
        return wordline_segment_bits() ? (num_data_bits / 2) / wordline_segment_bits() : 1;
    }
    // Outer strips (--outer-strips, generated 6T, standard floorplan): one
    // more strip at each stack's far end, so the segment farthest from the
    // band is driven from both ends like the others and every driver slice
    // is sized for half a segment.  A segment twice as long then has the
    // wordline delay of one driven from one end, with half the strips.
    bool outer_strips = false;
    // The wordline driver strips of one bank: one on each side of the
    // controller band, and a mid pair's two between every two segments of a
    // stack (a segment has a strip at each end, but the one farthest from
    // the band, unless the stacks have outer strips).
    unsigned wordline_strips() const {
        return 2 * (1 + 2 * (wordline_segments() - 1) + (outer_strips ? 1 : 0));
    }
    // The bitcells along a wordline one driver slice is sized for: a
    // segment's, or half of them when both its ends are driven.
    unsigned wordline_slice_cells() const {
        const unsigned cells = num_rows_per_mux * wordline_segment_bits();
        return outer_strips ? (cells + 1) / 2 : cells;
    }
    // Select buffers (--select-buffers, generated 6T): every strip re-drives
    // the predecode lines (sel_lo) for its own slices from a row of standard
    // buffers under them, so the controller's four outputs see a buffer
    // input per group of slices instead of every slice of every strip.
    // parseMainCliOptions resolves "auto".
    bool select_buffers = false;
    // Precharge buffers (--precharge-buffers, generated 6T): one more buffer
    // in every strip's row re-drives blprechn for the wordline segment the
    // strip serves, so the controller's output sees a buffer input a strip
    // instead of every column's precharge gates.  They stand in the select
    // buffers' row and turn those on.  parseMainCliOptions resolves "auto".
    bool precharge_buffers = false;
    // Two-port only: banks in pairs share port B's IO block, the pair's
    // arrays mirrored about it (--share-port-b).
    bool share_port_b = false;
    // Hidden compatibility flag; generated 6T still opts into the controller band.
    bool strips_in_controller = false;
    bool use_controller_band() const {
        return !single_port && (strips_in_controller ||
            (!bitcell_6t && (use_openroad || openroad_only)));
    }
    // Address bits per predecode group of the controller's slice decode
    // (sel_hi): 2, 3 or 4 (--row-predecode-bits).
    unsigned row_predecode_bits = 3;
    bool skip_characterization = true;
    std::string output_sp_name = "sram.sp";
    std::string output_gds_name = "sram.gds";
    bool spice_only = false;
    unsigned num_wl_buf = 5;
    unsigned num_sae_buf = 10;
    // Open-source flow options (Yosys / OpenROAD / OpenSTA)
    bool use_yosys = false;       // use Yosys instead of Design Compiler
    bool use_openroad = false;    // use OpenROAD instead of Innovus
    bool openroad_only = false;   // alias: enable both use_yosys + use_openroad for single-port ASAP7
    std::string openroad_path = ""; // OpenROAD binary; default resolves 'openroad' from $PATH
    std::string platform_path = ""; // path to ASAP7 platform (default: <openroad>/platform/asap7)
    // Characterization JSON feeding measured values into the Liberty emitter
    // (--liberty-from). Empty keeps the pure estimated-constant model.
    std::string liberty_from_json = "";
    // Export against an existing LEF without regenerating the macro.
    std::string liberty_only_lef;
    std::string liberty_output;
    
    // Custom Push-Rule Bitcell configuration
    double bitcell_width = 0.108;
    double bitcell_height = 0.27;
    unsigned num_rows_per_mux = 4;
};

MainCliOptions parseMainCliOptions(int argc, char** argv);

// The generated single-port (--bitcell 6t) controller's outputs fan out
// across the whole array: sel_lo to every driver slice of every strip, the
// IO controls to every IO block.  Their loads in pF, counted in gate fins
// plus a wire allowance per load, by output port prefix (sel_lo_A, sae_A, ...).
std::map<std::string, double> six_t_port_loads_pf(const MainCliOptions& options);

// What one controller output buffer (BUFx24) holds under the periphery's
// 150 ps slew limit, pF: past it on an unbuffered sel_lo, "auto" select
// buffers are on.
constexpr double kSelectLineLimitPf = 0.175;
// A buffered strip of `slices` driver slices: the slices each of its select
// buffers drives (balanced groups of at most 16), and the buffer.  The
// assembler's select_buffer_groups/select_buffer_cell are the same rules.
constexpr unsigned kSelectBufferMinSlices = 4;
std::vector<unsigned> select_buffer_groups(unsigned slices);
std::string select_buffer_cell(unsigned slices);
// A strip's precharge buffer drives one wordline segment's IO blocks (two
// strips share most segments, one drives the segment farthest from the
// band unless the stacks have outer strips): that load in pF, and the
// smallest buffer that holds it near 100 ps of transition, empty when none
// does (more segments are the cure).  The row has room for it past the last
// group's select buffers when the strip's smallest group has
// precharge_buffer_min_group slices (9 to 11, by the cell).  The assembler's
// precharge_buffer_cell and precharge_buffer_fits are the same rules.
double precharge_segment_load_pf(const MainCliOptions& options);
std::string precharge_buffer_cell(const MainCliOptions& options);
unsigned precharge_buffer_min_group(const std::string& cell);
bool precharge_buffer_fits(unsigned slices, const std::string& cell);

// A read: the controller raises the gated wordline select (sel_hi) and, a
// chain of buffers later (--num-sae-buf), the sense enable.  What the array
// needs between the two at the controller's ports, ps, all at TT:
//  * wordline_ps: a driver slice brings the far end of its wordline to half
//    swing after its select (Xyce: the slice classes on released 6T cells,
//    the M3 bar at 233 ohm/um; a segment driven from both ends is half of
//    one driven from one);
//  * bitline_ps: the pair then splits kSenseSplitMv at the IO block
//    (scripts/sense_timing_6t.py: the 4:1 block, the bitline bar's own RC,
//    the far cell read against the leakage of every other);
//  * select_buffer_ps: what a strip's select buffer puts on sel_lo before
//    its slices see it (--select-buffers; Liberty at a 50 ps input edge and
//    the group's counted load).  It only delays the wordline if sel_lo is
//    the later of the two selects.
// The controller's place-and-route holds its post-route STA to them
// (OpenRoadTclGenerator::set_sense_gap).  The lines from the ports to the
// strips and to the IO blocks run the same length and are not counted.
constexpr double kSenseSplitMv = 100.0;
struct SenseGapNeed {
    double wordline_ps = 0.0;
    double bitline_ps = 0.0;
    double select_buffer_ps = 0.0;
    double floor_ps() const { return wordline_ps + bitline_ps; }
};
double wordline_far_end_ps(unsigned cells);
double bitline_split_ps(unsigned cells);
SenseGapNeed sense_gap_need(const MainCliOptions& options);

#endif // MAIN_CONFIG_HELPERS_HPP
