#include "spice_generator.hpp"

#include <sstream>
#include <iomanip>

#include "plog/Log.h"

#include "spice_templates.hpp"
#include "utils.hpp"

namespace {
void append_ports(std::vector<std::string>& ports, std::initializer_list<const char*> names) {
    ports.reserve(ports.size() + names.size());
    for (const char* name : names) {
        ports.emplace_back(name);
    }
}

void append_ports(std::vector<std::string>& ports, const std::vector<std::string>& names) {
    ports.insert(ports.end(), names.begin(), names.end());
}

void append_indexed_ports(std::vector<std::string>& ports,
                          const std::string& prefix,
                          int count,
                          const std::string& suffix = "",
                          int offset = 0) {
    ports.reserve(ports.size() + count);
    for (int i = 0; i < count; ++i) {
        ports.emplace_back(prefix + std::to_string(i + offset) + suffix);
    }
}

void append_tokens(std::stringstream& ss, std::initializer_list<const char*> tokens) {
    for (const char* token : tokens) {
        ss << token << ' ';
    }
}

void append_indexed_tokens(std::stringstream& ss,
                           const std::string& prefix,
                           int count,
                           const std::string& suffix = "",
                           int offset = 0) {
    for (int i = 0; i < count; ++i) {
        ss << prefix << (i + offset) << suffix << ' ';
    }
}
} // namespace

namespace OpenFinRAM {
SpiceGenerator::SpiceGenerator(const MainCliOptions& config)
    : config_(config)
{
}

std::string SpiceGenerator::format_ports(const std::vector<std::string>& ports, int max_per_line) {
    if (ports.empty()) return "";
    
    std::stringstream ss;
    ss << ports[0];
    
    for (size_t i = 1; i < ports.size(); ++i) {
        if (i % max_per_line == 0) {
            ss << "\n+ ";
        } else {
            ss << " ";
        }
        ss << ports[i];
    }
    
    return ss.str();
}

std::string SpiceGenerator::create_subckt(const std::string& name,
                                          const std::vector<std::string>& ports,
                                          const std::string& instances) {
    std::stringstream ss;
    ss << ".SUBCKT " << name << " " << format_ports(ports) << "\n";
    ss << instances;
    ss << ".ENDS\n";
    return ss.str();
}

std::string SpiceGenerator::generate_cell_row() {
    std::vector<std::string> ports;
    append_indexed_ports(ports, "WL[", config_.num_wls, "]");
    append_ports(ports, {"BLN", "BL", "VDD", "VSS"});

    std::stringstream instances;
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "X" << i << " WL[" << i << "] BLN BL VDD VSS sram_cell_6t_122\n";
    }
    instances << "X" << config_.num_wls << " BLN VDD VSS dummy_sram_6t122\n";
    
    return create_subckt("sram_cell_row", ports, instances.str());
}

std::string SpiceGenerator::generate_sramcol() {
    std::vector<std::string> ports;
    append_indexed_ports(ports, "WL[", config_.num_wls, "]");
    append_ports(ports, {"BLN", "BL", "VDD", "VSS"});

    std::stringstream instances;
    instances << "M0 69 VSS BLN VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2\n";
    instances << "M1 70 VDD VSS VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2\n";
    instances << "M2 70 VDD VDD VDD pmos_rvt L=2e-08 W=2.7e-08 nfin=1\n";
    instances << "X1 ";
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "WL[" << i << "] ";
    }
    instances << "BLN BL VDD VSS sram_cell_row\n";
    
    std::string cell_name = "sramcol_x" + std::to_string(config_.num_wls) + "_sram_6t122";
    return create_subckt(cell_name, ports, instances.str());
}

std::string SpiceGenerator::generate_array() {
    std::vector<std::string> ports;
    append_indexed_ports(ports, "WL[", config_.num_wls, "]");
    append_indexed_ports(ports, "BLN[", 4, "]");
    append_indexed_ports(ports, "BL[", 4, "]");
    append_ports(ports, {"VDD", "VSS"});

    std::stringstream instances;
    instances << "X0 BLN[0] VDD VSS dummy_topbot_v1\n";
    instances << "X1 BLN[1] VDD VSS dummy_topbot_v1\n";
    instances << "X2 BLN[2] VDD VSS dummy_topbot_v2\n";
    instances << "X3 BLN[3] VDD VSS dummy_topbot_v2\n";
    
    for (int i = 0; i < 4; ++i) {
        instances << "X" << (4 + i) << " ";
        for (int j = 0; j < config_.num_wls; ++j) {
            instances << "WL[" << j << "] ";
        }
        instances << "BLN[" << i << "] BL[" << i << "] VDD VSS sram_cell_row\n";
    }
    
    return create_subckt("array_sram_6t122", ports, instances.str());
}

