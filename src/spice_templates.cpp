#include "spice_templates.hpp"

namespace OpenFinRAM {

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

std::string SpiceTemplates::get_cell_8t() {
    return R"(.SUBCKT sram_cell_8t WLA WLB BLA BLAN BLB BLBN VDD VSS
M0 Q  QB VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M1 QB Q  VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M2 Q  QB VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M3 QB Q  VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M4 Q  WLA BLA  VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M5 QB WLA BLAN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M6 Q  WLB BLB  VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M7 QB WLB BLBN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
.ENDS)";
}

std::string SpiceTemplates::get_replica_cell_8t() {
    return R"(.SUBCKT replica_cell_8t WLA WLB RBLA RBLB VDD VSS
M0 VSS VDD VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M1 VDD VSS VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M2 VSS VDD VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M3 VDD VSS VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M4 VSS WLA RBLA VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M5 VDD WLA RBLAN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M6 VSS WLB RBLB VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M7 VDD WLB RBLBN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
.ENDS)";
}

std::string SpiceTemplates::get_dummy_cell() {
    return R"(.SUBCKT dummy_sram_6t122 BLN VDD VSS
M0 QB VSS bln VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2
M1 Q VDD VSS VSS nmos_rvt L=2e-08 W=5.4e-08 nfin=2
M2 Q VDD VDD VDD pmos_rvt L=2e-08 W=2.7e-08 nfin=1
.ENDS)";
}

std::string SpiceTemplates::get_dummy_cell_8t() {
    return R"(.SUBCKT dummy_cell_8t BLA BLAN BLB BLBN VDD VSS
M0 VSS VDD VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M1 VDD VSS VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M2 VSS VDD VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M3 VDD VSS VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M4 VSS VSS BLA  VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M5 VDD VSS BLAN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M6 VSS VSS BLB  VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M7 VDD VSS BLBN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
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

std::string SpiceTemplates::get_prech_8t_v1() {
    return R"(.SUBCKT sram_prech_ymux_8t_v1
+ blprechn_A yseln_A ysel_A san_A sa_A bln_A bl_A
+ blprechn_B yseln_B ysel_B san_B sa_B bln_B bl_B
+ VDD VSS
M0  bln_A ysel_A san_A VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1  bl_A  ysel_A sa_A  VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M2  bln_A blprechn_A VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M3  bl_A  blprechn_A VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M4  san_A yseln_A bln_A VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5  sa_A  yseln_A bl_A  VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M6  bln_B ysel_B san_B VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M7  bl_B  ysel_B sa_B  VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M8  bln_B blprechn_B VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M9  bl_B  blprechn_B VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M10 san_B yseln_B bln_B VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M11 sa_B  yseln_B bl_B  VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
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

std::string SpiceTemplates::get_prech_8t_v2() {
    return R"(.SUBCKT sram_prech_ymux_8t_v2
+ blprechn_A yseln_A ysel_A san_A sa_A bln_A bl_A
+ blprechn_B yseln_B ysel_B san_B sa_B bln_B bl_B
+ VDD VSS
M0  bln_A ysel_A san_A VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1  bl_A  ysel_A sa_A  VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M2  bln_A blprechn_A VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M3  bl_A  blprechn_A VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M4  san_A yseln_A bln_A VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5  sa_A  yseln_A bl_A  VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M6  bln_B ysel_B san_B VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M7  bl_B  ysel_B sa_B  VSS nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M8  bln_B blprechn_B VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M9  bl_B  blprechn_B VDD VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M10 san_B yseln_B bln_B VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M11 sa_B  yseln_B bl_B  VDD pmos_rvt L=2e-08 W=8.1e-08 nfin=3
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

std::string SpiceTemplates::get_wrasst_prech_ymux_x8_sram_8t() {
    return R"(.SUBCKT wrasst_prech_ymux_x8_sram_8t
+ bln_A[0] bln_A[1] bln_A[2] bln_A[3]
+ bl_A[0]  bl_A[1]  bl_A[2]  bl_A[3]
+ bln_B[0] bln_B[1] bln_B[2] bln_B[3]
+ bl_B[0]  bl_B[1]  bl_B[2]  bl_B[3]
+ yseln_A[0] yseln_A[1] yseln_A[2] yseln_A[3]
+ ysel_A[0]  ysel_A[1]  ysel_A[2]  ysel_A[3]
+ yseln_B[0] yseln_B[1] yseln_B[2] yseln_B[3]
+ ysel_B[0]  ysel_B[1]  ysel_B[2]  ysel_B[3]
+ san_A sa_A san_B sa_B
+ blprechn_A blprechn_B
+ VDD VSS
X0 blprechn_A yseln_A[0] ysel_A[0] san_A sa_A bln_A[0] bl_A[0] blprechn_B yseln_B[0] ysel_B[0] san_B sa_B bln_B[0] bl_B[0] VDD VSS sram_prech_ymux_8t_v1
X1 blprechn_A yseln_A[1] ysel_A[1] san_A sa_A bln_A[1] bl_A[1] blprechn_B yseln_B[1] ysel_B[1] san_B sa_B bln_B[1] bl_B[1] VDD VSS sram_prech_ymux_8t_v1
X2 blprechn_A yseln_A[2] ysel_A[2] san_A sa_A bln_A[2] bl_A[2] blprechn_B yseln_B[2] ysel_B[2] san_B sa_B bln_B[2] bl_B[2] VDD VSS sram_prech_ymux_8t_v2
X3 blprechn_A yseln_A[3] ysel_A[3] san_A sa_A bln_A[3] bl_A[3] blprechn_B yseln_B[3] ysel_B[3] san_B sa_B bln_B[3] bl_B[3] VDD VSS sram_prech_ymux_8t_v2
.ENDS)";
}

std::string SpiceTemplates::get_write_driver() {
    return R"(.SUBCKT write_driver_sram D wrena wrenan wdo wdon vdd vss
M0 vdd D  53  vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M1 vss D  53  vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M2 50  51  vdd vdd pmos_rvt L=2e-08 W=2.7e-08 nfin=1
M3 vdd 50  51  vdd pmos_rvt L=2e-08 W=2.7e-08 nfin=1
M4  50  51  vss vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M5  vss 50  51  vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M6 D   wrenan 50  vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M7 53  wrenan 51  vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M8  wdo  wrena  50   vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M9  51   wrena  wdon vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M10 wdo  wrenan 50   vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M11 51   wrenan wdon vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
.ENDS)";
}

std::string SpiceTemplates::get_sense_amp() {
    return R"(.SUBCKT sense_amp_sram sa san SAE SAPRECHN qa qan vdd vss
M0 qan SAPRECHN vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M1 59  SAPRECHN qan vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M2 qa  SAPRECHN 59  vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M3 vdd SAPRECHN qa  vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M4 vdd qa  qan vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M5 qa  qan vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M6  52  sa  57  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M7  58  san 52  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M8  57  qa  qan vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M9 qa  qan 58  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M10  vss SAE 52  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M11  52  SAE vss vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M12  qan vss vss vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M13 vss vss qa  vss nmos_rvt L=2e-08 W=3.24e-07 nfin=12
M14 qan vdd vdd vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
M15 vdd vdd qa  vdd pmos_rvt L=2e-08 W=1.08e-07 nfin=4
.ENDS)";
}

