#pragma once

#include <string>

#include "main_config_helpers.hpp"

namespace OpenFinRAM {

/**
 * Write an explicitly estimated, single-port SRAM Liberty model.
 *
 * The model is intended for early synthesis/STA integration when transistor-
 * level characterization is skipped or unavailable. Timing values follow a
 * coarse FakeRAM-style ASAP7 baseline and are not characterized values.
 * Macro area is taken from the generated LEF.
 */
bool export_estimated_liberty(const MainCliOptions& options,
                              const std::string& lef_path,
                              const std::string& liberty_path,
                              std::string* error = nullptr);

}  // namespace OpenFinRAM