std::string SpiceGenerator::generate_colgrp() {
    std::vector<std::string> ports;
    append_indexed_ports(ports, "WLT[", config_.num_wls, "]");
    append_indexed_ports(ports, "WLB[", config_.num_wls, "]");
    append_indexed_ports(ports, "yseltn[", 4, "]");
    append_indexed_ports(ports, "yselt[", 4, "]");
    append_indexed_ports(ports, "yselbn[", 4, "]");
    append_indexed_ports(ports, "yselb[", 4, "]");
    append_ports(ports, {"D", "Q", "wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
                         "blprechtn", "blprechbn", "VDD", "VSS"});

    std::stringstream instances;
    instances << "X0 ";
    append_indexed_tokens(instances, "WLT[", config_.num_wls, "]");
    instances << "BLTN[0] BLTN[1] BLTN[2] BLTN[3] BLT[0] BLT[1] BLT[2] BLT[3] VDD VSS array_sram_6t122\n";
    instances << "X1 ";
    append_indexed_tokens(instances, "WLB[", config_.num_wls, "]");
    instances << "BLBN[0] BLBN[1] BLBN[2] BLBN[3] BLB[0] BLB[1] BLB[2] BLB[3] VDD VSS array_sram_6t122\n";
    instances << "X2 ";
    append_tokens(instances, {"wrenan", "wrena", "SAE", "SAPRECHN", "oeb_out", "oe_out", "D", "Q"});
    instances << "bltn[0] bltn[1] bltn[2]\n";
    instances << "+ bltn[3] blt[0] blt[1] blt[2] blt[3] blbn[0] blbn[1] blbn[2] blbn[3] blb[0]\n";
    instances << "+ blb[1] blb[2] blb[3] BLPRECHTN BLPRECHBN yseltn[0] yseltn[1] yseltn[2] yseltn[3] yselt[0]\n";
    instances << "+ yselt[1] yselt[2] yselt[3] yselbn[0] yselbn[1] yselbn[2] yselbn[3] yselb[0] yselb[1] yselb[2]\n";
    instances << "+ yselb[3] vdd vss iocolgrp_sram_6t122_v2\n";
    
    return create_subckt("colgrp_sram_6t122", ports, instances.str());
}

std::string SpiceGenerator::generate_stacked_colgrp() {
    std::vector<std::string> ports;
    append_indexed_ports(ports, "WLT[", config_.num_wls, "]");
    append_indexed_ports(ports, "WLB[", config_.num_wls, "]");
    append_indexed_ports(ports, "yseltn[", 4, "]");
    append_indexed_ports(ports, "yselt[", 4, "]");
    append_indexed_ports(ports, "yselbn[", 4, "]");
    append_indexed_ports(ports, "yselb[", 4, "]");
    append_indexed_ports(ports, "D[", config_.num_data_bits / 2, "]");
    append_indexed_ports(ports, "Q[", config_.num_data_bits / 2, "]");
    append_ports(ports, {"wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
                         "blprechtn", "blprechbn", "VDD", "VSS"});

    std::stringstream instances;
    for (int bit = 0; bit < config_.num_data_bits / 2; ++bit) {
        instances << "X" << bit << " ";
        append_indexed_tokens(instances, "WLT[", config_.num_wls, "]");
        append_indexed_tokens(instances, "WLB[", config_.num_wls, "]");
        append_indexed_tokens(instances, "yseltn[", 4, "]");
        append_indexed_tokens(instances, "yselt[", 4, "]");
        append_indexed_tokens(instances, "yselbn[", 4, "]");
        append_indexed_tokens(instances, "yselb[", 4, "]");
        instances << "D[" << bit << "] Q[" << bit << "] ";
        instances << "wrena wrenan saprechn sae oeb_out oe_out blprechtn blprechbn VDD VSS ";
        instances << "colgrp_sram_6t122\n";
    }
    
    // Generate bank name matching GDS: stacked_colgrp_x{bits}x{num_data_bits / 2}
    // bits = num_wls (since each colgrp has 2 arrays, each with num_wls)
    std::string bank_name = "stacked_colgrp_x" + std::to_string(config_.num_wls * 2) + "x" + std::to_string(config_.num_data_bits / 2);
    return create_subckt(bank_name, ports, instances.str());
}

