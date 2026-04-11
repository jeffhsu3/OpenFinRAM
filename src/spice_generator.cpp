#include "spice_generator.hpp"
#include "plog/Log.h"
#include <sstream>
#include <iomanip>

namespace OpenFinRAM {

// ============================================================================
// SPICE Templates Implementation
// ============================================================================

std::string SpiceTemplates::get_cell_6t() {
    return R"(.SUBCKT sram_cell_6t_122 WL BLN BL VDD VSS
M0 QB WL BLN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M1 Q QB VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M2 VSS Q QB VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M3 BL WL Q VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M4 Q QB VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M5 VDD Q QB VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
.ENDS)";
}

std::string SpiceTemplates::get_dummy_cell() {
    return R"(.SUBCKT dummy_sram_6t122 BLN VDD VSS
M0 QB VSS bln VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2
M1 Q VDD VSS VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2
M2 Q VDD VDD VDD pmos_rvt L=2e-08 W=2.7e-08 nfin=1
.ENDS)";
}

std::string SpiceTemplates::get_dummy_topbot_v1() {
    return R"(.SUBCKT dummy_topbot_v1 BLN VDD VSS
M0 6 VSS BLN VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2
M1 VSS VDD VSS VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2
M2 VDD VDD VDD VDD pmos_rvt L=2e-08 W=2.7e-08 nfin=1
.ENDS)";
}

std::string SpiceTemplates::get_dummy_topbot_v2() {
    return R"(.SUBCKT dummy_topbot_v2 BLN VDD VSS
M0 VSS VDD VSS VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2
M1 6 VSS BLN VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2
M2 VDD VDD VDD VDD pmos_rvt L=2e-08 W=2.7e-08 nfin=1
.ENDS)";
}

std::string SpiceTemplates::get_prech_v1() {
    return R"(.SUBCKT sram_prech_ymux_6t112_v1 blprechn yseln ysel san sa bln bl VDD VSS
M0 bln ysel san VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1 bl ysel sa VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M2 bln blprechn VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M3 bl blprechn VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M4 san yseln bln VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5 sa yseln bl VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
.ENDS)";
}

std::string SpiceTemplates::get_prech_v2() {
    return R"(.SUBCKT sram_prech_ymux_6t112_v2 blprechn yseln ysel san sa bln bl VDD VSS
M0 bl ysel sa VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1 bln ysel san VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M2 bl blprechn VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M3 bln blprechn VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M4 sa yseln bl VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5 san yseln bln VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
.ENDS)";
}

std::string SpiceTemplates::get_prech_ymux() {
    return R"(.SUBCKT wrasst_prech_ymux_x4_sram_6t122_v2 bln[0] bln[1] bln[2] bln[3] bl[0] bl[1] bl[2] bl[3] yseln[0] yseln[1]
+ yseln[2] yseln[3] ysel[0] ysel[1] ysel[2] ysel[3] san sa blprechn VDD 
+ VSS
X0 blprechn yseln[0] ysel[0] san sa bln[0] bl[0] VDD VSS sram_prech_ymux_6t112_v1
X1 blprechn yseln[1] ysel[1] san sa bln[1] bl[1] VDD VSS sram_prech_ymux_6t112_v1
X2 blprechn yseln[2] ysel[2] san sa bln[2] bl[2] VDD VSS sram_prech_ymux_6t112_v2
X3 blprechn yseln[3] ysel[3] san sa bln[3] bl[3] VDD VSS sram_prech_ymux_6t112_v2
.ENDS)";
}

std::string SpiceTemplates::get_io_nand() {
    return R"(.SUBCKT io_nand_3f_6f A B Y VDD VSS
M0 VSS A 6 VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1 6 A VSS VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M2 Y B 6 VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M3 6 B Y VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M4 VDD A Y VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5 Y B VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
.ENDS)";
}

