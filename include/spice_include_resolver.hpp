#ifndef SPICE_INCLUDE_RESOLVER_HPP
#define SPICE_INCLUDE_RESOLVER_HPP

#include <string>

/**
 * Resolve .inc/.include directives in a SPICE netlist by inlining files.
 */
class SpiceIncludeResolver {
public:
    /**
     * Resolve includes and return expanded content as a string.
     * @param input_path Path to the input netlist (e.g., ./sram.sp)
     * @param output Expanded netlist content
     * @param error Error message if failed
     * @return true on success
     */
    static bool resolve_to_string(const std::string& input_path, std::string& output, std::string& error);

    /**
     * Resolve includes and write expanded content to output_path.
     * @param input_path Path to the input netlist
     * @param output_path Path to write expanded netlist
     * @param error Error message if failed
     * @return true on success
     */
    static bool resolve_to_file(const std::string& input_path, const std::string& output_path, std::string& error);
};

#endif // SPICE_INCLUDE_RESOLVER_HPP
