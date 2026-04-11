#include "innovus_tcl_generator.hpp"
#include "plog/Log.h"
#include "gdstk/gdstk.hpp"

#include <fstream>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <regex>
#include <iostream>
#include <errno.h>

#include "utils.hpp"

namespace OpenFinRAM {

namespace {

bool ensure_dir_exists(const std::string& dir_path) {
    if (dir_path.empty()) {
        return true;
    }

    std::string current;
    size_t pos = 0;

    if (dir_path[0] == '/') {
        current = "/";
        pos = 1;
    }

    while (pos <= dir_path.size()) {
        size_t next = dir_path.find('/', pos);
        std::string part = dir_path.substr(pos, next - pos);

        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') {
                current += "/";
            }
            current += part;

            struct stat st;
            if (stat(current.c_str(), &st) != 0) {
                if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                return false;
            }
        }

        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }

    return true;
}

bool write_file_if_missing(const std::string& path, const std::string& content) {
    std::string dir;
    size_t slash_pos = path.find_last_of("/\\");
    if (slash_pos != std::string::npos) {
        dir = path.substr(0, slash_pos);
    }
    if (!dir.empty() && !ensure_dir_exists(dir)) {
        return false;
    }

    std::ofstream file(path.c_str(), std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    file << content;
    file.close();
    return true;
}

}  // namespace

InnovusTclGenerator::InnovusTclGenerator()
    : design_name_("design"),
      site_name_("asap7sc7p5t"),
      site_height_(0.27),  // ASAP7 standard cell height: 0.27 um
      local_cpu_(8),
      remote_cpu_(0),
      layer_map_file_(""),
      gds_merge_file_("") {
}

InnovusTclGenerator::~InnovusTclGenerator() {
}

void InnovusTclGenerator::set_design_name(const std::string& name) {
    design_name_ = name;
}

void InnovusTclGenerator::set_site_name(const std::string& name) {
    site_name_ = name;
}

void InnovusTclGenerator::set_site_height(double height) {
    site_height_ = height;
}

void InnovusTclGenerator::set_cpu_count(int local_cpu, int remote_cpu) {
    local_cpu_ = local_cpu;
    remote_cpu_ = remote_cpu;
}

void InnovusTclGenerator::set_layer_map_file(const std::string& path) {
    layer_map_file_ = path;
}

void InnovusTclGenerator::set_gds_merge_file(const std::string& path) {
    gds_merge_file_ = path;
}

bool InnovusTclGenerator::parse_qor_report(const std::string& qor_file_path) {
    std::ifstream file(qor_file_path);
    if (!file.is_open()) {
        LOGE << "Cannot open QoR report file: " << qor_file_path;
        return false;
    }
    
    qor_report_ = QoRReport();  // Reset
    
    std::string line;
    
    while (std::getline(file, line)) {
        // 直接搜索 Cell Area
        if (line.find("Cell Area:") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value_str = line.substr(pos + 1);
                std::istringstream iss(value_str);
                iss >> qor_report_.cell_area;
            }
        }
    }
    
    file.close();
    
    // 驗證解析結果
    if (qor_report_.cell_area > 0.0) {
        qor_report_.valid = true;
        LOGI << "Parsed QoR report successfully:";
        LOGI << "  Cell Area: " << qor_report_.cell_area << " um^2";
        return true;
    } else {
        LOGE << "Failed to parse Cell Area from QoR report";
        return false;
    }
}

double InnovusTclGenerator::align_to_site_height(double height) const {
    // 向上取整到 site height 的倍數
    int num_sites = static_cast<int>(std::ceil(height / site_height_));
    if (num_sites % 2) {
        num_sites++;  // 保持偶數行
    }
    return num_sites * site_height_;
}

double InnovusTclGenerator::calculate_floorplan_height(double width) const {
    if (!qor_report_.valid) {
        LOGE << "QoR report not valid, cannot calculate height";
        return 0.0;
    }
    
    if (width <= 0.0) {
        LOGE << "Invalid width: " << width;
        return 0.0;
    }
    
    // 目標 utilization: 最高 85%
    const double max_utilization = 0.80;
    
    // 計算最小高度 = Cell Area / Width
    double min_height = qor_report_.cell_area / width;
    
    // 對齊到 site height 的倍數
    double aligned_height = align_to_site_height(min_height);
    
    // 計算 utilization
    double utilization = qor_report_.cell_area / (width * aligned_height);
    
    // 如果 utilization 超過 85%，增加高度
    if (utilization > max_utilization) {
        LOGI << "Initial utilization " << (utilization * 100.0) 
             << "% exceeds maximum " << (max_utilization * 100.0) 
             << "%, increasing height...";
        
        // 計算滿足 85% utilization 的目標高度
        double target_height = qor_report_.cell_area / (width * max_utilization);
        
        // 對齊到 site height
        aligned_height = align_to_site_height(target_height);
        
        // 重新計算 utilization
        utilization = qor_report_.cell_area / (width * aligned_height);
    }
    
    LOGI << "Calculated floorplan height:";
    LOGI << "  Cell Area: " << qor_report_.cell_area << " um^2";
    LOGI << "  Width: " << width << " um";
    LOGI << "  Min Height: " << min_height << " um";
    LOGI << "  Aligned Height: " << aligned_height << " um (" 
         << (int)(aligned_height / site_height_) << " rows)";
    LOGI << "  Utilization: " << (utilization * 100.0) << " %";
    
    return aligned_height;
}