std::string SpiceTemplates::get_tbuf() {
    return R"(.SUBCKT TBUF_INV A ENB EN VDD VSS Y
MP1 n1 A VDD   VDD pmos_rvt L=2e-08 W=1296.00n nfin=48
MP2 Y   ENB  n1 VDD pmos_rvt L=2e-08 W=972.00n nfin=36
MN1 n2 A VSS   VSS nmos_rvt L=2e-08 W=1296.00n nfin=48
MN2 Y   EN  n2 VSS nmos_rvt L=2e-08 W=972.00n nfin=36
.ENDS)";
}

std::string SpiceTemplates::get_iocolgrp() {
    return R"(.SUBCKT iocolgrp_sram_6t122_v2 wrenan wrena SAE SAPRECHN oeb_out oe_out D Q bltn[0] bltn[1] bltn[2]
+ bltn[3] blt[0] blt[1] blt[2] blt[3] blbn[0] blbn[1] blbn[2] blbn[3] blb[0] 
+ blb[1] blb[2] blb[3] BLPRECHTN BLPRECHBN yseltn[0] yseltn[1] yseltn[2] yseltn[3] yselt[0]
+ yselt[1] yselt[2] yselt[3] yselbn[0] yselbn[1] yselbn[2] yselbn[3] yselb[0] yselb[1] yselb[2] 
+ yselb[3] vdd vss
M0 51 wrena 46 vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1 47 vss vss vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M2 vss 50 51 vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M3 57 44 47 vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M4 50 51 vss vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5 52 45 57 vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M6 45 wrena 50 vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M7 vss SAE 52 vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M8 52 SAE vss vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M9 58 46 52 vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M10 D wrenan 50 vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M11 53 wrenan 51 vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M12 44 47 58 vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M13 vss vss 44 vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M15 vss D 53 vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M19 vss 49 48 vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M22 51 wrenan 46 vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M23 47 vdd vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M24 vdd 50 51 vdd pmos_rvt L=2e-08 W=2.7e-08 nfin=1
M25 vdd 44 47 vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M26 50 51 vdd vdd pmos_rvt L=2e-08 W=2.7e-08 nfin=1
M27 47 SAPRECHN vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M28 45 wrenan 50 vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M29 59 SAPRECHN 47 vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M30 44 SAPRECHN 59 vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M31 vdd SAPRECHN 44 vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M32 44 47 vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M33 vdd vdd 44 vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M35 vdd D 53 vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M39 vdd 49 48 vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
X42 bltn[0] bltn[1] bltn[2] bltn[3] blt[0] blt[1] blt[2] blt[3] yseltn[0] yseltn[1]
+ yseltn[2] yseltn[3] yselt[0] yselt[1] yselt[2] yselt[3] 46 45 BLPRECHTN vdd 
+ vss wrasst_prech_ymux_x4_sram_6t122_v2
X43 blbn[0] blbn[1] blbn[2] blbn[3] blb[0] blb[1] blb[2] blb[3] yselbn[0] yselbn[1]
+ yselbn[2] yselbn[3] yselb[0] yselb[1] yselb[2] yselb[3] 46 45 BLPRECHBN vdd 
+ vss wrasst_prech_ymux_x4_sram_6t122_v2
X44 49 44 54 vdd vss io_nand_3f_6f
X45 54 47 49 vdd vss io_nand_3f_6f
X46 48 oeb_out oe_out vdd vss Q TBUF_INV
.ENDS)";
}

// ============================================================================
// SPICE Generator Implementation
// ============================================================================

SpiceGenerator::SpiceGenerator(const SpiceConfig& config)
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
    for (int i = 0; i < config_.num_wordlines; ++i) {
        ports.push_back("WL[" + std::to_string(i) + "]");
    }
    ports.push_back("BLN");
    ports.push_back("BL");
    ports.push_back("VDD");
    ports.push_back("VSS");
    
    // Instances
    std::stringstream instances;
    for (int i = 0; i < config_.num_wordlines; ++i) {
        instances << "X" << i << " WL[" << i << "] BLN BL VDD VSS sram_cell_6t_122\n";
    }
    instances << "X" << config_.num_wordlines << " BLN VDD VSS dummy_sram_6t122\n";
    
    return create_subckt("sram_cell_row", ports, instances.str());
}

