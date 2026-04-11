#pragma once

#include <string>

namespace OpenFinRAM {

/**
 * @brief Export LEF using Cadence abstract flow.
 *
 * Steps:
 * 1) Create cds.lib in project_root.
 * 2) Run strmin to import GDS into the specified library.
 * 3) Create export_lef.il in the library folder with cell-specific settings.
 * 4) Run abstract to export LEF.
 *
 * @param project_root Working directory where cds.lib/log are created.
 * @param cell_name SRAM top cell name (library name).
 * @param gds_path Path to GDS file (relative or absolute).
 * @param log_path Output log path for abstract (optional).
 * @param error Error message if any (optional).
 * @return true if all steps completed successfully.
 */
bool export_lef(const std::string& project_root,
                const std::string& cell_name,
                const std::string& gds_path,
                std::string* log_path,
                std::string* error);

} // namespace OpenFinRAM