std::string InnovusTclGenerator::generate_floorplan_command(double& width, double& height) const {
    if (height == 0.0) {
        height = calculate_floorplan_height(width);
        if (height == 0.0) {
            LOGE << "Cannot calculate height";
            return "";
        }
    } else {
        // 確保高度對齊到 site height
        height = align_to_site_height(height);
    }
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "floorPlan -site " << site_name_ 
        << " -s " << width << " " << height 
        << " 0.0 0.0 0.0 0.0";
    
    return oss.str();
}

std::string InnovusTclGenerator::generate_global_net_commands() const {
    std::ostringstream oss;
    oss << "clearGlobalNets\n";
    oss << "globalNetConnect VDD -type pgpin -pin VDD -inst * -module {}\n";
    oss << "globalNetConnect VSS -type pgpin -pin VSS -inst * -module {}";
    return oss.str();
}

std::string InnovusTclGenerator::generate_well_tap_command(double interval) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "addWellTap -cell TAPCELL_ASAP7_75t_R -cellInterval " << interval;
    return oss.str();
}

bool InnovusTclGenerator::generate_run_tcl(double width, double height, 
                                           const std::string& output_file,
                                           int num_wlt,
                                           int num_wlb,
                                           int num_ysel,
                                           int addr_width,
                                           int num_mux) const {
    
    if (!qor_report_.valid) {
        LOGE << "QoR report not valid, cannot generate run.tcl";
        return false;
    }
    
    if (height == 0.0) {
        height = calculate_floorplan_height(width);
        if (height == 0.0) {
            return false;
        }
    } else {
        height = align_to_site_height(height);
    }

    // Ensure output directory exists
    std::string out_dir;
    size_t slash_pos = output_file.find_last_of("/\\");
    if (slash_pos != std::string::npos) {
        out_dir = output_file.substr(0, slash_pos);
    }
    if (!out_dir.empty()) {
        if (!ensure_dir_exists(out_dir)) {
            LOGE << "Failed to create output directory for " << output_file;
            return false;
        }
        LOGI << "Output directory ready: " << out_dir;
    }
    
    std::ofstream file(output_file);
    if (!file.is_open()) {
        LOGE << "Cannot open output file: " << output_file;
        return false;
    }
    
    // 設定精度
    file << std::fixed << std::setprecision(3);
    
    // ========================================================================
    // 1. Multi-CPU 設定
    // ========================================================================
    file << "setMultiCpuUsage -localCpu " << local_cpu_ 
         << " -cpuPerRemoteHost 1 -remoteHost " << remote_cpu_ 
         << " -keepLicense true\n";
    file << "setDistributeHost -local\n\n";
    
    // ========================================================================
    // 2. 初始化設計
    // ========================================================================
    file << "source Default.globals\n";
    file << "init_design\n\n";

    // file << "set_dont_touch delay_cell_BUF* true\n\n";
    file << "set_dont_touch [get_cells -hier *] true\n\n";
    
    // ========================================================================
    // 3. Floorplan
    // ========================================================================
    file << generate_floorplan_command(width, height) << "\n\n";
    
    // ========================================================================
    // 4. Global Net Connect
    // ========================================================================
    file << generate_global_net_commands() << "\n\n";
    
    // ========================================================================
    // 5. Well Tap
    // ========================================================================
    file << generate_well_tap_command(width) << "\n\n";
    
    // ========================================================================
    // 6. Physical Pins - 根據固定規則計算位置
    // ========================================================================
    
    double colgrp_width = (width + 0.054 * 2) / num_mux;
    for (int mux = 0; mux < num_mux; ++mux) {
        const double pin_width = 0.018;
        LOGD << mux * 0.027 / num_mux;
        double x_pos = 0.207 + mux * (colgrp_width);  // WLT 第一根位置
        
        // WLT pins
        file << "# WLT (Word Line Top) pins\n";
        for (int i = 0; i < num_wlt; ++i) {
            double x_left = x_pos;
            double x_right = x_pos + pin_width;
            file << "createPhysicalPin wlt[" << i + mux * num_wlt << "] -layer 3"
                << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n";
            x_pos += 0.108;  // spacing to next WLT
        }
        file << "\n";
        
        // blprechtn (跟 WLT 最右邊 spacing 0.303)
        x_pos += 0.321 - 0.108;  // 減去上面多加的 spacing
        file << "# blprechtn pin\n";
        double x_left = x_pos;
        double x_right = x_pos + pin_width;
        file << "createPhysicalPin blprechtn[" << mux << "] -layer 3"
            << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n\n";
        
        // yseltn (跟 blprechtn spacing 0.054, 兩根 spacing 0.018)
        x_pos += 0.072;
        file << "# yseltn pins\n";
        for (int i = 0; i < num_ysel; ++i) {
            x_left = x_pos;
            x_right = x_pos + pin_width;
            file << "createPhysicalPin yseltn[" << i + mux * num_ysel << "] -layer 3"
                << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n";
            x_pos += 0.036;
        }
        file << "\n";
        
        // yselt (緊接著 yseltn)
        file << "# yselt pins\n";
        for (int i = 0; i < num_ysel; ++i) {
            x_left = x_pos;
            x_right = x_pos + pin_width;
            file << "createPhysicalPin yselt[" << i + mux * num_ysel << "] -layer 3"
                << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n";
            x_pos += 0.036;
        }
        file << "\n";

        // oe_n pin
        if (mux == 0) {
            file << "createPhysicalPin oe_n -layer 3"
                << " -rect " << x_pos << " 0.05 " << x_pos + pin_width << " " << height - 0.05 << "\n\n";
        }
        
        // wrena, saprechn, sae, wrenan (跟 yselt 最右邊 spacing 0.255, 兩根 spacing 0.018)
        x_pos += 0.273 - 0.036;  // 減去上面多加的 spacing
        double wrenan_left;
        file << "# Control signal pins\n";
        const char* ctrl_signals[] = {"wrena", "saprechn", "sae", "wrenan"};
        for (const char* sig : ctrl_signals) {
            if (std::string(sig) == "saprechn") {
                x_pos += 0.036;
            }
            x_left = x_pos;
            x_right = x_pos + pin_width;
            file << "createPhysicalPin " << sig << "[" << mux << "] -layer 3"
                << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n";
            if (std::string(sig) == "wrenan") {
                wrenan_left = x_left;  // 記錄 wrenan 的位置
            }
            x_pos += 0.036;
        }

        // oeb_out pin
        file << "createPhysicalPin oeb_out[" << mux << "] -layer 3"
            << " -rect " << x_pos + 0.387 + 0.412 << " -0.150 " << x_pos + 0.387 + 0.412 + pin_width << " " << height + 0.150 << "\n";

        // oe_out pin
        file << "createPhysicalPin oe_out[" << mux << "] -layer 3"
            << " -rect " << x_pos + 0.459 + 0.561 << " -0.150 " << x_pos + 0.459 + 0.561 + pin_width << " " << height + 0.150 << "\n";

        file << "\n";
        
        // A pins (跟 wrenan 右邊 spacing 0.036)
        if (mux == 0) {
            file << "# Address pins\n";
            for (int i = 0; i < addr_width; ++i) {
                if (i == 7) {
                    // Add 0.018 + 0.026 spacing between A[6] and A[7]
                    x_pos += 0.044;
                }
                if (i == 9) {
                    x_pos += 0.553;
                }

                x_left = x_pos;
                x_right = x_pos + pin_width;
                file << "createPhysicalPin A[" << i << "] -layer 3"
                    << " -rect " << x_left << " 0.05 " << x_right << " " << height - 0.05 << "\n";
                x_pos += 0.036;
            }
            file << "\n";
        

            x_pos = wrenan_left + 0.477 + 0.018;
            const char* ctrl_signals_2[] = {"ce_n", "we_n", "clk", "rst_n"};
            for (const char* sig : ctrl_signals_2) {
                x_left = x_pos;
                x_right = x_pos + pin_width;
                file << "createPhysicalPin " << sig << " -layer 3"
                    << " -rect " << x_left << " 0.05 " << x_right << " " << height - 0.05 << "\n";
                x_pos += 0.036;
            }
            file << "\n";

        }
        
        // yselb (跟 wrenan spacing 0.669, 兩根 spacing 0.036)
        x_pos = wrenan_left + 0.669 + 0.54;
        file << "# yselb pins\n";
        for (int i = 0; i < num_ysel; ++i) {
            x_left = x_pos;
            x_right = x_pos + pin_width;
            file << "createPhysicalPin yselb[" << i + mux * num_ysel << "] -layer 3"
                << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n";
            x_pos += 0.036;
        }
        file << "\n";
        
        // yselbn (緊接著 yselb)
        file << "# yselbn pins\n";
        for (int i = 0; i < num_ysel; ++i) {
            x_left = x_pos;
            x_right = x_pos + pin_width;
            file << "createPhysicalPin yselbn[" << i + mux * num_ysel << "] -layer 3"
                << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n";
            x_pos += 0.036;
        }
        file << "\n";
        
        // blprechbn (跟 yselb 最右邊距離 0.054)
        x_pos += 0.072 - 0.036;
        file << "# blprechbn pin\n";
        x_left = x_pos;
        x_right = x_pos + pin_width;
        file << "createPhysicalPin blprechbn[" << mux << "] -layer 3"
            << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n\n";
        
        // WLB pins (跟 blprechbn 距離 0.303, 兩根 spacing 0.09)
        x_pos += 0.321;
        file << "# WLB (Word Line Bottom) pins\n";
        for (int i = 0; i < num_wlb; ++i) {
            x_left = x_pos;
            x_right = x_pos + pin_width;
            file << "createPhysicalPin wlb[" << i + mux * num_wlb << "] -layer 3"
                << " -rect " << x_left << " -0.150 " << x_right << " " << height + 0.150 << "\n";
            x_pos += 0.108;
        }
        file << "\n";
        
        LOGI << "Generated " << num_wlt << " WLT pins, " << num_wlb << " WLB pins, "
            << num_ysel << " ysel pins per group";

        // Generate VDD/VSS pins
        file << "# Power pins\n";
        
        // 用於記錄所有 power stripe 的資訊
        struct PowerStripe {
            std::string net_name;  // VDD or VSS
            double x_left;
            double x_right;
            double width;  // stripe 寬度
        };
        std::vector<PowerStripe> power_stripes;
        
        // VDD
        x_pos = 0.063 + mux * (colgrp_width);
        x_right = x_pos + pin_width;
        if (mux == 0) {
            file << "createPGPin VDD -geom M3 " << x_pos << " -0.150 " << x_right << " " << height + 0.150 << "\n";  
        }
        file << "add_shape -net VDD -layer M3 -rect {" << x_pos << " -0.150 " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VDD", x_pos, x_right, pin_width});
        
        
        x_pos += 0.099 * 2 + num_wlt * 0.108;
        x_right = x_pos + pin_width;
        file << "add_shape -net VDD -layer M3 -rect {" << x_pos << " -0.150 " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VDD", x_pos, x_right, pin_width});

        x_pos += 0.648;
        x_right = x_pos + 0.09;
        file << "add_shape -net VDD -layer M3 -rect {" << x_pos << " -0.150 " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VDD", x_pos, x_right, 0.09});

        x_pos += 1.404 + 0.54;
        x_right = x_pos + pin_width;
        file << "add_shape -net VDD -layer M3 -rect {" << x_pos << " -0.150 " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VDD", x_pos, x_right, pin_width});

        x_pos += 0.099 * 2 + num_wlb * 0.108;
        x_right = x_pos + pin_width;
        file << "add_shape -net VDD -layer M3 -rect {" << x_pos << " -0.150 " << x_right << " " << height + 0.150 << "} -shape STRIPE\n\n";
        power_stripes.push_back({"VDD", x_pos, x_right, pin_width});

        // VSS
        x_pos = 0.099 + mux * (colgrp_width);
        x_right = x_pos + pin_width;
        if (mux == 0) {
            file << "createPGPin VSS -geom M3 " << x_pos << " -0.150 " << x_right << " " << height + 0.150 << "\n";
        }
        file << "add_shape -net VSS -layer M3 -rect {" << x_pos << " -0.150 " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VSS", x_pos, x_right, pin_width});

        x_pos += 0.063 + 0.045 + num_wlt * 0.108;
        x_right = x_pos + pin_width;
        file << "add_shape -net VSS -layer M3 -rect {" << x_pos << " " << -0.150 << " " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VSS", x_pos, x_right, pin_width});

        x_pos += 0.846;
        x_right = x_pos + 0.018;
        file << "add_shape -net VSS -layer M3 -rect {" << x_pos << " " << -0.150 << " " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VSS", x_pos, x_right, 0.018});

        x_pos += 0.396;
        x_right = x_pos + 0.026;
        file << "add_shape -net VSS -layer M3 -rect {" << x_pos << " " << -0.150 << " " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VSS", x_pos, x_right, 0.026});

        // x_pos += 0.153;
        // x_right = x_pos + 0.036;
        // file << "add_shape -net VSS -layer M3 -rect {" << x_pos << " " << -0.150 << " " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        // power_stripes.push_back({"VSS", x_pos, x_right, 0.036});

        x_pos += 0.918 + 0.54;
        x_right = x_pos + pin_width;
        file << "add_shape -net VSS -layer M3 -rect {" << x_pos << " " << -0.150 << " " << x_right << " " << height + 0.150 << "} -shape STRIPE\n";
        power_stripes.push_back({"VSS", x_pos, x_right, pin_width});

        x_pos += 0.063 + 0.045 + num_wlb * 0.108;
        x_right = x_pos + pin_width;
        file << "add_shape -net VSS -layer M3 -rect {" << x_pos << " " << -0.150 << " " << x_right << " " << height + 0.150 << "} -shape STRIPE\n\n";
        power_stripes.push_back({"VSS", x_pos, x_right, pin_width});
        
        // ========================================================================
        // 8. Define V2 vias and add vias on power stripes
        // ========================================================================
        file << "# Define V2 vias for different stripe widths\n";
        
        const double via_height = 0.018;
        const double enclosure = 0.005;  // 只在 x 方向有 enclosure
        
        // 收集所有獨特的 stripe 寬度
        std::vector<double> unique_widths;
        for (const auto& stripe : power_stripes) {
            bool found = false;
            for (double w : unique_widths) {
                if (std::abs(w - stripe.width) < 0.0001) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                unique_widths.push_back(stripe.width);
            }
        }
        
        // 為每個獨特的寬度創建 via definition
        std::map<double, std::string> width_to_via_name;
        int via_idx = 0;
        for (double w : unique_widths) {
            // V2 寬度 = M3 寬度
            double v2_width = w;
            double v2_half_width = v2_width / 2.0;
            double v2_half_height = via_height / 2.0;
            
            // M2 寬度 = V2 寬度 + 2 * enclosure (左右各凸 0.005)
            double m2_width = v2_width + 2.0 * enclosure;
            double m2_half_width = m2_width / 2.0;
            double m2_half_height = via_height / 2.0;  // 上下不凸
            
            // M3 寬度 = V2 寬度 (等寬)
            double m3_half_width = v2_half_width;
            double m3_half_height = via_height / 2.0;  // 上下不凸
            
            std::ostringstream via_name;
            via_name << "V2_W" << std::fixed << std::setprecision(0) << (w * 1000);  // 例如: V2_W18, V2_W90
            std::string via_name_str = via_name.str();
            width_to_via_name[w] = via_name_str;
            
            if (mux == 0) {
                file << "add_via_definition -name " << via_name_str
                    << " -cut_layer V2"
                    << " -cut_rects {{" << -v2_half_width << " " << -v2_half_height << " " 
                    << v2_half_width << " " << v2_half_height << "}}"
                    << " -bottom_layer M2"
                    << " -bottom_rects {{" << -m2_half_width << " " << -m2_half_height << " " 
                    << m2_half_width << " " << m2_half_height << "}}"
                    << " -top_layer M3"
                    << " -top_rects {{" << -m3_half_width << " " << -m3_half_height << " " 
                    << m3_half_width << " " << m3_half_height << "}}\n";
            }
            
            via_idx++;
        }
        file << "\n";
        
        file << "# V2 vias on power stripes\n";
        
        const double via_row_spacing = 0.540;  // 2 * site_height_ = 0.540 um
        
        // 為每個 power stripe 添加 V2 via
        for (const auto& stripe : power_stripes) {
            double stripe_x_center = (stripe.x_left + stripe.x_right) / 2.0;
            std::string via_name = width_to_via_name[stripe.width];
            
            // VDD 從 y=0.0 開始，VSS 從 y=0.270 開始，間隔都是 0.540
            double y_start = (stripe.net_name == "VDD") ? 0.0 : 0.270;
            
            // 沿著 y 方向添加 via
            for (double y_center = y_start; y_center < height + 0.09; y_center += via_row_spacing) {
                file << "add_via -net " << stripe.net_name 
                    << " -pt {" << stripe_x_center << " " << y_center << "}"
                    << " -via " << via_name << "\n";
            }
        }
        file << "\n";
    }

    
    // ========================================================================
    // 7. Place and Route
    // ========================================================================
    // 7. 註解：後續步驟
    // ========================================================================
    // file << "# TODO: Add createPhysicalPin commands for other ports\n";
    file << "\nplace_design\n";
    // file << "create_ccopt_clock_tree_spec\n";
    // file << "ccopt_design\n";
    file << "sroute -connect corePin\n";
    file << "routeDesign\n\n";
    file << "addFiller -cell {DECAPx10_ASAP7_75t_R  } -prefix FILLER_DECAP_\n";
    file << "addFiller -cell {DECAPx6_ASAP7_75t_R   } -prefix FILLER_DECAP_\n";
    file << "addFiller -cell {DECAPx4_ASAP7_75t_R  } -prefix FILLER_DECAP_\n";
    file << "addFiller -cell {TAPCELL_ASAP7_75t_R } -prefix FILLER_TAP_\n";
    file << "addFiller -cell {FILLER_ASAP7_75t_R FILLERxp5_ASAP7_75t_R } -prefix FILLER_\n\n";
    file << "streamOut ctrl_decode.gds -mapFile " << join_path(get_executable_directory(), "tech/TechLib/asap7_fromAPR.layermap") << " -libName DesignLib -merge { " << join_path(get_executable_directory(), "tech/gds/asap7sc7p5t_28_R_220121a.gds") << " } -uniquifyCellNames -dieAreaAsBoundary -units 4000 -mode ALL\n";
    file << "saveNetlist netlist_for_lvs.v -phys -excludeCellInst {FILLERxp5_ASAP7_75t_R FILLER_ASAP7_75t_R TAPCELL_ASAP7_75t_R TAPCELL_WITH_FILLER_ASAP7_75t_R} -excludeLeafCell\n\n";
    file << "exit\n";

    file.close();
    
    LOGI << "Generated run.tcl: " << output_file;
    LOGI << "  Design: " << design_name_;
    LOGI << "  Floorplan: " << width << " x " << height << " um";
    LOGI << "  Site: " << site_name_ << " (height: " << site_height_ << " um)";
    
    return true;
}

