#include "lef_extractor.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "plog/Log.h"
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

static bool remove_path_if_exists(const std::string& path, bool is_dir, std::string* error) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return true;
    }

    if (is_dir) {
        std::string cmd = "rm -rf \"" + path + "\"";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            if (error) {
                *error = "Failed to remove directory: " + path + ", status: " + std::to_string(rc);
            }
            return false;
        }
        return true;
    }

    if (std::remove(path.c_str()) != 0) {
        if (error) {
            *error = "Failed to remove file: " + path + ", error: " + std::strerror(errno);
        }
        return false;
    }

    return true;
}

static bool write_text_file(const std::string& file_path, const std::string& content, std::string* error) {
    std::ofstream out(file_path);
    if (!out) {
        if (error) {
            *error = "Failed to open file for writing: " + file_path;
        }
        return false;
    }
    out << content;
    return true;
}

static std::string build_export_lef_il(const std::string& cell_name) {
    std::string il;
    il += "absSkillMode()\n";
    il += "absSetLibrary(\"" + cell_name  + "_gds\")\n";
    il += "absSelectCellFrom(\"" + cell_name + "\" \"" + cell_name + "\")\n";
    il += "absDisableUpdate()\n";
    il += "absSetBinOption(\"Core\" \"PinsTextPinMap\" \"(M1 M1)(M2 M2)(M3 M3)(M4 M4)(M5 M5)\")\n";
    il += "absSetBinOption(\"Core\" \"PinsClockNames\" \"CLK clk\")\n";
    il += "absSetBinOption(\"Core\" \"PinsOutputNames\" \"^(Y|Q|QN)([.*])?(!)?$\")\n";
    il += "absSetBinOption(\"Core\" \"PinsBoundaryLayers\" \"BOUNDARY\")\n";
    il += "absSetBinOption(\"Core\" \"ExtractAntennaGate\" \"(Gate (Gate and Active)) \")\n";
    il += "absEnableUpdate()\n";
    il += "absAbstract()\n";
    il += "absSetOption(\"ExportLEFFile\" \"" + cell_name + ".lef\")\n";
    il += "absExportLEF()\n";
    il += "absExit()\n";
    return il;
}

bool export_lef(const std::string& project_root,
                const std::string& cell_name,
                const std::string& gds_path,
                std::string* log_path,
                std::string* error) {
    const std::string cds_lib_path = project_root + "/cds.lib";
    const std::string gds_lib_dir = project_root + "/" + cell_name + "_gds";

    if (!remove_path_if_exists(gds_lib_dir, true, error)) {
        return false;
    }
    if (!remove_path_if_exists(cds_lib_path, false, error)) {
        return false;
    }

    std::string cur_path = get_executable_directory();
    const std::string cds_lib_content = "DEFINE asap7_TechLib " + join_path(cur_path, "tech/TechLib") + "\n";
    if (!write_text_file(cds_lib_path, cds_lib_content, error)) {
        return false;
    }

    std::string strmin_cmd = "tcsh -c 'strmin -library " + cell_name + "_gds" +
                             " -strmFile " + gds_path +
                             " -logFile ./log_read_gds.txt"
                             " -snapToGrid -attachTechFileOfLib asap7_TechLib"
                             " -layerMap " + join_path(cur_path, "tech/TechLib/asap7_TechLib.layermap") + "' > /dev/null 2>&1";

    LOGD << "Running command: " << strmin_cmd;

    int strmin_status = std::system(strmin_cmd.c_str());
    if (strmin_status != 0) {
        if (error) {
            *error = "strmin command failed with status " + std::to_string(strmin_status);
        }
        return false;
    }

    // const std::string cell_dir = project_root + "/" + cell_name;
    // if (!ensure_directory_exists(cell_dir, error)) {
    //     return false;
    // }

    const std::string export_il_path = project_root + "/export_lef.il";
    if (!write_text_file(export_il_path, build_export_lef_il(cell_name), error)) {
        return false;
    }

    // const std::string abstract_log_path = project_root + "/my_abstract.log";
    // if (log_path) {
    //     *log_path = abstract_log_path;
    // }

    std::string abstract_cmd = "tcsh -c 'abstract -nogui -replay export_lef.il -log my_abstract.log' > /dev/null 2>&1";

    int abstract_status = std::system(abstract_cmd.c_str());
    if (abstract_status != 0) {
        if (error) {
            *error = "abstract command failed with status " + std::to_string(abstract_status);
        }
        return false;
    }

    return true;
}

} // namespace OpenFinRAM
