#pragma once

#include <string>

namespace OpenFinRAM {

/**
 * @brief Export a hard-macro LEF directly from the final GDS using gdstk.
 *
 * The exporter uses the top-level BOUNDARY polygon for SIZE, direct
 * pin-purpose labels for the macro interface, and containing pin/drawing
 * geometry for each PORT.  It does not require Cadence strmin or Abstract.
 *
 * @param project_root Directory where <cell_name>.lef is written.
 * @param cell_name SRAM top cell name (library name).
 * @param gds_path Path to GDS file (relative or absolute).
 * @param output_path Generated LEF path (optional).
 * @param error Error message if any (optional).
 * @return true if the LEF was generated successfully.
 */
bool export_lef(const std::string& project_root,
                const std::string& cell_name,
                const std::string& gds_path,
                std::string* output_path,
                std::string* error);

} // namespace OpenFinRAM
