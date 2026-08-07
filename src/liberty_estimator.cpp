#include "liberty_estimator.hpp"

#include <exception>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>

#include "plog/Log.h"
#include "utils.hpp"

namespace OpenFinRAM {
namespace {

struct MacroSize {
    double width = 0;
    double height = 0;
};

bool read_lef_size(const std::string& lef_path, MacroSize& size, std::string* error) {
    std::ifstream lef(lef_path);
    if (!lef) {
        if (error) *error = "Failed to open LEF: " + lef_path;
        return false;
    }

    const std::regex size_pattern(
        R"(^\s*SIZE\s+([-+0-9.eE]+)\s+BY\s+([-+0-9.eE]+)\s*;)",
        std::regex::icase);
    std::string line;
    std::smatch match;
    while (std::getline(lef, line)) {
        if (!std::regex_search(line, match, size_pattern)) continue;
        try {
            size.width = std::stod(match[1].str());
            size.height = std::stod(match[2].str());
        } catch (const std::exception&) {
            if (error) *error = "Invalid SIZE statement in LEF: " + line;
            return false;
        }
        if (size.width > 0 && size.height > 0) return true;
    }

    if (error) *error = "No valid SIZE statement found in LEF: " + lef_path;
    return false;
}

void emit_constraint_arcs(std::ostringstream& out,
                          const std::string& template_name,
                          const std::string& indent) {
    for (const char* timing_type : {"setup_rising", "hold_rising"}) {
        out << indent << "timing () {\n";
        out << indent << "  related_pin : \"clk\";\n";
        out << indent << "  timing_type : " << timing_type << ";\n";
        out << indent << "  rise_constraint (" << template_name << ") {\n";
        out << indent << "    index_1 (\"0.009, 0.227\");\n";
        out << indent << "    index_2 (\"0.009, 0.227\");\n";
        out << indent << "    values (\"0.050, 0.050\", \"0.050, 0.050\");\n";
        out << indent << "  }\n";
        out << indent << "  fall_constraint (" << template_name << ") {\n";
        out << indent << "    index_1 (\"0.009, 0.227\");\n";
        out << indent << "    index_2 (\"0.009, 0.227\");\n";
        out << indent << "    values (\"0.050, 0.050\", \"0.050, 0.050\");\n";
        out << indent << "  }\n";
        out << indent << "}\n";
    }
}

void emit_input_pin(std::ostringstream& out,
                    const std::string& name,
                    const std::string& constraint_template) {
    out << "    pin (" << name << ") {\n";
    out << "      direction : input;\n";
    out << "      capacitance : 0.005;\n";
    emit_constraint_arcs(out, constraint_template, "      ");
    out << "    }\n";
}

void emit_input_bus(std::ostringstream& out,
                    const std::string& name,
                    const std::string& bus_type,
                    const std::string& constraint_template,
                    bool memory_write) {
    out << "    bus (" << name << ") {\n";
    out << "      bus_type : " << bus_type << ";\n";
    out << "      direction : input;\n";
    out << "      capacitance : 0.005;\n";
    if (memory_write) {
        out << "      memory_write () {\n";
        out << "        address : A;\n";
        out << "        clocked_on : \"clk\";\n";
        out << "      }\n";
    }
    emit_constraint_arcs(out, constraint_template, "      ");
    out << "    }\n";
}

std::string build_estimated_liberty(const MainCliOptions& options,
                                    const MacroSize& size) {
    const std::string cell_name =
        "sram_x" + std::to_string(options.num_wls * 2) + "x" +
        std::to_string(options.num_data_bits) + "x" +
        std::to_string(options.num_banks);
    const int addr_width = get_addr_width(options);
    const std::string data_type = cell_name + "_DATA";
    const std::string address_type = cell_name + "_ADDRESS";
    const std::string sdel_type = cell_name + "_SDEL";
    const std::string delay_template = cell_name + "_estimated_delay";
    const std::string slew_template = cell_name + "_estimated_slew";
    const std::string constraint_template = cell_name + "_estimated_constraint";

    // These are deliberately coarse early-PPA defaults. They match the
    // compact ASAP7 FakeRAM convention (2x2 tables) and are intentionally not
    // scaled until there is a calibrated model for OpenFinRAM's actual
    // decoder, muxing, bit-line, and interconnect architecture.
    constexpr double kClockToQ = 0.218;   // ns
    constexpr double kMinPeriod = 0.157;  // ns

    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << "library (" << cell_name << "_estimated) {\n";
    out << "  technology (cmos);\n";
    out << "  delay_model : table_lookup;\n";
    out << "  revision : \"OpenFinRAM estimated-1\";\n";
    out << "  comment : \"ESTIMATED EARLY-PPA MODEL; NOT SPICE/SILICONSMART CHARACTERIZED. "
           "Timing uses a coarse FakeRAM-style ASAP7 baseline; power is not modeled.\";\n";
    out << "  time_unit : \"1ns\";\n";
    out << "  voltage_unit : \"1V\";\n";
    out << "  current_unit : \"1uA\";\n";
    out << "  leakage_power_unit : \"1uW\";\n";
    out << "  capacitive_load_unit (1, pf);\n";
    out << "  nom_process : 1;\n";
    out << "  nom_temperature : 25;\n";
    out << "  nom_voltage : 0.7;\n";
    out << "  default_cell_leakage_power : 0;\n";
    out << "  default_input_pin_cap : 0.005;\n";
    out << "  default_output_pin_cap : 0.0;\n";
    out << "  default_inout_pin_cap : 0.0;\n";
    out << "  default_max_transition : 0.227;\n";
    out << "  input_threshold_pct_fall : 50;\n";
    out << "  input_threshold_pct_rise : 50;\n";
    out << "  output_threshold_pct_fall : 50;\n";
    out << "  output_threshold_pct_rise : 50;\n";
    out << "  slew_lower_threshold_pct_fall : 20;\n";
    out << "  slew_upper_threshold_pct_fall : 80;\n";
    out << "  slew_lower_threshold_pct_rise : 20;\n";
    out << "  slew_upper_threshold_pct_rise : 80;\n";
    out << "  operating_conditions (PVT_0P7V_25C) {\n";
    out << "    process : 1;\n";
    out << "    temperature : 25;\n";
    out << "    voltage : 0.7;\n";
    out << "    tree_type : balanced_tree;\n";
    out << "  }\n";
    out << "  default_operating_conditions : PVT_0P7V_25C;\n\n";

    out << "  lu_table_template (" << delay_template << ") {\n";
    out << "    variable_1 : input_net_transition;\n";
    out << "    variable_2 : total_output_net_capacitance;\n";
    out << "    index_1 (\"0.009, 0.227\");\n";
    out << "    index_2 (\"0.005, 0.500\");\n";
    out << "  }\n";
    out << "  lu_table_template (" << slew_template << ") {\n";
    out << "    variable_1 : total_output_net_capacitance;\n";
    out << "    index_1 (\"0.005, 0.500\");\n";
    out << "  }\n";
    out << "  lu_table_template (" << constraint_template << ") {\n";
    out << "    variable_1 : related_pin_transition;\n";
    out << "    variable_2 : constrained_pin_transition;\n";
    out << "    index_1 (\"0.009, 0.227\");\n";
    out << "    index_2 (\"0.009, 0.227\");\n";
    out << "  }\n\n";

    auto emit_bus_type = [&](const std::string& name, int width) {
        out << "  type (" << name << ") {\n";
        out << "    base_type : array;\n";
        out << "    data_type : bit;\n";
        out << "    bit_width : " << width << ";\n";
        out << "    bit_from : " << (width - 1) << ";\n";
        out << "    bit_to : 0;\n";
        out << "    downto : true;\n";
        out << "  }\n";
    };
    emit_bus_type(data_type, static_cast<int>(options.num_data_bits));
    emit_bus_type(address_type, addr_width);
    emit_bus_type(sdel_type, 4);
    out << "\n";

    out << "  cell (" << cell_name << ") {\n";
    out << "    area : " << size.width * size.height << ";\n";
    out << "    interface_timing : true;\n";
    out << "    cell_leakage_power : 0;\n";
    out << "    memory () {\n";
    out << "      type : ram;\n";
    out << "      address_width : " << addr_width << ";\n";
    out << "      word_width : " << options.num_data_bits << ";\n";
    out << "    }\n";
    out << "    pg_pin (vdd) {\n";
    out << "      direction : inout;\n";
    out << "      pg_type : primary_power;\n";
    out << "      voltage_name : \"VDD\";\n";
    out << "    }\n";
    out << "    pg_pin (vss) {\n";
    out << "      direction : inout;\n";
    out << "      pg_type : primary_ground;\n";
    out << "      voltage_name : \"VSS\";\n";
    out << "    }\n";
    out << "    pin (clk) {\n";
    out << "      direction : input;\n";
    out << "      capacitance : 0.025;\n";
    out << "      clock : true;\n";
    out << "      min_period : " << kMinPeriod << ";\n";
    out << "    }\n";

    emit_input_pin(out, "ce_n", constraint_template);
    emit_input_pin(out, "oe_n", constraint_template);
    emit_input_pin(out, "we_n", constraint_template);
    emit_input_bus(out, "sdel", sdel_type, constraint_template, false);
    emit_input_bus(out, "A", address_type, constraint_template, false);
    emit_input_bus(out, "D", data_type, constraint_template, true);

    out << "    bus (Q) {\n";
    out << "      bus_type : " << data_type << ";\n";
    out << "      direction : output;\n";
    out << "      max_capacitance : 0.500;\n";
    out << "      memory_read () {\n";
    out << "        address : A;\n";
    out << "      }\n";
    out << "      timing () {\n";
    out << "        related_pin : \"clk\";\n";
    out << "        timing_type : rising_edge;\n";
    out << "        timing_sense : non_unate;\n";
    for (const char* table : {"cell_rise", "cell_fall"}) {
        out << "        " << table << " (" << delay_template << ") {\n";
        out << "          index_1 (\"0.009, 0.227\");\n";
        out << "          index_2 (\"0.005, 0.500\");\n";
        out << "          values (\"" << kClockToQ << ", " << kClockToQ
            << "\", \"" << kClockToQ << ", " << kClockToQ << "\");\n";
        out << "        }\n";
    }
    for (const char* table : {"rise_transition", "fall_transition"}) {
        out << "        " << table << " (" << slew_template << ") {\n";
        out << "          index_1 (\"0.005, 0.500\");\n";
        out << "          values (\"0.009, 0.227\");\n";
        out << "        }\n";
    }
    out << "      }\n";
    out << "    }\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

}  // namespace

bool export_estimated_liberty(const MainCliOptions& options,
                              const std::string& lef_path,
                              const std::string& liberty_path,
                              std::string* error) {
    if (!options.single_port) {
        if (error) {
            *error = "Estimated Liberty generation currently supports the single-port interface only";
        }
        return false;
    }

    MacroSize size;
    if (!read_lef_size(lef_path, size, error)) return false;

    std::ofstream liberty(liberty_path, std::ios::out | std::ios::trunc);
    if (!liberty) {
        if (error) *error = "Failed to open Liberty file for writing: " + liberty_path;
        return false;
    }
    liberty << build_estimated_liberty(options, size);
    liberty.close();
    if (!liberty) {
        if (error) *error = "Failed while writing Liberty file: " + liberty_path;
        return false;
    }

    LOGI << "Estimated Liberty model written to " << liberty_path
         << " (area " << size.width * size.height << " um^2; timing is not characterized)";
    return true;
}

}  // namespace OpenFinRAM
