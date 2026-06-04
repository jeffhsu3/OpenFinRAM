#ifndef LEF_MANAGER_HPP
#define LEF_MANAGER_HPP

#include "main_config_helpers.hpp"

class LefManager {
public:
    explicit LefManager(const MainCliOptions& cli_options);

    bool export_lef();

private:
    MainCliOptions cli_options_;
};

#endif // LEF_MANAGER_HPP
