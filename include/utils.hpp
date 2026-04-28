#ifndef UTILS_HPP
#define UTILS_HPP

#include <unistd.h>  // Linux 系統呼叫
#include <limits.h>  // 定義了 PATH_MAX
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <cstring>

#include "main_config_helpers.hpp"

std::string get_executable_directory();

std::string join_path(const std::string& base, const std::string& name);

bool create_directory(const std::string& path, std::string* error);

std::string get_current_timestamp();

bool directory_exists(const std::string& path);

bool file_exists(const std::string& path);

int get_addr_width(const MainCliOptions& cli_options);

bool copy_file(const std::string& src, const std::string& dst);

#endif // UTILS_HPP
