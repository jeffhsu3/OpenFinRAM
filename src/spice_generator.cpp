#include "spice_generator.hpp"

#include <sstream>
#include <iomanip>

#include "plog/Log.h"

#include "spice_templates.hpp"
#include "utils.hpp"

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
    
    // WL ports
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WL[" + std::to_string(i) + "]");
    }
    ports.push_back("BLN");
    ports.push_back("BL");
    ports.push_back("VDD");
    ports.push_back("VSS");
    
    // Instances
    std::stringstream instances;
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "X" << i << " WL[" << i << "] BLN BL VDD VSS sram_cell_6t_122\n";
    }
    instances << "X" << config_.num_wls << " BLN VDD VSS dummy_sram_6t122\n";
    
    return create_subckt("sram_cell_row", ports, instances.str());
}

std::string SpiceGenerator::generate_sramcol() {
    std::vector<std::string> ports;
    
    // WL ports
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WL[" + std::to_string(i) + "]");
    }
    ports.push_back("BLN");
    ports.push_back("BL");
    ports.push_back("VDD");
    ports.push_back("VSS");
    
    // Instances
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
    
    // WL ports
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WL[" + std::to_string(i) + "]");
    }
    
    // BL ports
    std::vector<std::string> bl_ports = {
        "BLN[0]", "BLN[1]", "BLN[2]", "BLN[3]",
        "BL[0]", "BL[1]", "BL[2]", "BL[3]"
    };
    ports.insert(ports.end(), bl_ports.begin(), bl_ports.end());
    ports.push_back("VDD");
    ports.push_back("VSS");
    
    // Instances
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
    
    // WLT and WLB ports
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLT[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLB[" + std::to_string(i) + "]");
    }
    
    // YSEL ports
    std::vector<std::string> ysel_ports = {
        "yseltn[0]", "yseltn[1]", "yseltn[2]", "yseltn[3]",
        "yselt[0]", "yselt[1]", "yselt[2]", "yselt[3]",
        "yselbn[0]", "yselbn[1]", "yselbn[2]", "yselbn[3]",
        "yselb[0]", "yselb[1]", "yselb[2]", "yselb[3]"
    };
    ports.insert(ports.end(), ysel_ports.begin(), ysel_ports.end());
    
    // Control and data ports
    std::vector<std::string> ctrl_ports = {
        "D", "Q", "wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
        "blprechtn", "blprechbn", "VDD", "VSS"
    };
    ports.insert(ports.end(), ctrl_ports.begin(), ctrl_ports.end());
    
    // Instances
    std::stringstream instances;
    
    // Top array
    instances << "X0 ";
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "WLT[" << i << "] ";
    }
    instances << "BLTN[0] BLTN[1] BLTN[2] BLTN[3] BLT[0] BLT[1] BLT[2] BLT[3] VDD VSS array_sram_6t122\n";
    
    // Bottom array
    instances << "X1 ";
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "WLB[" << i << "] ";
    }
    instances << "BLBN[0] BLBN[1] BLBN[2] BLBN[3] BLB[0] BLB[1] BLB[2] BLB[3] VDD VSS array_sram_6t122\n";
    
    // IO column group
    instances << "X2 wrenan wrena SAE SAPRECHN oeb_out oe_out D Q ";
    instances << "bltn[0] bltn[1] bltn[2]\n";
    instances << "+ bltn[3] blt[0] blt[1] blt[2] blt[3] blbn[0] blbn[1] blbn[2] blbn[3] blb[0]\n";
    instances << "+ blb[1] blb[2] blb[3] BLPRECHTN BLPRECHBN yseltn[0] yseltn[1] yseltn[2] yseltn[3] yselt[0]\n";
    instances << "+ yselt[1] yselt[2] yselt[3] yselbn[0] yselbn[1] yselbn[2] yselbn[3] yselb[0] yselb[1] yselb[2]\n";
    instances << "+ yselb[3] sae_A sae_B vdd vss iocolgrp_sram_6t122_v2\n";
    
    return create_subckt("colgrp_sram_6t122", ports, instances.str());
}