std::string InnovusTclGenerator::generate_physical_pin_commands(
    const std::vector<PinInfo>& pins,
    int layer,
    double pin_width,
    double core_height) const {
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    
    for (const auto& pin : pins) {
        // 將 pin 對齊到 label 位置的中心
        double pin_center = pin.x_position;
        double x_left = pin_center - pin_width / 2.0;
        double x_right = pin_center + pin_width / 2.0;
        double y_bottom = 0.0;
        double y_top = core_height;
        
        oss << "createPhysicalPin " << pin.name 
            << " -layer " << layer
            << " -rect " << x_left << " " << y_bottom << " " << x_right << " " << y_top
            << "\n";
    }
    
    return oss.str();
}

bool InnovusTclGenerator::run_innovus(const std::string& tcl_file,
                                      const std::string& work_dir,
                                      const std::string& log_file) const {
    // Ensure required Innovus setup files exist (hard-coded defaults)
    const std::string default_globals_path = work_dir + "/Default.globals";
    const std::string default_view_path = work_dir + "/Default.view";
    const std::string timing_sdc_path = work_dir + "/timing.sdc";

    const std::string default_globals_content =
        "###############################################################\n"
        "#  Generated by:      Cadence Innovus 21.17-s075_1\n"
        "#  OS:                Linux x86_64(Host ID rhel8-server)\n"
        "#  Generated on:      Sun Nov 23 12:31:47 2025\n"
        "#  Design:            \n"
        "#  Command:           save_global Default.globals\n"
        "###############################################################\n"
        "#\n"
        "# Version 1.1\n"
        "#\n"
        "\n"
        "set ::TimeLib::tsgMarkCellLatchConstructFlag 1\n"
        "set conf_qxconf_file {NULL}\n"
        "set conf_qxlib_file {NULL}\n"
        "set dbgDualViewAwareXTree 1\n"
        "set defHierChar {/}\n"
        "set distributed_client_message_echo {1}\n"
        "set distributed_mmmc_disable_reports_auto_redirection {0}\n"
        "set enable_ilm_dual_view_gui_and_attribute 1\n"
        "set enc_enable_print_mode_command_reset_options 1\n"
        "set init_gnd_net {VSS}\n"
        "set init_lef_file {" + join_path(get_executable_directory(), "tech/lef/asap7_tech.lef") + " " + join_path(get_executable_directory(), "tech/lef/asap7sc7p5t_28_R.lef") + "}\n"
        "set init_mmmc_file {Default.view}\n"
        "set init_pwr_net {VDD}\n"
        "set init_top_cell {ctrl_decode}\n"
        "set init_verilog {../verilog/netlist.v}\n"
        "set latch_time_borrow_mode max_borrow\n";

    const std::string default_view_content =
        "# Version:1.0 MMMC View Definition File\n"
        "# Do Not Remove Above Line\n"
        "create_rc_corner -name typical_rc -T {25} -preRoute_res {1.0} -preRoute_cap {1.0} -preRoute_clkres {0.0} -preRoute_clkcap {0.0} -postRoute_res {1.0} -postRoute_cap {1.0} -postRoute_xcap {1.0} -postRoute_clkres {0.0} -postRoute_clkcap {0.0} -qx_tech_file {" + join_path(get_executable_directory(), "tech/qrc/qrcTechFile_typ03_unscaledV02") + "}\n"
        "create_op_cond -name typical_opc -library_file {" + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_AO_RVT_TT.lib") + "} -P {1} -V {0.7} -T {25}\n"
        "create_op_cond -name typical_opc -library_file {" + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_INVBUF_RVT_TT.lib") + "} -P {1} -V {0.7} -T {25}\n"
        "create_op_cond -name typical_opc -library_file {" + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_OA_RVT_TT.lib") + "} -P {1} -V {0.7} -T {25}\n"
        "create_op_cond -name typical_opc -library_file {" + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_SEQ_RVT_TT.lib") + "} -P {1} -V {0.7} -T {25}\n"
        "create_op_cond -name typical_opc -library_file {" + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_SIMPLE_RVT_TT.lib") + "} -P {1} -V {0.7} -T {25}\n"
        "create_library_set -name AllLib -timing {" + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_AO_RVT_TT.lib") + " " + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_INVBUF_RVT_TT.lib") + " " + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_OA_RVT_TT.lib") + " " + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_SEQ_RVT_TT.lib") + " " + join_path(get_executable_directory(), "tech/lib/asap7sc7p5t_SIMPLE_RVT_TT.lib") + "}\n"
        "create_constraint_mode -name delay1ns2 -sdc_files {timing.sdc}\n"
        "create_delay_corner -name typical_dc -rc_corner {typical_rc} -early_library_set {AllLib} -late_library_set {AllLib}\n"
        "create_analysis_view -name typical_view -constraint_mode {delay1ns2} -delay_corner {typical_dc}\n"
        "set_analysis_view -setup {typical_view} -hold {typical_view}\n";

    const std::string timing_sdc_content =
        "create_clock -name clk -period 10.0 [get_ports clk]\n";

    if (!write_file_if_missing(default_globals_path, default_globals_content)) {
        LOGE << "Failed to create " << default_globals_path;
        return false;
    }
    if (!write_file_if_missing(default_view_path, default_view_content)) {
        LOGE << "Failed to create " << default_view_path;
        return false;
    }
    if (!write_file_if_missing(timing_sdc_path, timing_sdc_content)) {
        LOGE << "Failed to create " << timing_sdc_path;
        return false;
    }

    // 檢查 TCL 檔案是否存在
    std::ifstream tcl_check(tcl_file);
    if (!tcl_check.good()) {
        LOGE << "TCL file not found: " << tcl_file;
        return false;
    }
    tcl_check.close();
    
    // 檢查工作目錄是否存在
    struct stat info;
    if (stat(work_dir.c_str(), &info) != 0) {
        LOGE << "Work directory does not exist: " << work_dir;
        return false;
    }
    if (!(info.st_mode & S_IFDIR)) {
        LOGE << "Work directory path is not a directory: " << work_dir;
        return false;
    }
    
    LOGI << "Running Innovus...";
    LOGI << "  TCL file: " << tcl_file;
    LOGI << "  Work directory: " << work_dir;
    LOGI << "  Log file: " << log_file;
    
    // 建立 Innovus 命令
    std::ostringstream cmd;
    std::string tcl_file_name = tcl_file.substr(tcl_file.find_last_of("/\\") + 1);  // 只取檔名
    cmd << "tcsh -c 'cd " << work_dir << " && ";
    cmd << "innovus -no_gui -files " << tcl_file_name << "'";
    
    LOGI << "Executing command: " << cmd.str();
    
    // 執行命令
    int result = system(cmd.str().c_str());
    
    if (result != 0) {
        LOGE << "Innovus execution failed with exit code: " << result;
        LOGE << "Please check log file: " << work_dir << "/" << log_file;
        return false;
    }
    
    LOGI << "Innovus execution completed successfully";
    return true;
}

