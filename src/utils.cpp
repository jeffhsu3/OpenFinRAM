#include "utils.hpp"

#include <cmath>
#include <ctime>
#include <fstream>

std::string get_executable_directory() {
    char buffer[PATH_MAX];
    // 讀取 /proc/self/exe 指向的真實路徑
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    
    if (len != -1) {
        buffer[len] = '\0';
        std::string fullPath(buffer);
        
        // 尋找最後一個 '/' 的位置，以取得目錄部分
        size_t lastSlash = fullPath.find_last_of("/");
        if (lastSlash != std::string::npos) {
            return fullPath.substr(0, lastSlash);
        }
        return fullPath;
    } else {
        // 錯誤處理
        return "";
    }
}

std::string join_path(const std::string& base, const std::string& name) {
    if (base.empty() || base == ".") {
        return name;
    }
    if (base.back() == '/') {
        return base + name;
    }
    return base + "/" + name;
}

bool create_directory(const std::string& path, std::string* error) {
    if (path.empty()) {
        return true;
    }
    if (mkdir(path.c_str(), 0755) != 0) {
        if (error) {
            *error = "Failed to create directory: " + path + " (" + std::strerror(errno) + ")";
        }
        return false;
    }
    return true;
}

std::string get_current_timestamp() {
    std::time_t now = std::time(nullptr);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&now));
    return std::string(buf);
}

const std::string& get_run_timestamp() {
    static const std::string ts = get_current_timestamp();
    return ts;
}

bool directory_exists(const std::string& path) {
    struct stat st;
    return (stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
}

bool file_exists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

int get_addr_width(const MainCliOptions& cli_options) {
    return std::ceil(std::log2(cli_options.num_wls)) 
         + std::ceil(std::log2(cli_options.num_banks))
         + 1 + 2; // +1 for top/bottom, +2 for ysel
}

bool copy_file(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ofstream out(dst, std::ios::binary);
    if (!out) {
        return false;
    }
    out << in.rdbuf();
    return true;
}