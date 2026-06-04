#ifndef LVS_MANAGER_HPP
#define LVS_MANAGER_HPP

#include "main_config_helpers.hpp"

class LvsManager {
public:
    explicit LvsManager(const MainCliOptions& cli_options);

    bool run_lvs();

private:
    MainCliOptions cli_options_;
};

#endif // LVS_MANAGER_HPP