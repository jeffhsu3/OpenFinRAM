#include "spice_include_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::string ltrim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    return s.substr(i);
}

std::string rtrim(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1]))) {
        --i;
    }
    return s.substr(0, i);
}

std::string trim(const std::string& s) {
    return rtrim(ltrim(s));
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_comment_line(const std::string& line) {
    std::string t = ltrim(line);
    if (t.empty()) {
        return false;
    }
    char c = t[0];
    return (c == '*' || c == ';');
}

std::string get_dirname(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

std::string normalize_path(const std::string& path) {
    if (path.empty()) {
        return path;
    }

    bool is_abs = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    std::string token;

    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!token.empty()) {
                if (token == ".") {
                    // skip
                } else if (token == "..") {
                    if (!parts.empty() && parts.back() != "..") {
                        parts.pop_back();
                    } else if (!is_abs) {
                        parts.push_back(token);
                    }
                } else {
                    parts.push_back(token);
                }
                token.clear();
            }
        } else {
            token.push_back(path[i]);
        }
    }

    std::ostringstream oss;
    if (is_abs) {
        oss << '/';
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            oss << '/';
        }
        oss << parts[i];
    }

    std::string normalized = oss.str();
    if (normalized.empty()) {
        return is_abs ? "/" : ".";
    }
    return normalized;
}

std::string join_path(const std::string& base_dir, const std::string& rel) {
    if (rel.empty()) {
        return base_dir;
    }
    if (!rel.empty() && rel[0] == '/') {
        return normalize_path(rel);
    }

    std::string combined = base_dir;
    if (!combined.empty() && combined.back() != '/') {
        combined += '/';
    }
    combined += rel;
    return normalize_path(combined);
}

bool parse_include_path(const std::string& line, std::string& include_path, std::string& error) {
    std::string t = ltrim(line);
    if (t.empty()) {
        return false;
    }

    std::string token;
    size_t i = 0;
    while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i]))) {
        token.push_back(t[i]);
        ++i;
    }

    std::string token_lower = to_lower(token);
    if (token_lower != ".inc" && token_lower != ".include") {
        return false;
    }

    std::string rest = ltrim(t.substr(i));
    if (rest.empty()) {
        error = "Missing include path after " + token;
        return false;
    }

    char quote = rest[0];
    if (quote == '"' || quote == '\'' || quote == '<') {
        char end_quote = (quote == '<') ? '>' : quote;
        size_t end_pos = rest.find(end_quote, 1);
        if (end_pos == std::string::npos) {
            error = "Unterminated include path in line: " + line;
            return false;
        }
        include_path = rest.substr(1, end_pos - 1);
        include_path = trim(include_path);
        return true;
    }

    size_t end = 0;
    while (end < rest.size() && !std::isspace(static_cast<unsigned char>(rest[end]))) {
        ++end;
    }
    include_path = rest.substr(0, end);
    include_path = trim(include_path);

    if (include_path.empty()) {
        error = "Empty include path in line: " + line;
        return false;
    }

    return true;
}

bool resolve_file_internal(
    const std::string& input_path,
    std::string& output,
    std::string& error,
    std::vector<std::string>& include_stack,
    std::unordered_set<std::string>& in_progress
) {
    std::ifstream infile(input_path.c_str());
    if (!infile) {
        error = "Cannot open netlist: " + input_path;
        return false;
    }

    std::string normalized_input = normalize_path(input_path);
    if (in_progress.find(normalized_input) != in_progress.end()) {
        error = "Include cycle detected at: " + normalized_input;
        return false;
    }

    in_progress.insert(normalized_input);
    include_stack.push_back(normalized_input);

    std::string base_dir = get_dirname(normalized_input);
    std::string line;

    while (std::getline(infile, line)) {
        if (is_comment_line(line)) {
            output.append(line);
            output.append("\n");
            continue;
        }

        std::string include_path;
        std::string parse_error;
        bool is_include = parse_include_path(line, include_path, parse_error);

        if (is_include) {
            std::string resolved_path = join_path(base_dir, include_path);
            if (resolved_path.empty()) {
                error = "Failed to resolve include path: " + include_path;
                return false;
            }

            if (!resolve_file_internal(resolved_path, output, error, include_stack, in_progress)) {
                return false;
            }

            output.append("\n");
            continue;
        }

        if (!parse_error.empty()) {
            error = parse_error;
            return false;
        }

        output.append(line);
        output.append("\n");
    }

    include_stack.pop_back();
    in_progress.erase(normalized_input);
    return true;
}

}  // namespace

bool SpiceIncludeResolver::resolve_to_string(const std::string& input_path, std::string& output, std::string& error) {
    output.clear();
    error.clear();

    std::vector<std::string> include_stack;
    std::unordered_set<std::string> in_progress;

    return resolve_file_internal(input_path, output, error, include_stack, in_progress);
}

bool SpiceIncludeResolver::resolve_to_file(const std::string& input_path, const std::string& output_path, std::string& error) {
    std::string resolved;
    if (!resolve_to_string(input_path, resolved, error)) {
        return false;
    }

    std::ofstream outfile(output_path.c_str());
    if (!outfile) {
        error = "Cannot write output netlist: " + output_path;
        return false;
    }

    outfile << resolved;
    return true;
}
