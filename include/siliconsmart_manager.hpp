#ifndef SILICONSMART_MANAGER_HPP
#define SILICONSMART_MANAGER_HPP

#include "main_config_helpers.hpp"

class SiliconSmartManager {
public:
    SiliconSmartManager(const MainCliOptions& options);

    bool run_siliconsmart();

    bool gen_config();

    bool gen_run();

    bool gen_template();

    bool gen_inst();

    bool copy_lib_file();

private:
    MainCliOptions cli_option_;

    std::string get_port_list();
};

#endif // SILICONSMART_MANAGER_HPP