std::string SpiceGenerator::generate_sramcol() {
    std::vector<std::string> ports;
    
    // WL ports
    for (int i = 0; i < config_.num_wordlines; ++i) {
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
    for (int i = 0; i < config_.num_wordlines; ++i) {
        instances << "WL[" << i << "] ";
    }
    instances << "BLN BL VDD VSS sram_cell_row\n";
    
    std::string cell_name = "sramcol_x" + std::to_string(config_.num_wordlines) + "_sram_6t122";
    return create_subckt(cell_name, ports, instances.str());
}

std::string SpiceGenerator::generate_array() {
    std::vector<std::string> ports;
    
    // WL ports
    for (int i = 0; i < config_.num_wordlines; ++i) {
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
        for (int j = 0; j < config_.num_wordlines; ++j) {
            instances << "WL[" << j << "] ";
        }
        instances << "BLN[" << i << "] BL[" << i << "] VDD VSS sram_cell_row\n";
    }
    
    return create_subckt("array_sram_6t122", ports, instances.str());
}

std::string SpiceGenerator::generate_colgrp() {
    std::vector<std::string> ports;
    
    // WLT and WLB ports
    for (int i = 0; i < config_.num_wordlines; ++i) {
        ports.push_back("WLT[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wordlines; ++i) {
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
    for (int i = 0; i < config_.num_wordlines; ++i) {
        instances << "WLT[" << i << "] ";
    }
    instances << "BLTN[0] BLTN[1] BLTN[2] BLTN[3] BLT[0] BLT[1] BLT[2] BLT[3] VDD VSS array_sram_6t122\n";
    
    // Bottom array
    instances << "X1 ";
    for (int i = 0; i < config_.num_wordlines; ++i) {
        instances << "WLB[" << i << "] ";
    }
    instances << "BLBN[0] BLBN[1] BLBN[2] BLBN[3] BLB[0] BLB[1] BLB[2] BLB[3] VDD VSS array_sram_6t122\n";
    
    // IO column group
    instances << "X2 wrenan wrena SAE SAPRECHN oeb_out oe_out D Q ";
    instances << "bltn[0] bltn[1] bltn[2]\n";
    instances << "+ bltn[3] blt[0] blt[1] blt[2] blt[3] blbn[0] blbn[1] blbn[2] blbn[3] blb[0]\n";
    instances << "+ blb[1] blb[2] blb[3] BLPRECHTN BLPRECHBN yseltn[0] yseltn[1] yseltn[2] yseltn[3] yselt[0]\n";
    instances << "+ yselt[1] yselt[2] yselt[3] yselbn[0] yselbn[1] yselbn[2] yselbn[3] yselb[0] yselb[1] yselb[2]\n";
    instances << "+ yselb[3] vdd vss iocolgrp_sram_6t122_v2\n";
    
    return create_subckt("colgrp_sram_6t122", ports, instances.str());
}

std::string SpiceGenerator::generate_stacked_colgrp() {
    std::vector<std::string> ports;
    
    // WLT and WLB ports
    for (int i = 0; i < config_.num_wordlines; ++i) {
        ports.push_back("WLT[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.num_wordlines; ++i) {
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
    for (int i = 0; i < config_.data_bits; ++i) {
        ports.push_back("D[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.data_bits; ++i) {
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
    for (int bit = 0; bit < config_.data_bits; ++bit) {
        instances << "X" << bit << " ";
        for (int i = 0; i < config_.num_wordlines; ++i) {
            instances << "WLT[" << i << "] ";
        }
        for (int i = 0; i < config_.num_wordlines; ++i) {
            instances << "WLB[" << i << "] ";
        }
        for (const auto& ysel : ysel_ports) {
            instances << ysel << " ";
        }
        instances << "D[" << bit << "] Q[" << bit << "] ";
        instances << "wrena wrenan saprechn sae oeb_out oe_out blprechtn blprechbn VDD VSS ";
        instances << "colgrp_sram_6t122\n";
    }
    
    // Generate bank name matching GDS: stacked_colgrp_x{bits}x{num_colgrp}
    // bits = num_wordlines (since each colgrp has 2 arrays, each with num_wordlines)
    std::string bank_name = "stacked_colgrp_x" + std::to_string(config_.num_wordlines * 2) + "x" + std::to_string(config_.num_colgrp);
    return create_subckt(bank_name, ports, instances.str());
}

std::string SpiceGenerator::generate_stacked_colgrp_mux() {
    std::vector<std::string> ports;
    
    for (int mux = 0; mux < config_.num_mux; ++mux) {
        // WLT and WLB ports
        for (int i = 0; i < config_.num_wordlines; ++i) {
            ports.push_back("WLT[" + std::to_string(i + mux * config_.num_wordlines) + "]");
        }
        for (int i = 0; i < config_.num_wordlines; ++i) {
            ports.push_back("WLB[" + std::to_string(i + mux * config_.num_wordlines) + "]");
        }
    }
    
    // YSEL ports
    std::vector<std::string> ysel_ports;
    for (int mux = 0; mux < config_.num_mux; ++mux) {
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
    for (int i = 0; i < config_.data_bits; ++i) {
        ports.push_back("D[" + std::to_string(i) + "]");
    }
    for (int i = 0; i < config_.data_bits; ++i) {
        ports.push_back("Q[" + std::to_string(i) + "]");
    }
    
    // Control ports
    std::vector<std::string> ctrl_port_names = {
        "wrena", "wrenan", "saprechn", "sae", "oeb_out", "oe_out",
        "blprechtn", "blprechbn"
    };
    std::vector<std::string> ctrl_ports;
    for (const auto& name : ctrl_port_names) {
        for (int mux = 0; mux < config_.num_mux; ++mux) {
            ctrl_ports.push_back(name + "[" + std::to_string(mux) + "]");
        }
    }
    ctrl_ports.push_back("VDD");
    ctrl_ports.push_back("VSS");
    ports.insert(ports.end(), ctrl_ports.begin(), ctrl_ports.end());
    
    // Instances
    std::stringstream instances;
    for (int mux = 0; mux < config_.num_mux; ++mux) {
        for (int bit = 0; bit < config_.data_bits; ++bit) {
            instances << "X" << mux << "_" << bit << " ";
            for (int i = 0; i < config_.num_wordlines; ++i) {
                instances << "WLT[" << (i + mux * config_.num_wordlines) << "] ";
            }
            for (int i = 0; i < config_.num_wordlines; ++i) {
                instances << "WLB[" << (i + mux * config_.num_wordlines) << "] ";
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
    
    // Generate bank name matching GDS: stacked_colgrp_x{bits}x{num_colgrp}
    // bits = num_wordlines (since each colgrp has 2 arrays, each with num_wordlines)
    std::string bank_name = "stacked_colgrp_x" + std::to_string(config_.num_wordlines * 2) + "x" + std::to_string(config_.num_colgrp) + "x" + std::to_string(config_.num_mux);
    return create_subckt(bank_name, ports, instances.str());
}

std::string SpiceGenerator::generate_spice_content() {
    std::stringstream content;
    std::string sep = std::string(70, '*') + "\n";
    
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
    
    return content.str();
}

bool SpiceGenerator::generate(const std::string& output_filename) {
    std::string output_path = config_.output_dir + "/" + output_filename;
    std::string content = generate_spice_content();
    
    std::ofstream outfile(output_path);
    if (!outfile.is_open()) {
        LOGE << "Failed to open output file: " << output_path;
        return false;
    }
    
    outfile << content;
    outfile.close();
    
    LOGI << "✓ Generated SPICE netlist: " << output_filename;
    LOGI << "  Number of wordlines: " << config_.num_wordlines;
    LOGI << "  Number of column groups: " << config_.num_colgrp;
    LOGI << "  Data bits: " << config_.data_bits;
    LOGI << "  Mux slices: " << config_.num_mux;
    
    return true;
}

} // namespace OpenFinRAM