std::string SpiceTemplates::get_skewed_inv() {
    return R"(.SUBCKT skewed_inv_sram in out VDD VSS
MP out in VDD VDD pmos_rvt L=2e-08 W=2.16e-07 nfin=8
MN out in VSS VSS nmos_rvt L=2e-08 W=5.4e-08  nfin=2
.ENDS)";
}

std::string SpiceTemplates::get_or2() {
    return R"(.SUBCKT or2_sram A B VDD VSS Y
MM5 VSS net7 Y VSS nmos_rvt w=162.00n l=20n nfin=6
MM1 VSS B net7 VSS nmos_rvt w=54.0n l=20n nfin=2
MM2 VSS A net7 VSS nmos_rvt w=54.0n l=20n nfin=2
MM0 VDD net7 Y VDD pmos_rvt w=162.00n l=20n nfin=6
MM4 net15 B net7 VDD pmos_rvt w=81.0n l=20n nfin=3
MM3 VDD A net15 VDD pmos_rvt w=81.0n l=20n nfin=3
.ENDS)";
}

std::string SpiceTemplates::get_buf() {
    return R"(.SUBCKT buf_sram A VDD VSS Y
MM3 Y AN VSS VSS nmos_rvt w=8.1e-08 l=20n nfin=3
MM2 AN A VSS VSS nmos_rvt w=8.1e-08 l=20n nfin=3
MM0 Y AN VDD VDD pmos_rvt w=8.1e-08 l=20n nfin=3
MM1 AN A VDD VDD pmos_rvt w=8.1e-08 l=20n nfin=3
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

std::string SpiceTemplates::get_buf_sram(const std::string& port, const int& num_buf) {
    std::string buf_str;
    for (int i = 0; i < num_buf - 1; ++i) {
        buf_str += "XBUF_" + std::to_string(i) + " sae_" + port + "_" + std::to_string(i) + " VDD VSS sae_" + port + "_" + std::to_string(i + 1) + " buf_sram\n";
    }

    buf_str += "XBUF_" + std::to_string(num_buf - 1) + " sae_" + port + "_" + std::to_string(num_buf - 1) + " VDD VSS sae_" + port + " buf_sram\n";

    return buf_str;
}

std::string SpiceTemplates::get_iocolgrp_8t(const int& num_buf) {
    std::string result = R"(.SUBCKT iocolgrp_sram_8t
+ wrena_A wrenan_A wrena_B wrenan_B
+ RBL_A RBL_B
+ oeb_out_A oe_out_A DA QA
+ oeb_out_B oe_out_B DB QB
+ blt_A[0]  blt_A[1]  blt_A[2]  blt_A[3]
+ bltn_A[0] bltn_A[1] bltn_A[2] bltn_A[3]
+ blb_A[0]  blb_A[1]  blb_A[2]  blb_A[3]
+ blbn_A[0] blbn_A[1] blbn_A[2] blbn_A[3]
+ blt_B[0]  blt_B[1]  blt_B[2]  blt_B[3]
+ bltn_B[0] bltn_B[1] bltn_B[2] bltn_B[3]
+ blb_B[0]  blb_B[1]  blb_B[2]  blb_B[3]
+ blbn_B[0] blbn_B[1] blbn_B[2] blbn_B[3]
+ blprechtn_A blprechbn_A blprechtn_B blprechbn_B
+ blprechn_rbl_A blprechn_rbl_B
+ yseltn_A[0] yseltn_A[1] yseltn_A[2] yseltn_A[3]
+ yselt_A[0]  yselt_A[1]  yselt_A[2]  yselt_A[3]
+ yselbn_A[0] yselbn_A[1] yselbn_A[2] yselbn_A[3]
+ yselb_A[0]  yselb_A[1]  yselb_A[2]  yselb_A[3]
+ yseltn_B[0] yseltn_B[1] yseltn_B[2] yseltn_B[3]
+ yselt_B[0]  yselt_B[1]  yselt_B[2]  yselt_B[3]
+ yselbn_B[0] yselbn_B[1] yselbn_B[2] yselbn_B[3]
+ yselb_B[0]  yselb_B[1]  yselb_B[2]  yselb_B[3]
+ vdd vss
XINV_A RBL_A sae_A_0 vdd vss skewed_inv_sram
)";
    result += get_buf_sram("A", num_buf);
    result += R"(
XINV_B RBL_B sae_B_0 vdd vss skewed_inv_sram
)";
    result += get_buf_sram("B", num_buf);
    result += R"(