bool InnovusTclGenerator::run_v2lvs(const std::string& work_dir,
                                    const std::string& verilog_file,
                                    const std::string& spice_file,
                                    const std::string& cdl_file) const {
    // 檢查工作目錄是否存在
    struct stat info;
    if (stat(work_dir.c_str(), &info) != 0) {
        LOGE << "Work directory does not exist: " << work_dir;
        return false;
    }
    if (!(info.st_mode & S_IFDIR)) {
        LOGE << "Work directory path is not a directory: " << work_dir;
        return false;
    }
    
    LOGI << "Running v2lvs...";
    LOGI << "  Work directory: " << work_dir;
    LOGI << "  Input Verilog: " << verilog_file;
    LOGI << "  Output SPICE: " << spice_file;
    LOGI << "  CDL file: " << cdl_file;
    
    // 建立 v2lvs 命令
    std::ostringstream cmd;
    cmd << "tcsh -c 'cd " << work_dir << " && ";
    cmd << "v2lvs -v " << verilog_file 
        << " -o " << spice_file 
        << " -s " << cdl_file << "'";
    
    LOGI << "Executing command: " << cmd.str();
    
    // 執行命令
    int result = system(cmd.str().c_str());
    
    if (result != 0) {
        LOGE << "v2lvs execution failed with exit code: " << result;
        return false;
    }
    
    LOGI << "v2lvs execution completed successfully";
    LOGI << "  Generated: " << work_dir << "/" << spice_file;
    return true;
}