std::string SpiceGenerator::generate_stacked_colgrp_mux() {
    std::vector<std::string> ports;
    for (int mux = 0; mux < config_.num_banks; ++mux) {
        append_indexed_ports(ports, "WLT[", config_.num_wls, "]", mux * config_.num_wls);
        append_indexed_ports(ports, "WLB[", config_.num_wls, "]", mux * config_.num_wls);
    }

    append_indexed_ports(ports, "D[", config_.num_data_bits / 2, "]");
    append_indexed_ports(ports, "Q[", config_.num_data_bits / 2, "]");

    const std::vector<std::string> ctrl_port_names = {
        "wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
        "blprechtn", "blprechbn"
    };
    for (const auto& name : ctrl_port_names) {
        for (int mux = 0; mux < config_.num_banks; ++mux) {
            ports.push_back(name + "[" + std::to_string(mux) + "]");
        }
    }

    for (int mux = 0; mux < config_.num_banks; ++mux) {
        append_indexed_ports(ports, "yseltn[", 4, "]", mux * 4);
        append_indexed_ports(ports, "yselt[", 4, "]", mux * 4);
        append_indexed_ports(ports, "yselbn[", 4, "]", mux * 4);
        append_indexed_ports(ports, "yselb[", 4, "]", mux * 4);
    }
    append_ports(ports, {"VDD", "VSS"});

    std::stringstream instances;
    for (int mux = 0; mux < config_.num_banks; ++mux) {
        for (int bit = 0; bit < config_.num_data_bits / 2; ++bit) {
            instances << "X" << mux << "_" << bit << " ";
            append_indexed_tokens(instances, "WLT[", config_.num_wls, "]", mux * config_.num_wls);
            append_indexed_tokens(instances, "WLB[", config_.num_wls, "]", mux * config_.num_wls);
            append_indexed_tokens(instances, "yseltn[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yselt[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yselbn[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yselb[", 4, "]", mux * 4);
            instances << "D[" << bit << "] Q[" << bit << "] ";
            instances << "wrena[" << mux << "] wrenan[" << mux << "] saprechn[" << mux << "] sae[" << mux << "] oeb_out[" << mux << "] oe_out[" << mux << "] blprechtn[" << mux << "] blprechbn[" << mux << "] VDD VSS ";
            instances << "colgrp_sram_6t122\n";
        }
    }
    
    // Generate bank name matching GDS: stacked_colgrp_x{bits}x{num_data_bits / 2}
    // bits = num_wls (since each colgrp has 2 arrays, each with num_wls)
    std::string bank_name = "stacked_colgrp_x" + std::to_string(config_.num_wls * 2) + "x" + std::to_string(config_.num_data_bits / 2) + "x" + std::to_string(config_.num_banks);
    return create_subckt(bank_name, ports, instances.str());
}

std::string SpiceGenerator::generate_cell_row_8t() {
    std::vector<std::string> ports;
    append_indexed_ports(ports, "WLA[", config_.num_wls, "]");
    append_indexed_ports(ports, "WLB[", config_.num_wls, "]");
    append_ports(ports, {"BLA", "BLAN", "BLB", "BLBN", "VDD", "VSS"});

    std::stringstream instances;
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "X" << i << " WLA[" << i << "] WLB[" << i << "] BLA BLAN BLB BLBN VDD VSS sram_cell_8t\n";
    }

    instances << "X" << config_.num_wls + 1 << " BLA BLAN BLB BLBN VDD VSS dummy_cell_8t\n";

    return create_subckt("sram_cell_row_8t", ports, instances.str());
}