std::string SpiceGenerator::generate_stacked_colgrp() {
    std::vector<std::string> ports;
    
    // WLT and WLB ports
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLT[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLB[" + std::to_string(i) + "]");
    }
    
    // YSEL ports
    std::vector<std::string> ysel_ports = {
        "yseltn[0]", "yseltn[1]", "yseltn[2]", "yseltn[3]",
        "yselt[0]", "yselt[1]", "yselt[2]", "yselt[3]",
        "yselbn[0]", "yselbn[1]", "yselbn[2]", "yselbn[3]",
        "yselb[0]", "yselb[1]", "yselb[2]", "yselb[3]"
    };
    ports.insert(ports.end(), ysel_ports.begin(), ysel_ports.end());
    
    // Data ports
    for (int i = 0; i < config_.num_data_bits / 2; ++i) {
        ports.push_back("D[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_data_bits / 2; ++i) {
        ports.push_back("Q[" + std::to_string(i) + "]");
    }
    
    // Control ports
    std::vector<std::string> ctrl_ports = {
        "wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
        "blprechtn", "blprechbn", "VDD", "VSS"
    };
    ports.insert(ports.end(), ctrl_ports.begin(), ctrl_ports.end());
    
    // Instances
    std::stringstream instances;
    for (int bit = 0; bit < config_.num_data_bits / 2; ++bit) {
        instances << "X" << bit << " ";
        for (int i = 0; i < config_.num_wls; ++i) {
            instances << "WLT[" << i << "] ";
        }
        for (int i = 0; i < config_.num_wls; ++i) {
            instances << "WLB[" << i << "] ";
        }
        for (const auto& ysel : ysel_ports) {
            instances << ysel << " ";
        }
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
        // WLT and WLB ports
        for (int i = 0; i < config_.num_wls; ++i) {
            ports.push_back("WLT[" + std::to_string(i + mux * config_.num_wls) + "]");
        }
        for (int i = 0; i < config_.num_wls; ++i) {
            ports.push_back("WLB[" + std::to_string(i + mux * config_.num_wls) + "]");
        }
    }
    
    // YSEL ports
    std::vector<std::string> ysel_ports;
    for (int mux = 0; mux < config_.num_banks; ++mux) {
        for (int i = 0; i < 4; ++i) {
            ysel_ports.push_back("yseltn[" + std::to_string(i + mux * 4) + "]");
        }
        for (int i = 0; i < 4; ++i) {
            ysel_ports.push_back("yselt[" + std::to_string(i + mux * 4) + "]");
        }
        for (int i = 0; i < 4; ++i) {
            ysel_ports.push_back("yselbn[" + std::to_string(i + mux * 4) + "]");
        }
        for (int i = 0; i < 4; ++i) {
            ysel_ports.push_back("yselb[" + std::to_string(i + mux * 4) + "]");
        }
    }
    ports.insert(ports.end(), ysel_ports.begin(), ysel_ports.end());
    
    // Data ports
    for (int i = 0; i < config_.num_data_bits / 2; ++i) {
        ports.push_back("D[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_data_bits / 2; ++i) {
        ports.push_back("Q[" + std::to_string(i) + "]");
    }
    
    // Control ports
    std::vector<std::string> ctrl_port_names = {
        "wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
        "blprechtn", "blprechbn"
    };
    std::vector<std::string> ctrl_ports;
    for (const auto& name : ctrl_port_names) {
        for (int mux = 0; mux < config_.num_banks; ++mux) {
            ctrl_ports.push_back(name + "[" + std::to_string(mux) + "]");
        }
    }
    ctrl_ports.push_back("VDD");
    ctrl_ports.push_back("VSS");
    ports.insert(ports.end(), ctrl_ports.begin(), ctrl_ports.end());
    
    // Instances
    std::stringstream instances;
    for (int mux = 0; mux < config_.num_banks; ++mux) {
        for (int bit = 0; bit < config_.num_data_bits / 2; ++bit) {
            instances << "X" << mux << "_" << bit << " ";
            for (int i = 0; i < config_.num_wls; ++i) {
                instances << "WLT[" << (i + mux * config_.num_wls) << "] ";
            }
            for (int i = 0; i < config_.num_wls; ++i) {
                instances << "WLB[" << (i + mux * config_.num_wls) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yseltn[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yselt[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yselbn[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yselb[" << (i + mux * 4) << "] ";
            }
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
    
    // WL ports
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLB[" + std::to_string(i) + "]");
    }

    ports.push_back("BLA");
    ports.push_back("BLAN");
    ports.push_back("BLB");
    ports.push_back("BLBN");
    ports.push_back("VDD");
    ports.push_back("VSS");
    
    // sram_cell_row_8t instances
    std::stringstream instances;
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "X" << i << " WLA[" << i << "] WLB[" << i << "] BLA BLAN BLB BLBN VDD VSS sram_cell_8t\n";
    }
    
    // instances << "X" << config_.num_wls     << " RWLA RWLB RBLA RBLB VDD VSS replica_cell_8t\n";
    instances << "X" << config_.num_wls + 1 << " BLA BLAN BLB BLBN VDD VSS dummy_cell_8t\n";
    
    return create_subckt("sram_cell_row_8t", ports, instances.str());
}

std::string SpiceGenerator::generate_array_8t() {
    std::vector<std::string> ports;

    // WL ports
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLB[" + std::to_string(i) + "]");
    }

    // BL ports
    for (int i = 0; i < 4; ++i) {
        ports.push_back("BLA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("BLAN[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("BLB[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("BLBN[" + std::to_string(i) + "]");
    }

    ports.push_back("VDD");
    ports.push_back("VSS");

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

    // WLTA/WLTB and WLBA/WLBB ports
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLTA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLTB[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLBA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        ports.push_back("WLBB[" + std::to_string(i) + "]");
    }

    // D/Q ports
    ports.push_back("DA");
    ports.push_back("QA");
    ports.push_back("QB");

    // wrena/wrenan, oeb_out/oe_out, blprechtn/blprechbn ports for both ports
    std::vector<std::string> ctrl_port_names = {
        "wrenaA", "wrenanA",
        "oeb_outA", "oe_outA", "oeb_outB", "oe_outB",
        "blprechtnA", "blprechbnA", "blprechtnB", "blprechbnB"
    };
    ports.insert(ports.end(), ctrl_port_names.begin(), ctrl_port_names.end());

    // yseltn/yselt/yselbn/yselb ports for both ports
    for (int i = 0; i < 4; ++i) {
        ports.push_back("yseltnA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("yseltA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("yselbnA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("yselbA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("yseltnB[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("yseltB[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("yselbnB[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4; ++i) {
        ports.push_back("yselbB[" + std::to_string(i) + "]");
    }

    ports.push_back("sae_A");
    ports.push_back("sae_B");
    ports.push_back("VDD");
    ports.push_back("VSS");

    std::stringstream instances;
    instances << "X0 ";
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "WLTA[" << i << "] ";
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "WLTB[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLT_A[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLTN_A[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLT_B[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLTN_B[" << i << "] ";
    }
    instances << " VDD VSS array_sram_8t\n";

    instances << "X1 ";
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "WLBA[" << i << "] ";
    }
    for (int i = 0; i < config_.num_wls; ++i) {
        instances << "WLBB[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLB_A[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLBN_A[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLB_B[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLBN_B[" << i << "] ";
    }
    instances << " VDD VSS array_sram_8t\n";

    instances << "X2 wrenaA wrenanA oeb_outA oe_outA ";
    instances << "DA QA oeb_outB oe_outB QB ";
    for (int i = 0; i < 4; ++i) {
        instances << "BLT_A[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLTN_A[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLB_A[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLBN_A[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLT_B[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLTN_B[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "BLB_B[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {         
        instances << "BLBN_B[" << i << "] ";
    }

    instances << "blprechtnA blprechbnA blprechtnB blprechbnB ";
    for (int i = 0; i < 4; ++i) {
        instances << "yseltnA[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "yseltA[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "yselbnA[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "yselbA[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "yseltnB[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "yseltB[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "yselbnB[" << i << "] ";
    }
    for (int i = 0; i < 4; ++i) {
        instances << "yselbB[" << i << "] ";
    }
    instances << "sae_A sae_B VDD VSS iocolgrp_sram_8t\n";

    return create_subckt("colgrp_sram_8t", ports, instances.str());
}

std::string SpiceGenerator::generate_stacked_colgrp_8t() {
    std::vector<std::string> ports;

    // WLTA/WLTB and WLBA/WLBB ports for each bank
    for (int mux = 0; mux < config_.num_banks; ++mux) {
        for (int i = 0; i < config_.num_wls; ++i) {
            ports.push_back("WLTA[" + std::to_string(i + mux * config_.num_wls) + "]");
        }
        for (int i = 0; i < config_.num_wls; ++i) {
            ports.push_back("WLTB[" + std::to_string(i + mux * config_.num_wls) + "]");
        }
        for (int i = 0; i < config_.num_wls; ++i) {
            ports.push_back("WLBA[" + std::to_string(i + mux * config_.num_wls) + "]");
        }
        for (int i = 0; i < config_.num_wls; ++i) {
            ports.push_back("WLBB[" + std::to_string(i + mux * config_.num_wls) + "]");
        }
    }

    // D/Q ports
    for (int i = 0; i < config_.num_data_bits / 2; ++i) {
        ports.push_back("DA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_data_bits / 2; ++i) {
        ports.push_back("QA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_data_bits / 2; ++i) {
        ports.push_back("QB[" + std::to_string(i) + "]");
    }

    // wrena/wrenan, oeb_out/oe_out, blprechtn/blprechbn ports for each bank
    std::vector<std::string> ctrl_port_names = {
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

    for (int i = 0; i < 4 * config_.num_banks; ++i) {
        ports.push_back("yseltnA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4 * config_.num_banks; ++i) {
        ports.push_back("yseltA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4 * config_.num_banks; ++i) {
        ports.push_back("yselbnA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4 * config_.num_banks; ++i) {
        ports.push_back("yselbA[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4 * config_.num_banks; ++i) {
        ports.push_back("yseltnB[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4 * config_.num_banks; ++i) {
        ports.push_back("yseltB[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4 * config_.num_banks; ++i) {
        ports.push_back("yselbnB[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < 4 * config_.num_banks; ++i) {
        ports.push_back("yselbB[" + std::to_string(i) + "]");
    }

    ports.push_back("VDD");
    ports.push_back("VSS");

    // Instances
    std::stringstream instances;
    for (int mux = 0; mux < config_.num_banks; ++mux) {
        for (int bit = 0; bit < config_.num_data_bits / 2; ++bit) {
            instances << "X" << mux << "_" << bit << " ";
            for (int i = 0; i < config_.num_wls; ++i) {
                instances << "WLTA[" << (i + mux * config_.num_wls) << "] ";
            }
            for (int i = 0; i < config_.num_wls; ++i) {
                instances << "WLTB[" << (i + mux * config_.num_wls) << "] ";
            }
            for (int i = 0; i < config_.num_wls; ++i) {
                instances << "WLBA[" << (i + mux * config_.num_wls) << "] ";
            }
            for (int i = 0; i < config_.num_wls; ++i) {
                instances << "WLBB[" << (i + mux * config_.num_wls) << "] ";
            }

            instances << "DA[" << bit << "] QA[" << bit << "] QB[" << bit << "] ";
            instances << "wrenaA[" << mux << "] wrenanA[" << mux
                      << "] oeb_outA[" << mux << "] oe_outA[" << mux << "] oeb_outB[" << mux << "] oe_outB[" << mux 
                      << "] blprechtnA[" << mux << "] blprechbnA[" << mux << "] blprechtnB[" << mux << "] blprechbnB[" << mux
                      << "] ";

            for (int i = 0; i < 4; ++i) {
                instances << "yseltnA[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yseltA[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yselbnA[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yselbA[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yseltnB[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yseltB[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yselbnB[" << (i + mux * 4) << "] ";
            }
            for (int i = 0; i < 4; ++i) {
                instances << "yselbB[" << (i + mux * 4) << "] ";
            }

            instances << "sae_A[" << mux << "] sae_B[" << mux << "] ";
            instances << " VDD VSS colgrp_sram_8t\n";
        }
    }

    // Generate bank name matching GDS: stacked_colgrp_x{bits}x{num_data_bits / 2}x{num_banks}
    // bits = num_wls (since each colgrp has 2 arrays, each with num_wls)
    std::string bank_name = "stacked_colgrp_x" + std::to_string(config_.num_wls * 2) + "x" + std::to_string(config_.num_data_bits / 2) + "x" + std::to_string(config_.num_banks);
    return create_subckt(bank_name, ports, instances.str());
}

std::string SpiceGenerator::generate_spice_content(const bool& single_port) {
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
        content << sep << SpiceTemplates::get_iocolgrp() << "\n\n";
        
        // Generate hierarchy
        content << sep << generate_cell_row() << "\n";
        content << sep << generate_sramcol() << "\n";
        content << sep << generate_array() << "\n";
        content << sep << generate_colgrp() << "\n";
        content << sep << generate_stacked_colgrp() << "\n";
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
    std::string output_path = join_path(get_current_dir_name(), "sram_colgrp.sp");
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
