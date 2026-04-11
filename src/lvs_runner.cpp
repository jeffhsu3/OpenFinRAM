#include "lvs_runner.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "utils.hpp"

namespace OpenFinRAM {

static bool ensure_directory_exists(const std::string& dir_path, std::string* error) {
    struct stat st;
    if (stat(dir_path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return true;
        }
        if (error) {
            *error = "Path exists but is not a directory: " + dir_path;
        }
        return false;
    }

    if (mkdir(dir_path.c_str(), 0755) != 0) {
        if (error) {
            *error = "Failed to create directory: " + dir_path + ", error: " + std::strerror(errno);
        }
        return false;
    }

    return true;
}

static bool write_run_control(const std::string& file_path,
                              const std::string& layout_path,
                              const std::string& netlist_path,
                              const std::string& cell_name,
                              std::string* error) {
    std::ofstream out(file_path);
    if (!out) {
        if (error) {
            *error = "Failed to open run control file for writing: " + file_path;
        }
        return false;
    }

    out << "LAYOUT PATH \"" << layout_path << "\"\n";
    out << "LAYOUT SYSTEM GDSII\n";
    out << "LAYOUT PRIMARY \"" << cell_name << "\"\n\n";
    out << "SOURCE PATH \"" << netlist_path << "\"\n";
    out << "SOURCE SYSTEM SPICE\n";
    out << "SOURCE PRIMARY \"" << cell_name << "\"\n\n";
    out << "MASK SVDB DIRECTORY \"svdb\" QUERY\n\n";
    out << "LVS REPORT \"" << cell_name << ".lvs.report\"\n\n";
    out << "LVS POWER NAME \"vdd!\" \"VDD!\" \"vdd\" \"VDD\"\n";
    out << "LVS GROUND NAME \"VSS\" \"VSS!\" \"GND\" \"GND!\"\n\n";
    out << "LVS REPORT MAXIMUM ALL\n\n";
    out << "LVS EXECUTE ERC YES\n\n";
    out << "ERC RESULTS DATABASE \"" << cell_name << ".erc.results\"\n";
    out << "ERC CELL NAME YES CELL SPACE XFORM\n\n";
    out << "ERC SUMMARY REPORT \"" << cell_name << ".erc.summary\" HIER\n\n";
    out << "DRC ICSTATION YES\n\n";
    out << "INCLUDE " << join_path(get_executable_directory(), "tech/calibre/ruledirs/lvs/lvsRules_calibre_asap7.rul") << "\n";

    return true;
}

static bool file_contains_token(const std::string& file_path, const std::string& token) {
    std::ifstream in(file_path);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();
    return content.find(token) != std::string::npos;
}

bool run_lvs(const std::string& project_root,
             const std::string& layout_path,
             const std::string& netlist_path,
             const std::string& cell_name,
             std::string* log_path,
             std::string* error) {
    const std::string lvs_dir = project_root + "/lvs";
    if (!ensure_directory_exists(lvs_dir, error)) {
        return false;
    }

    const std::string run_control_path = lvs_dir + "/_run_control.svrf";
    if (!write_run_control(run_control_path, layout_path, netlist_path, cell_name, error)) {
        return false;
    }

    const std::string lvs_log_path = lvs_dir + "/log_lvs.txt";
    if (log_path) {
        *log_path = lvs_log_path;
    }

    const std::string command = "tcsh -c 'cd " + lvs_dir + " && calibre -lvs -flatten -nowait ./_run_control.svrf > log_lvs.txt'";
    int status = std::system(command.c_str());
    if (status != 0) {
        if (error) {
            *error = "LVS command failed with status " + std::to_string(status);
        }
    }

    const bool ok = file_contains_token(lvs_log_path, "LVS completed. CORRECT.");
    if (!ok && error && error->empty()) {
        *error = "LVS did not report CORRECT. See log for details.";
    }

    return ok;
}

} // namespace OpenFinRAM