std::string SpiceGenerator::generate_array_8t() {
    std::vector<std::string> ports;
    append_indexed_ports(ports, "WLA[", config_.num_wls, "]");
    append_indexed_ports(ports, "WLB[", config_.num_wls, "]");
    append_indexed_ports(ports, "BLA[", 4, "]");
    append_indexed_ports(ports, "BLAN[", 4, "]");
    append_indexed_ports(ports, "BLB[", 4, "]");
    append_indexed_ports(ports, "BLBN[", 4, "]");
    append_ports(ports, {"VDD", "VSS"});

    std::stringstream instances;
    for (int i = 0; i < 4; ++i) {
        instances << "X" << i << " ";
        for (int j = 0; j < config_.num_wls; ++j) {
            instances << "WLA[" << j << "] ";
        }
        for (int j = 0; j < config_.num_wls; ++j) {
            instances << "WLB[" << j << "] ";
        }
        instances << "BLA[" << i << "] BLAN[" << i << "] BLB[" << i << "] BLBN[" << i << "] VDD VSS sram_cell_row_8t\n";
    }

    return create_subckt("array_sram_8t", ports, instances.str());
}

std::string SpiceGenerator::generate_colgrp_8t() {
    std::vector<std::string> ports;
    append_indexed_ports(ports, "WLTA[", config_.num_wls, "]");
    append_indexed_ports(ports, "WLTB[", config_.num_wls, "]");
    append_indexed_ports(ports, "WLBA[", config_.num_wls, "]");
    append_indexed_ports(ports, "WLBB[", config_.num_wls, "]");
    append_ports(ports, {"DA", "QA", "QB"});

    const std::vector<std::string> ctrl_port_names = {
        "wrenaA", "wrenanA",
        "oeb_outA", "oe_outA", "oeb_outB", "oe_outB",
        "blprechtnA", "blprechbnA", "blprechtnB", "blprechbnB"
    };
    append_ports(ports, ctrl_port_names);

    append_indexed_ports(ports, "yseltnA[", 4, "]");
    append_indexed_ports(ports, "yseltA[", 4, "]");
    append_indexed_ports(ports, "yselbnA[", 4, "]");
    append_indexed_ports(ports, "yselbA[", 4, "]");
    append_indexed_ports(ports, "yseltnB[", 4, "]");
    append_indexed_ports(ports, "yseltB[", 4, "]");
    append_indexed_ports(ports, "yselbnB[", 4, "]");
    append_indexed_ports(ports, "yselbB[", 4, "]");
    append_ports(ports, {"sae_A", "sae_B", "VDD", "VSS"});

    std::stringstream instances;
    instances << "X0 ";
    append_indexed_tokens(instances, "WLTA[", config_.num_wls, "]");
    append_indexed_tokens(instances, "WLTB[", config_.num_wls, "]");
    append_indexed_tokens(instances, "BLT_A[", 4, "]");
    append_indexed_tokens(instances, "BLTN_A[", 4, "]");
    append_indexed_tokens(instances, "BLT_B[", 4, "]");
    append_indexed_tokens(instances, "BLTN_B[", 4, "]");
    instances << " VDD VSS array_sram_8t\n";

    instances << "X1 ";
    append_indexed_tokens(instances, "WLBA[", config_.num_wls, "]");
    append_indexed_tokens(instances, "WLBB[", config_.num_wls, "]");
    append_indexed_tokens(instances, "BLB_A[", 4, "]");
    append_indexed_tokens(instances, "BLBN_A[", 4, "]");
    append_indexed_tokens(instances, "BLB_B[", 4, "]");
    append_indexed_tokens(instances, "BLBN_B[", 4, "]");
    instances << " VDD VSS array_sram_8t\n";

    instances << "X2 wrenaA wrenanA oeb_outA oe_outA ";
    instances << "DA QA oeb_outB oe_outB QB ";
    append_indexed_tokens(instances, "BLT_A[", 4, "]");
    append_indexed_tokens(instances, "BLTN_A[", 4, "]");
    append_indexed_tokens(instances, "BLB_A[", 4, "]");
    append_indexed_tokens(instances, "BLBN_A[", 4, "]");
    append_indexed_tokens(instances, "BLT_B[", 4, "]");
    append_indexed_tokens(instances, "BLTN_B[", 4, "]");
    append_indexed_tokens(instances, "BLB_B[", 4, "]");
    append_indexed_tokens(instances, "BLBN_B[", 4, "]");

    instances << "blprechtnA blprechbnA blprechtnB blprechbnB ";
    append_indexed_tokens(instances, "yseltnA[", 4, "]");
    append_indexed_tokens(instances, "yseltA[", 4, "]");
    append_indexed_tokens(instances, "yselbnA[", 4, "]");
    append_indexed_tokens(instances, "yselbA[", 4, "]");
    append_indexed_tokens(instances, "yseltnB[", 4, "]");
    append_indexed_tokens(instances, "yseltB[", 4, "]");
    append_indexed_tokens(instances, "yselbnB[", 4, "]");
    append_indexed_tokens(instances, "yselbB[", 4, "]");
    instances << "sae_A sae_B VDD VSS iocolgrp_sram_8t\n";

    return create_subckt("colgrp_sram_8t", ports, instances.str());
}

