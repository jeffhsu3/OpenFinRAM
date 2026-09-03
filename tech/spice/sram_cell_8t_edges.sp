* OpenFinRAM ASAP7 8T dummy and edge-cell reference netlists.
*
* Cell roles follow the Apache-2.0 OpenRAM dual-port cell family at:
* https://github.com/VLSIDA/sky130_fd_bd_sram/tree/fc63b12883b4bf458ee8c756ba64c37063e1ffb9/cells
*
* The cap cells are electrically empty physical boundary cells.  Their ports
* name pass-through/terminating conductors in GDS, matching OpenRAM's cap-row
* and cap-column convention.

.SUBCKT dummy_cell_8t BLA BLAN BLB BLBN VDD VSS
M0 VSS VDD VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M1 VDD VSS VSS VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M2 VSS VDD VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M3 VDD VSS VDD VDD pmos_sram L=2e-08 W=2.7e-08 nfin=1
M4 VSS VSS BLA  VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M5 VDD VSS BLAN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M6 VSS VSS BLB  VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
M7 VDD VSS BLBN VSS nmos_sram L=2e-08 W=5.4e-08 nfin=2
.ENDS dummy_cell_8t

.SUBCKT sram_cell_8t_col_cap BLA BLAN BLB BLBN VDD VSS
.ENDS sram_cell_8t_col_cap

.SUBCKT sram_cell_8t_row_cap WLA WLB VSS
.ENDS sram_cell_8t_row_cap

.SUBCKT sram_cell_8t_corner VDD VSS
.ENDS sram_cell_8t_corner
