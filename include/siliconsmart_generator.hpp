#ifndef OPENFINRAM_SILICONSMART_GENERATOR_HPP_
#define OPENFINRAM_SILICONSMART_GENERATOR_HPP_

#include <cstdint>
#include <string>

namespace OpenFinRAM {

// ============================================================================
// SiliconSmart 配置
// ============================================================================
struct SiliconSmartConfig {
    std::string cell_name;
    uint64_t addr_width = 0;
    uint64_t data_width = 0;
    std::string sis_dir = "./sis";           // 預設在當前目錄下建立 sis
    std::string flat_spice_path = "./sram_flat.sp";
    std::string configure_template_path = ""; // 可選：作為 configure.tcl 的模板
};

// ============================================================================
// SiliconSmart 檔案產生器
// ============================================================================
class SiliconSmartGenerator {
public:
    SiliconSmartGenerator() = default;
    ~SiliconSmartGenerator() = default;

    // 產生 sis 目錄與相關檔案
    bool generate(const SiliconSmartConfig& config) const;

    // 執行 SiliconSmart（tcsh -c 'cd sis && siliconsmart ./run.tcl'）
    bool run_siliconsmart(const SiliconSmartConfig& config) const;
};

} // namespace OpenFinRAM

#endif // OPENFINRAM_SILICONSMART_GENERATOR_HPP_