std::string SpiceGenerator::generate_stacked_colgrp_8t() {
    std::vector<std::string> ports;
    for (int mux = 0; mux < config_.num_banks; ++mux) {
        append_indexed_ports(ports, "WLTA[", config_.num_wls, "]", mux * config_.num_wls);
        append_indexed_ports(ports, "WLTB[", config_.num_wls, "]", mux * config_.num_wls);
        append_indexed_ports(ports, "WLBA[", config_.num_wls, "]", mux * config_.num_wls);
        append_indexed_ports(ports, "WLBB[", config_.num_wls, "]", mux * config_.num_wls);
    }

    append_indexed_ports(ports, "DA[", config_.num_data_bits / 2, "]");
    append_indexed_ports(ports, "QA[", config_.num_data_bits / 2, "]");
    append_indexed_ports(ports, "QB[", config_.num_data_bits / 2, "]");

    const std::vector<std::string> ctrl_port_names = {
        "wrenaA", "wrenanA",
        "oeb_outA", "oe_outA", "oeb_outB", "oe_outB", 
        "blprechtnA", "blprechbnA", "blprechtnB", "blprechbnB",
        "sae_A", "sae_B"
    };
    for (const auto& name : ctrl_port_names) {
        for (int mux = 0; mux < config_.num_banks; ++mux) {
            ports.push_back(name + "[" + std::to_string(mux) + "]");
        }
    }

    append_indexed_ports(ports, "yseltnA[", 4 * config_.num_banks, "]");
    append_indexed_ports(ports, "yseltA[", 4 * config_.num_banks, "]");
    append_indexed_ports(ports, "yselbnA[", 4 * config_.num_banks, "]");
    append_indexed_ports(ports, "yselbA[", 4 * config_.num_banks, "]");
    append_indexed_ports(ports, "yseltnB[", 4 * config_.num_banks, "]");
    append_indexed_ports(ports, "yseltB[", 4 * config_.num_banks, "]");
    append_indexed_ports(ports, "yselbnB[", 4 * config_.num_banks, "]");
    append_indexed_ports(ports, "yselbB[", 4 * config_.num_banks, "]");
    append_ports(ports, {"VDD", "VSS"});

    std::stringstream instances;
    for (int mux = 0; mux < config_.num_banks; ++mux) {
        for (int bit = 0; bit < config_.num_data_bits / 2; ++bit) {
            instances << "X" << mux << "_" << bit << " ";
            append_indexed_tokens(instances, "WLTA[", config_.num_wls, "]", mux * config_.num_wls);
            append_indexed_tokens(instances, "WLTB[", config_.num_wls, "]", mux * config_.num_wls);
            append_indexed_tokens(instances, "WLBA[", config_.num_wls, "]", mux * config_.num_wls);
            append_indexed_tokens(instances, "WLBB[", config_.num_wls, "]", mux * config_.num_wls);

            instances << "DA[" << bit << "] QA[" << bit << "] QB[" << bit << "] ";
            instances << "wrenaA[" << mux << "] wrenanA[" << mux
                      << "] oeb_outA[" << mux << "] oe_outA[" << mux << "] oeb_outB[" << mux << "] oe_outB[" << mux 
                      << "] blprechtnA[" << mux << "] blprechbnA[" << mux << "] blprechtnB[" << mux << "] blprechbnB[" << mux
                      << "] ";

            append_indexed_tokens(instances, "yseltnA[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yseltA[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yselbnA[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yselbA[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yseltnB[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yseltB[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yselbnB[", 4, "]", mux * 4);
            append_indexed_tokens(instances, "yselbB[", 4, "]", mux * 4);

            instances << "sae_A[" << mux << "] sae_B[" << mux << "] ";
            instances << " VDD VSS colgrp_sram_8t\n";
        }
    }

    // Generate bank name matching GDS: stacked_colgrp_x{bits}x{num_data_bits / 2}x{num_banks}
    // bits = num_wls (since each colgrp has 2 arrays, each with num_wls)
    std::string bank_name = "stacked_colgrp_x" + std::to_string(config_.num_wls * 2) + "x" + std::to_string(config_.num_data_bits / 2) + "x" + std::to_string(config_.num_banks);
    return create_subckt(bank_name, ports, instances.str());
}