MP_RBL_A RBL_A blprechn_rbl_A vdd vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
MP_RBL_B RBL_B blprechn_rbl_B vdd vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
XWD_A DA wrena_A wrenan_A sa_A san_A vdd vss write_driver_sram
XWD_B DB wrena_B wrenan_B sa_B san_B vdd vss write_driver_sram
XSA_A sa_A san_A sae_A sae_A qa_A qan_A vdd vss sense_amp_sram
XSA_B sa_B san_B sae_B sae_B qa_B qan_B vdd vss sense_amp_sram
XWRMUX_T
+ bltn_A[0] bltn_A[1] bltn_A[2] bltn_A[3]
+ blt_A[0]  blt_A[1]  blt_A[2]  blt_A[3]
+ bltn_B[0] bltn_B[1] bltn_B[2] bltn_B[3]
+ blt_B[0]  blt_B[1]  blt_B[2]  blt_B[3]
+ yseltn_A[0] yseltn_A[1] yseltn_A[2] yseltn_A[3]
+ yselt_A[0]  yselt_A[1]  yselt_A[2]  yselt_A[3]
+ yseltn_B[0] yseltn_B[1] yseltn_B[2] yseltn_B[3]
+ yselt_B[0]  yselt_B[1]  yselt_B[2]  yselt_B[3]
+ san_A sa_A san_B sa_B
+ blprechtn_A blprechtn_B
+ vdd vss wrasst_prech_ymux_x8_sram_8t
XWRMUX_B
+ blbn_A[0] blbn_A[1] blbn_A[2] blbn_A[3]
+ blb_A[0]  blb_A[1]  blb_A[2]  blb_A[3]
+ blbn_B[0] blbn_B[1] blbn_B[2] blbn_B[3]
+ blb_B[0]  blb_B[1]  blb_B[2]  blb_B[3]
+ yselbn_A[0] yselbn_A[1] yselbn_A[2] yselbn_A[3]
+ yselb_A[0]  yselb_A[1]  yselb_A[2]  yselb_A[3]
+ yselbn_B[0] yselbn_B[1] yselbn_B[2] yselbn_B[3]
+ yselb_B[0]  yselb_B[1]  yselb_B[2]  yselb_B[3]
+ san_A sa_A san_B sa_B
+ blprechbn_A blprechbn_B
+ vdd vss wrasst_prech_ymux_x8_sram_8t
M19_A vss n49_A n48_A vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M39_A vdd n49_A n48_A vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
M19_B vss n49_B n48_B vss nmos_rvt L=2e-08 W=8.1e-08 nfin=3
M39_B vdd n49_B n48_B vdd pmos_rvt L=2e-08 W=8.1e-08 nfin=3
X44_A n49_A qa_A  n54_A vdd vss io_nand_3f_6f
X45_A n54_A qan_A n49_A vdd vss io_nand_3f_6f
X44_B n49_B qa_B  n54_B vdd vss io_nand_3f_6f
X45_B n54_B qan_B n49_B vdd vss io_nand_3f_6f
X46_A n48_A oeb_out_A oe_out_A vdd vss QA TBUF_INV
X46_B n48_B oeb_out_B oe_out_B vdd vss QB TBUF_INV
.ENDS)";

    return result;
}

} // namespace OpenFinRAM