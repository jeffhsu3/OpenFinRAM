#pragma once

#include <string>

namespace OpenFinRAM {

/**
 * @brief Run LVS using calibre and check result.
 * @param project_root Project root directory (e.g. ".")
 * @param layout_path Layout GDS path (relative to project root)
 * @param netlist_path Netlist path (relative to project root)
 * @param cell_name Top cell name for LVS
 * @param log_path Output log file path (optional)
 * @param error Error message if any (optional)
 * @return true if LVS completed and is CORRECT
 */
bool run_lvs(const std::string& project_root,
             const std::string& layout_path,
             const std::string& netlist_path,
             const std::string& cell_name,
             std::string* log_path,
             std::string* error);

} // namespace OpenFinRAM
