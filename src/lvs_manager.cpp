#include "lvs_manager.hpp"

#include "plog/Log.h"

#include "lvs_runner.hpp"
#include "utils.hpp"

LvsManager::LvsManager(const MainCliOptions& cli_options)
    : cli_options_(cli_options) {}

bool LvsManager::run_lvs() {
    std::string sram_cell_name = "sram_x" + std::to_string(cli_options_.num_wls * 2) + "x" + std::to_string(cli_options_.num_data_bits) + "x" + std::to_string(cli_options_.num_banks);
    std::string lvs_log_path;
    std::string lvs_error;

    LOGI << "Running LVS for cell: " << sram_cell_name;
    bool lvs_ok = OpenFinRAM::run_lvs(".", join_path(get_executable_directory(), "results/" + sram_cell_name + "_" + get_run_timestamp() + "/" + sram_cell_name + ".gds"), join_path(get_executable_directory(), "results/" + sram_cell_name + "_" + get_run_timestamp() + "/" + sram_cell_name + ".sp"), sram_cell_name, &lvs_log_path, &lvs_error);

    if (lvs_ok) {
        LOGI << "LVS completed. CORRECT.";
    } else {
        LOGW << "LVS failed or not correct. Log: " << lvs_log_path;
        if (!lvs_error.empty()) {
            LOGW << "LVS error: " << lvs_error;
        }
    }

    return lvs_ok;
}