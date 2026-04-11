#ifndef OPENFINRAM_INNOVUS_TCL_GENERATOR_HPP_
#define OPENFINRAM_INNOVUS_TCL_GENERATOR_HPP_

#include <string>
#include <vector>
#include <map>

namespace OpenFinRAM {

// ============================================================================
// Pin 資訊結構
// ============================================================================
struct PinInfo {
    std::string name;     // Pin 名稱（例如：wlt[0], wlb[15]）
    double x_position;    // X 座標 (um)
    
    PinInfo() : name(""), x_position(0.0) {}
    PinInfo(const std::string& n, double x) : name(n), x_position(x) {}
};

// ============================================================================
// QoR Report Parser
// 解析 Design Compiler 產生的 QoR report，提取面積資訊
// ============================================================================
struct QoRReport {
    double cell_area;           // Cell Area (um^2)
    double combinational_area;  // Combinational Area
    double noncombinational_area;  // Noncombinational Area
    int leaf_cell_count;        // Leaf Cell Count
    bool valid;                 // 是否成功解析
    
    QoRReport() : cell_area(0.0), combinational_area(0.0), 
                  noncombinational_area(0.0), leaf_cell_count(0), valid(false) {}
};

// ============================================================================
// Innovus TCL Generator
// 根據 SRAM 寬度和 QoR report 產生 Innovus P&R 的 TCL script
// ============================================================================
class InnovusTclGenerator {
public:
    InnovusTclGenerator();
    ~InnovusTclGenerator();
    
    // 設定參數
    void set_design_name(const std::string& name);
    void set_site_name(const std::string& name);  // 預設: asap7sc7p5t
    void set_site_height(double height);  // 預設: 0.27 (um)
    void set_cpu_count(int local_cpu, int remote_cpu = 0);
    
    // 設定技術檔案路徑
    void set_layer_map_file(const std::string& path);
    void set_gds_merge_file(const std::string& path);
    
    // 解析 QoR report
    bool parse_qor_report(const std::string& qor_file_path);
    
    // 取得解析的 QoR report 資料
    const QoRReport& get_qor_report() const { return qor_report_; }
    
    // 計算 floorplan 尺寸
    // width: SRAM 寬度 (um)
    // 回傳值: 計算出的高度 (um)，已對齊到 site height 的倍數
    double calculate_floorplan_height(double width) const;
    
    // 產生 floorplan 命令
    // width: floorplan 寬度 (um)
    // height: floorplan 高度 (um)，如果為 0 則自動計算
    std::string generate_floorplan_command(double& width, double& height) const;
    
    // 產生完整的 run.tcl（目前僅包含 floorplan）
    // width: floorplan 寬度 (um)
    // height: floorplan 高度 (um)，如果為 0 則自動計算
    // output_file: 輸出檔案路徑
    // num_wlt: WLT pin 數量
    // num_wlb: WLB pin 數量
    // num_ysel: ysel pin 數量（每組，yselt/yselb）
    // addr_width: address pin 數量
    bool generate_run_tcl(double width, 
                          double height, 
                          const std::string& output_file,
                          int num_wlt,
                          int num_wlb,
                          int num_ysel,
                          int addr_width,
                          int num_mux) const;
    
    // 產生 global net connect 命令
    std::string generate_global_net_commands() const;
    
    // 產生 well tap 命令
    // interval: tap cell 間距 (um)
    std::string generate_well_tap_command(double interval) const;
    
    // 產生 createPhysicalPin 命令
    // pins: Pin 資訊列表
    // layer: Metal layer 編號（例如：3 表示 M3）
    // pin_width: Pin 寬度 (um)
    // core_height: Core 高度 (um)
    std::string generate_physical_pin_commands(const std::vector<PinInfo>& pins,
                                               int layer,
                                               double pin_width,
                                               double core_height) const;
    
    // 執行 Innovus
    // tcl_file: TCL script 檔案路徑
    // work_dir: 工作目錄（執行 Innovus 的目錄）
    // log_file: log 檔案路徑（選填，預設為 innovus.log）
    // 回傳值: true 表示成功，false 表示失敗
    bool run_innovus(const std::string& tcl_file,
                    const std::string& work_dir,
                    const std::string& log_file = "innovus.log") const;
    
    // 執行 v2lvs 將 Verilog netlist 轉換為 SPICE
    // work_dir: 工作目錄
    // verilog_file: 輸入的 Verilog netlist 檔案名稱
    // spice_file: 輸出的 SPICE netlist 檔案名稱
    // cdl_file: CDL 參考檔案路徑
    // 回傳值: true 表示成功，false 表示失敗
    bool run_v2lvs(const std::string& work_dir,
                   const std::string& verilog_file = "netlist_for_lvs.v",
                   const std::string& spice_file = "netlist_for_lvs.sp",
                   const std::string& cdl_file = "/home/s1111534/asap7/asap7sc7p5t_28/CDL/LVS/asap7sc7p5t_28_R.cdl") const;
    
    // 後處理 SPICE netlist（類似 spice_converter.cpp 中的 post_process_netlist）
    // work_dir: 工作目錄
    // spice_file: 要處理的 SPICE netlist 檔案名稱
    // cdl_file: CDL 參考檔案路徑
    // 回傳值: true 表示成功，false 表示失敗
    bool post_process_netlist(const std::string& work_dir,
                              const std::string& spice_file = "netlist_for_lvs.sp",
                              const std::string& cdl_file = "/home/s1111534/asap7/asap7sc7p5t_28/CDL/LVS/asap7sc7p5t_28_R.cdl");

private:
    std::string design_name_;
    std::string site_name_;
    double site_height_;
    int local_cpu_;
    int remote_cpu_;
    std::string layer_map_file_;
    std::string gds_merge_file_;
    QoRReport qor_report_;
    
    // Subcircuit dictionary for netlist post-processing
    std::map<std::string, std::vector<std::string>> subckt_dict_;
    
    // 對齊到 site height 的倍數（向上取整）
    double align_to_site_height(double height) const;
    
    // Netlist post-processing helper methods
    bool file_exists(const std::string& path) const;
    std::vector<std::string> read_file(const std::string& filepath) const;
    bool write_file(const std::string& filepath, const std::vector<std::string>& lines) const;
    std::vector<std::string> merge_continuation_lines(const std::vector<std::string>& lines) const;
    std::vector<std::string> add_power_to_subckt(const std::vector<std::string>& lines) const;
    bool parse_cdl_file(const std::string& cdl_path);
    void parse_subckt_from_lines(const std::vector<std::string>& lines);
    std::vector<std::string> expand_pins(const std::vector<std::string>& lines);
    std::vector<std::string> process_connect_directives(const std::vector<std::string>& lines) const;
};

} // namespace OpenFinRAM

#endif // OPENFINRAM_INNOVUS_TCL_GENERATOR_HPP_
