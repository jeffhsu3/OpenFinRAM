#pragma once

#include <string>

namespace OpenFinRAM {

/**
 * @brief PEX (Parasitic Extraction) Runner
 * 
 * 這個類別封裝了 Calibre PEX 的執行流程，包括：
 * 1. 生成 SVRF 控制文件和 TCL 腳本
 * 2. 執行 LVS (Layout vs Schematic)
 * 3. 執行 PDB (Parasitic Database) 抽取
 * 4. 執行 FMT (Formatter) 生成最終的 netlist
 */
class PEXRunner {
public:
    /**
     * @brief PEX 配置結構
     */
    struct Config {
        std::string gds_path;           // GDS layout 文件路徑
        std::string spice_path;         // SPICE netlist 文件路徑
        std::string cell_name;          // 要抽取的 cell 名稱
        std::string output_dir;         // 輸出目錄
        std::string pdk_rule_file;      // PDK RCX rule 文件路徑
        int turbo_count;                // Calibre 多線程數量
        
        Config() 
            : pdk_rule_file("/home/s1111534/asap7/asap7_pdk_r1p7/calibre/ruledirs/rcx/rcxControl_calibre_asap7_170801.rul")
            , turbo_count(8) 
        {}
    };
    
    /**
     * @brief 構造函數
     * @param config PEX 配置
     */
    explicit PEXRunner(const Config& config);
    
    /**
     * @brief 執行完整的 PEX 流程
     * @return 成功返回 true，失敗返回 false
     */
    bool run();
    
    /**
     * @brief 獲取輸出的 PEX netlist 路徑
     * @return PEX netlist 的完整路徑
     */
    std::string get_pex_netlist_path() const;
    
    /**
     * @brief 檢查 PEX 輸出是否存在
     * @return 存在返回 true，否則返回 false
     */
    bool check_output_exists() const;

private:
    /**
     * @brief 創建工作目錄結構
     * @return 成功返回 true，失敗返回 false
     */
    bool create_directories();
    
    /**
     * @brief 生成 SVRF 控制文件
     * @return 成功返回 true，失敗返回 false
     */
    bool generate_svrf_file();
    
    /**
     * @brief 生成 TCL 腳本
     * @return 成功返回 true，失敗返回 false
     */
    bool generate_tcl_script();
    
    /**
     * @brief 執行 LVS
     * @return 成功返回 true，失敗返回 false
     */
    bool execute_lvs();
    
    /**
     * @brief 執行 PDB 抽取
     * @return 成功返回 true，失敗返回 false
     */
    bool execute_pdb();
    
    /**
     * @brief 執行 FMT 生成 netlist
     * @return 成功返回 true，失敗返回 false
     */
    bool execute_fmt();
    
    /**
     * @brief 執行 shell 命令並檢查返回值
     * @param command 要執行的命令
     * @param log_file 日誌文件路徑
     * @return 成功返回 true，失敗返回 false
     */
    bool execute_command(const std::string& command, const std::string& log_file);
    
    Config config_;
    std::string run_dir_;        // run_{cell_name} 目錄
    std::string svdb_dir_;       // svdb 目錄
};

} // namespace OpenFinRAM
