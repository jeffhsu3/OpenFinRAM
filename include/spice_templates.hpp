#ifndef SPICE_TEMPLATE_HPP
#define SPICE_TEMPLATE_HPP

#include <string>

namespace OpenFinRAM {

class SpiceTemplates {
public:
    static std::string get_cell_6t();
    static std::string get_cell_8t();
    static std::string get_replica_cell_8t();
    static std::string get_dummy_cell();
    static std::string get_dummy_cell_8t();
    static std::string get_dummy_topbot_v1();
    static std::string get_dummy_topbot_v2();
    static std::string get_prech_v1();
    static std::string get_prech_8t_v1();
    static std::string get_prech_v2();
    static std::string get_prech_8t_v2();
    static std::string get_prech_ymux();
    static std::string get_wrasst_prech_ymux_x8_sram_8t();
    static std::string get_write_driver();
    static std::string get_sense_amp();
    static std::string get_skewed_inv();
    static std::string get_or2();
    static std::string get_buf();
    static std::string get_io_nand();
    static std::string get_tbuf();
    static std::string get_iocolgrp();
    static std::string get_buf_sram(const std::string& port, const int& num_buf = 5);
    static std::string get_iocolgrp_8t(const int& num_buf = 5);
};
} // namespace OpenFinRAM

#endif // SPICE_TEMPLATE_HPP