// ============================================================================
// Netlist Post-processing Helper Methods
// ============================================================================

bool InnovusTclGenerator::file_exists(const std::string& path) const {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

std::vector<std::string> InnovusTclGenerator::read_file(const std::string& filepath) const {
    std::vector<std::string> lines;
    std::ifstream file(filepath.c_str());
    
    if (!file.is_open()) {
        LOGE << "Cannot open file " << filepath;
        return lines;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    file.close();
    
    LOGI << "Read " << lines.size() << " lines from " << filepath;
    return lines;
}

bool InnovusTclGenerator::write_file(const std::string& filepath,
                                     const std::vector<std::string>& lines) const {
    std::ofstream file(filepath.c_str());
    
    if (!file.is_open()) {
        LOGE << "Cannot open file for writing " << filepath;
        return false;
    }
    
    for (const auto& line : lines) {
        file << line << "\n";
    }
    file.close();
    
    LOGI << "Wrote " << lines.size() << " lines to " << filepath;
    return true;
}

std::vector<std::string> InnovusTclGenerator::merge_continuation_lines(
    const std::vector<std::string>& lines) const {
    
    std::vector<std::string> result;
    
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        
        if (!line.empty() && line[0] == '+') {
            // Continuation line - append to previous line
            if (!result.empty()) {
                result.back() += " " + line.substr(1);
            }
        } else {
            result.push_back(line);
        }
    }
    
    LOGI << "After merging continuation lines: " << result.size() << " lines";
    return result;
}

std::vector<std::string> InnovusTclGenerator::add_power_to_subckt(
    const std::vector<std::string>& lines) const {
    
    std::vector<std::string> result;
    int subckt_count = 0;
    
    for (const auto& line : lines) {
        if (line.substr(0, 7) == ".SUBCKT" || line.substr(0, 7) == ".subckt") {
            // Add VDD VSS to the end of SUBCKT line
            std::string new_line = line;
            if (line.back() != '\n') {
                new_line += " VDD VSS";
            } else {
                new_line = line.substr(0, line.length() - 1) + " VDD VSS";
            }
            result.push_back(new_line);
            subckt_count++;
        } else {
            result.push_back(line);
        }
    }
    
    LOGI << "Added VDD VSS to " << subckt_count << " SUBCKT definitions";
    return result;
}

bool InnovusTclGenerator::parse_cdl_file(const std::string& cdl_path) {
    // Expand home directory in path
    std::string expanded_path = cdl_path;
    if (!expanded_path.empty() && expanded_path[0] == '~') {
        const char* home = getenv("HOME");
        if (home != nullptr) {
            expanded_path = std::string(home) + expanded_path.substr(1);
        }
    }
    
    LOGI << "Parsing CDL file: " << expanded_path;
    
    if (!file_exists(expanded_path)) {
        LOGW << "CDL file not found: " << expanded_path;
        LOGW << "Continuing with netlist parsing only";
        return false;
    }
    
    std::vector<std::string> cdl_lines = read_file(expanded_path);
    
    for (const auto& line : cdl_lines) {
        if (line.substr(0, 7) == ".SUBCKT" || line.substr(0, 7) == ".subckt") {
            std::istringstream iss(line);
            std::string token;
            std::vector<std::string> tokens;
            
            while (iss >> token) {
                tokens.push_back(token);
            }
            
            if (tokens.size() >= 2) {
                std::string subckt_name = tokens[1];
                std::vector<std::string> pins(tokens.begin() + 2, tokens.end());
                subckt_dict_[subckt_name] = pins;
            }
        }
    }
    
    LOGI << "Parsed " << subckt_dict_.size() << " subcircuits from CDL";
    return true;
}

void InnovusTclGenerator::parse_subckt_from_lines(const std::vector<std::string>& lines) {
    int new_subckt_count = 0;
    
    for (const auto& line : lines) {
        if (line.substr(0, 7) == ".SUBCKT" || line.substr(0, 7) == ".subckt") {
            std::istringstream iss(line);
            std::string token;
            std::vector<std::string> tokens;
            
            while (iss >> token) {
                tokens.push_back(token);
            }
            
            if (tokens.size() >= 2) {
                std::string subckt_name = tokens[1];
                std::vector<std::string> pins(tokens.begin() + 2, tokens.end());
                
                // Only add if not already in CDL dict
                if (subckt_dict_.find(subckt_name) == subckt_dict_.end()) {
                    subckt_dict_[subckt_name] = pins;
                    new_subckt_count++;
                }
            }
        }
    }
    
    LOGI << "Parsed " << new_subckt_count << " additional subcircuits from netlist";
    LOGI << "Total " << subckt_dict_.size() << " subcircuits available";
}

std::vector<std::string> InnovusTclGenerator::expand_pins(
    const std::vector<std::string>& lines) {
    
    std::vector<std::string> result;
    int pin_replacements = 0;
    int pin_errors = 0;
    
    for (const auto& line : lines) {
        if (line.find("$PINS") != std::string::npos) {
            // Parse the line with $PINS
            std::istringstream iss(line);
            std::string instance_name, subckt_name, dummy;
            std::vector<std::string> pin_mappings;
            
            iss >> instance_name >> subckt_name >> dummy; // Skip "$PINS"
            
            std::string mapping;
            while (iss >> mapping) {
                pin_mappings.push_back(mapping);
            }
            
            // Look up subcircuit definition
            if (subckt_dict_.find(subckt_name) != subckt_dict_.end()) {
                const auto& subckt_pins = subckt_dict_[subckt_name];
                
                // Parse pin mappings (format: PIN=NET)
                std::map<std::string, std::string> pin_map;
                for (const auto& pmapping : pin_mappings) {
                    size_t eq_pos = pmapping.find('=');
                    if (eq_pos != std::string::npos) {
                        std::string pin = pmapping.substr(0, eq_pos);
                        std::string net = pmapping.substr(eq_pos + 1);
                        
                        // Convert to uppercase for comparison
                        std::transform(pin.begin(), pin.end(), pin.begin(), ::toupper);
                        pin_map[pin] = net;
                    }
                }
                
                // Build connection list
                std::string new_line = instance_name;
                for (const auto& pin : subckt_pins) {
                    std::string pin_upper = pin;
                    std::transform(pin_upper.begin(), pin_upper.end(), pin_upper.begin(), ::toupper);
                    
                    if (pin_map.find(pin_upper) != pin_map.end()) {
                        new_line += " " + pin_map[pin_upper];
                    } else if (pin == "VDD" || pin == "VSS") {
                        new_line += " " + pin;
                    } else {
                        LOGW << "Pin " << pin << " not found in mapping for " << instance_name;
                        new_line += " " + pin;
                        pin_errors++;
                    }
                }
                
                new_line += " " + subckt_name;
                result.push_back(new_line);
                pin_replacements++;
            } else {
                LOGW << "Subcircuit " << subckt_name << " not found in definitions";
                result.push_back(line);
                pin_errors++;
            }
        } else {
            result.push_back(line);
        }
    }
    
    if (pin_replacements > 0) {
        LOGI << "Expanded " << pin_replacements << " instances with $PINS";
    }
    if (pin_errors > 0) {
        LOGW << "Encountered " << pin_errors << " pin mapping warnings";
    }
    
    return result;
}

std::vector<std::string> InnovusTclGenerator::process_connect_directives(
    const std::vector<std::string>& lines) const {
    
    std::vector<std::string> result;
    int connect_directives = 0;
    
    for (const auto& line : lines) {
        if (line.substr(0, 9) == "*.CONNECT" || line.substr(0, 9) == "*.connect") {
            // Parse *.CONNECT net1 net2
            std::istringstream iss(line);
            std::string token, net1, net2;
            iss >> token >> net1 >> net2;  // Skip "*.CONNECT"
            
            if (!net1.empty() && !net2.empty()) {
                // Create buffer instance
                std::string instance = "X" + net1 + net2;
                std::string new_line = instance + " " + net2 + " VDD VSS " + net1 + " BUFx2_ASAP7_75t_R";
                result.push_back(new_line);
                connect_directives++;
            } else {
                result.push_back(line);
            }
        } else {
            result.push_back(line);
        }
    }
    
    if (connect_directives > 0) {
        LOGI << "Processed " << connect_directives << " .CONNECT directives";
    }
    
    return result;
}

bool InnovusTclGenerator::post_process_netlist(const std::string& work_dir,
                                               const std::string& spice_file,
                                               const std::string& cdl_file) {
    LOGI << "========================================";
    LOGI << "Post-processing SPICE Netlist";
    LOGI << "========================================";
    
    // Construct full path to netlist
    std::string netlist_path = work_dir + "/" + spice_file;
    
    if (!file_exists(netlist_path)) {
        LOGE << "Netlist file not found: " << netlist_path;
        return false;
    }
    
    // Step 1: Read file
    LOGI << "Step 1: Reading netlist...";
    std::vector<std::string> lines = read_file(netlist_path);
    if (lines.empty()) {
        LOGE << "Failed to read netlist or file is empty";
        return false;
    }
    
    // Step 2: Merge continuation lines
    LOGI << "Step 2: Merging continuation lines...";
    lines = merge_continuation_lines(lines);
    
    // // Step 3: Add VDD VSS to SUBCKT definitions
    // LOGI << "Step 3: Adding power to SUBCKT definitions...";
    // lines = add_power_to_subckt(lines);
    
    // Step 4: Parse CDL file
    LOGI << "Step 4: Parsing CDL definitions...";
    parse_cdl_file(cdl_file);
    
    // Step 5: Parse subcircuits from netlist
    LOGI << "Step 5: Parsing subcircuits from netlist...";
    parse_subckt_from_lines(lines);
    
    // Step 6: Expand $PINS format
    LOGI << "Step 6: Expanding $PINS format...";
    lines = expand_pins(lines);
    
    // Step 7: Process .CONNECT directives
    LOGI << "Step 7: Processing .CONNECT directives...";
    lines = process_connect_directives(lines);
    
    // Step 8: Write back to file
    LOGI << "Step 8: Writing processed netlist...";
    if (!write_file(netlist_path, lines)) {
        LOGE << "Failed to write netlist";
        return false;
    }
    
    LOGI << "========================================";
    LOGI << "Post-processing completed";
    LOGI << "Output: " << netlist_path;
    LOGI << "========================================";
    
    return true;
}

} // namespace OpenFinRAM
