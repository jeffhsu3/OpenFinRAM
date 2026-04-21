#ifndef INNOVUS_MANAGER_HPP
#define INNOVUS_MANAGER_HPP

#include "main_config_helpers.hpp"

class InnovusManager {
public:
    explicit InnovusManager(const MainCliOptions& cli_options);

    bool run_innovus_flow();

private:
    MainCliOptions cli_options_;
};

#endif // INNOVUS_MANAGER_HPP