#include "capacitance_parser.hpp"
#include "plog/Log.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>

namespace OpenFinRAM {

std::string CapacitanceParser::normalize_pin_name(const std::string& name) const {
    std::string normalized = name;
    
    // Remove leading/trailing whitespace
    size_t start = normalized.find_first_not_of(" \t\r\n");
    size_t end = normalized.find_last_not_of(" \t\r\n");
    
    if (start != std::string::npos && end != std::string::npos) {
        normalized = normalized.substr(start, end - start + 1);
    }
    
    // Convert to uppercase for case-insensitive comparison
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::toupper);
    
    return normalized;
}

bool CapacitanceParser::parse_line(const std::string& line, CapacitanceData& data) {
    // Skip empty lines and header lines
    if (line.empty() || line.find("//") == 0 || line.find("GID") != std::string::npos) {
        return false;
    }
    
    std::istringstream iss(line);
    std::string gid, cell_name, layout_name, source_name;
    double total_c, total_cc, ratio_cc;
    
    // Parse format: GID totalC totalCC ratioCC Cell Layout Source
    if (!(iss >> gid >> total_c >> total_cc >> ratio_cc >> cell_name >> layout_name >> source_name)) {
        return false;
    }
    
    // Use the Source name as the pin name (it's more meaningful)
    data.pin_name = source_name;
    data.total_cap_f = total_c;  // Value is in Farads
    data.total_cap_pf = total_c * 1e12;  // Convert F to pF (1 F = 1e12 pF)
    
    return true;
}

bool CapacitanceParser::parse(const std::string& rep_file_path) {
    LOGI << "Parsing capacitance report: " << rep_file_path;
    
    capacitances_.clear();
    
    std::ifstream file(rep_file_path);
    if (!file.is_open()) {
        LOGE << "Failed to open capacitance report file: " << rep_file_path;
        return false;
    }
    
    std::string line;
    int line_count = 0;
    int parsed_count = 0;
    
    while (std::getline(file, line)) {
        line_count++;
        
        CapacitanceData data;
        if (parse_line(line, data)) {
            std::string normalized_name = normalize_pin_name(data.pin_name);
            capacitances_[normalized_name] = data;
            parsed_count++;
        }
    }
    
    file.close();
    
    LOGI << "Parsed " << parsed_count << " capacitance entries from " << line_count << " lines";
    
    return parsed_count > 0;
}

double CapacitanceParser::get_capacitance_pf(const std::string& pin_name) const {
    std::string normalized = normalize_pin_name(pin_name);
    
    auto it = capacitances_.find(normalized);
    if (it != capacitances_.end()) {
        return it->second.total_cap_pf;
    }
    
    return 0.0;
}

std::vector<CapacitanceData> CapacitanceParser::get_capacitances_by_pattern(const std::string& pattern) const {
    std::vector<CapacitanceData> results;
    std::string normalized_pattern = normalize_pin_name(pattern);
    
    for (const auto& entry : capacitances_) {
        if (entry.first.find(normalized_pattern) != std::string::npos) {
            results.push_back(entry.second);
        }
    }
    
    return results;
}

double CapacitanceParser::get_average_capacitance_pf(const std::string& pattern) const {
    auto matches = get_capacitances_by_pattern(pattern);
    
    if (matches.empty()) {
        return 0.0;
    }
    
    double sum = 0.0;
    for (const auto& data : matches) {
        sum += data.total_cap_pf;
    }
    
    return sum / matches.size();
}

double CapacitanceParser::get_max_capacitance_pf(const std::string& pattern) const {
    auto matches = get_capacitances_by_pattern(pattern);
    
    if (matches.empty()) {
        return 0.0;
    }
    
    double max_cap = 0.0;
    for (const auto& data : matches) {
        if (data.total_cap_pf > max_cap) {
            max_cap = data.total_cap_pf;
        }
    }
    
    return max_cap;
}

void CapacitanceParser::print_summary() const {
    if (capacitances_.empty()) {
        LOGI << "No capacitance data available";
        return;
    }
    
    LOGI << "========================================";
    LOGI << "Capacitance Summary";
    LOGI << "========================================";
    LOGI << "Total entries: " << capacitances_.size();
    
    // Group by pattern and show statistics
    std::vector<std::string> patterns = {"WLT", "WLB", "BLPRECHTN", "BLPRECHBN", 
                                         "YSELT", "YSELB", "YSELTN", "YSELBN",
                                         "SAE", "SAPRECHN", "WRENA", "WRENAN"};
    
    for (const auto& pattern : patterns) {
        auto matches = get_capacitances_by_pattern(pattern);
        if (!matches.empty()) {
            double avg = get_average_capacitance_pf(pattern);
            double max = get_max_capacitance_pf(pattern);
            
            LOGI << "  " << pattern << ":";
            LOGI << "    Count: " << matches.size();
            LOGI << "    Average: " << std::scientific << std::setprecision(3) << avg << " pF";
            LOGI << "    Maximum: " << std::scientific << std::setprecision(3) << max << " pF";
        }
    }
    
    LOGI << "========================================";
}

} // namespace OpenFinRAM
