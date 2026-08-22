#ifndef OPENROAD_MANAGER_HPP
#define OPENROAD_MANAGER_HPP

#include "main_config_helpers.hpp"

class OpenRoadManager {
public:
    explicit OpenRoadManager(const MainCliOptions& cli_options);
    bool run_openroad_flow();
private:
    MainCliOptions cli_options_;
};

#endif // OPENROAD_MANAGER_HPP