std::string SpiceGenerator::generate_spice_content(bool single_port) {
    std::stringstream content;
    std::string sep = std::string(70, '*') + "\n";
    
    if (single_port){
        LOGD << "Generating single-port SRAM SPICE netlist...";

        // Add all basic cell templates
        content << sep << SpiceTemplates::get_cell_6t() << "\n\n";
        content << sep << SpiceTemplates::get_dummy_cell() << "\n\n";
        content << sep << SpiceTemplates::get_dummy_topbot_v1() << "\n\n";
        content << sep << SpiceTemplates::get_dummy_topbot_v2() << "\n\n";
        content << sep << SpiceTemplates::get_prech_v1() << "\n\n";
        content << sep << SpiceTemplates::get_prech_v2() << "\n\n";
        content << sep << SpiceTemplates::get_prech_ymux() << "\n\n";
        content << sep << SpiceTemplates::get_io_nand() << "\n\n";
        content << sep << SpiceTemplates::get_tbuf() << "\n\n";
        content << sep << SpiceTemplates::get_write_driver() << "\n\n";
        content << sep << SpiceTemplates::get_sense_amp() << "\n\n";
        content << sep << SpiceTemplates::get_iocolgrp() << "\n\n";
        
        // Generate hierarchy
        content << sep << generate_cell_row() << "\n";
        content << sep << generate_sramcol() << "\n";
        content << sep << generate_array() << "\n";
        content << sep << generate_colgrp() << "\n";
        // content << sep << generate_stacked_colgrp() << "\n";
        content << sep << generate_stacked_colgrp_mux() << "\n";
    } else {
        LOGD << "Generating dual-port SRAM SPICE netlist...";

        content << sep << SpiceTemplates::get_cell_8t() << "\n\n";
        content << sep << SpiceTemplates::get_dummy_cell_8t() << "\n\n";
        content << sep << SpiceTemplates::get_replica_cell_8t() << "\n\n";

        content << sep << SpiceTemplates::get_prech_8t_v1() << "\n\n";
        content << sep << SpiceTemplates::get_prech_8t_v2() << "\n\n";
        content << sep << SpiceTemplates::get_wrasst_prech_ymux_x8_sram_8t() << "\n\n";

        content << sep << SpiceTemplates::get_write_driver() << "\n\n";
        content << sep << SpiceTemplates::get_sense_amp() << "\n\n";
        content << sep << SpiceTemplates::get_skewed_inv() << "\n\n";
        // content << sep << SpiceTemplates::get_or2() << "\n\n";
        content << sep << SpiceTemplates::get_buf() << "\n\n";
        content << sep << SpiceTemplates::get_io_nand() << "\n\n";
        content << sep << SpiceTemplates::get_tbuf() << "\n\n";
        content << sep << SpiceTemplates::get_iocolgrp_8t(20) << "\n\n";

        content << sep << generate_cell_row_8t() << "\n";
        content << sep << generate_array_8t() << "\n";
        content << sep << generate_colgrp_8t() << "\n";
        content << sep << generate_stacked_colgrp_8t() << "\n";
    }
    
    return content.str();
}

bool SpiceGenerator::generate() {
    std::string output_path = join_path(get_current_dir_name(), "tmp/sram_colgrp_" + get_run_timestamp() + ".sp");
    std::string content = generate_spice_content(config_.single_port);
    
    std::ofstream outfile(output_path);
    if (!outfile.is_open()) {
        LOGE << "Failed to open output file: " << output_path;
        return false;
    }
    
    outfile << content;
    outfile.close();
    
    LOGI << "✓ Generated SPICE netlist: " << output_path;
    LOGI << "  Number of wordlines: " << config_.num_wls;
    LOGI << "  Number of data bits: " << config_.num_data_bits / 2;
    LOGI << "  Number of banks: " << config_.num_banks;
        
    return true;
}

} // namespace OpenFinRAM
