#include "spice_simulator.hpp"
#include "plog/Log.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <random>
#include <set>

namespace OpenFinRAM {

SpiceSimulator::SpiceSimulator(const SpiceSimConfig& config)
    : config_(config) 
{
    if (config_.output_dir.empty()) {
        config_.output_dir = ".";
    }
}

std::string SpiceSimulator::get_sram_cell_name() const {
    std::ostringstream oss;
    oss << "sram_x" << config_.num_wordlines << "x" << config_.data_bits;
    return oss.str();
}

std::string SpiceSimulator::generate_header() const {
    std::ostringstream oss;
    
    oss << "* BIST Testbench for SRAM\n";
    oss << "* Auto-generated BIST pattern for memory verification\n";
    oss << "* Address bits: " << config_.addr_bits 
        << ", Data bits: " << config_.data_bits << "\n";
    oss << "* Total addresses: " << (1UL << config_.addr_bits) << "\n\n";
    
    oss << "* ===================================================================\n";
    oss << "* Model and Netlist Includes\n";
    oss << "* ===================================================================\n\n";
    oss << ".INCLUDE \"" << config_.pdk_model << "\"\n";
    oss << ".INCLUDE \"" << config_.sram_netlist << "\"\n\n";
    
    return oss.str();
}

std::string SpiceSimulator::generate_power_supplies() const {
    std::ostringstream oss;
    
    oss << "* ===================================================================\n";
    oss << "* Global Nets and Power\n";
    oss << "* ===================================================================\n";
    oss << ".GLOBAL vdd vss\n\n";
    oss << "VVDD vdd 0 DC " << config_.v_dd << "\n";
    oss << "VVSS vss 0 DC 0\n\n";
    
    return oss.str();
}

std::string SpiceSimulator::generate_clock_reset(double t_start, double t_end) const {
    std::ostringstream oss;
    
    oss << "* ===================================================================\n";
    oss << "* Clock and Reset Generation\n";
    oss << "* ===================================================================\n";
    
    // Clock: PULSE(V1 V2 TD TR TF PW PER)
    // V1=0, V2=VDD, TD=t_start, TR=0.01n, TF=0.01n, PW=t_clk/2, PER=t_clk
    double clk_td = t_start - 0.01;
    double clk_tr = 0.01;
    double clk_tf = 0.01;
    double clk_pw = config_.t_clk / 2.0 - 0.01;
    double clk_per = config_.t_clk;
    
    oss << "Vclk clk vss PULSE(0 " << config_.v_dd 
        << " " << clk_td << "n " << clk_tr << "n " << clk_tf << "n "
        << clk_pw << "n " << clk_per << "n)\n";
    
    // Reset: active low, released at t_rst
    oss << "Vrst_n rst_n vss PWL(0n " << config_.v_dd 
        << ", " << (config_.t_rst - 0.01) << "n " << config_.v_dd
        << ", " << config_.t_rst << "n 0"
        << ", " << (config_.t_rst + config_.t_hold) << "n 0"
        << ", " << (config_.t_rst + config_.t_hold + 0.01) << "n " << config_.v_dd
        << ", " << t_end << "n " << config_.v_dd << ")\n\n";
    
    return oss.str();
}

std::string SpiceSimulator::generate_control_signals(
    const std::vector<double>& write_times,
    const std::vector<double>& read_times,
    double t_end) const 
{
    std::ostringstream oss;
    
    oss << "* ===================================================================\n";
    oss << "* Control Signals (CE_N, WE_N, OE_N)\n";
    oss << "* CE_N: Chip Enable (always active low = 0V)\n";
    oss << "* WE_N: Write Enable (0V=Write, VDD=Read)\n";
    oss << "* OE_N: Output Enable (0V=Read Enable, VDD=Output Disabled)\n";
    oss << "* Write: WE_N=0 | Read: WE_N=1\n";
    oss << "* ===================================================================\n";
    
    // CE_N is always active (0V) - chip is always enabled
    oss << "Vce_n ce_n vss DC 0\n";
    
    // Build combined operation timeline for WE_N
    std::vector<std::pair<double, bool>> operations;  // time, is_write
    
    for (double t : write_times) {
        operations.push_back({t, true});
    }
    for (double t : read_times) {
        operations.push_back({t, false});
    }
    
    // Sort operations by time
    std::sort(operations.begin(), operations.end());
    
    // Generate WE_N signal (0V for write, VDD for read)
    oss << "Vwe_n we_n vss PWL(0n " << config_.v_dd;
    if (!operations.empty()) {
        for (size_t i = 0; i < operations.size(); ++i) {
            double t = operations[i].first;
            bool is_write = operations[i].second;
            double active_voltage = is_write ? 0.0 : config_.v_dd;  // 0 for write, VDD for read
            
            // Previous state
            if (i > 0) {
                double prev_voltage = operations[i-1].second ? 0.0 : config_.v_dd;
                oss << ", " << (t - config_.t_setup - 0.01) << "n " << prev_voltage;
            } else {
                oss << ", " << (t - config_.t_setup - 0.01) << "n " << config_.v_dd;
            }
            
            // Transition to active state
            oss << ", " << (t - config_.t_setup) << "n " << active_voltage;
            
            // Hold active state
            oss << ", " << (t + config_.t_hold) << "n " << active_voltage;
        }
        
        // Return to idle (read state)
        double last_voltage = operations.back().second ? 0.0 : config_.v_dd;
        oss << ", " << (operations.back().first + config_.t_hold + 0.01) << "n " << config_.v_dd;
    }
    oss << ", " << t_end << "n " << config_.v_dd << ")\n\n";

    // Generate OE_N signal (VDD for write, 0V for read) = inverse of WE_N
    oss << "Voe_n oe_n vss PWL(0n 0";
    if (!operations.empty()) {
        for (size_t i = 0; i < operations.size(); ++i) {
            double t = operations[i].first;
            bool is_write = operations[i].second;
            double active_voltage = is_write ? config_.v_dd : 0.0;

            // Previous state
            if (i > 0) {
                double prev_voltage = operations[i-1].second ? config_.v_dd : 0.0;
                oss << ", " << (t - config_.t_setup - 0.01) << "n " << prev_voltage;
            } else {
                oss << ", " << (t - config_.t_setup - 0.01) << "n 0";
            }

            // Transition to active state
            oss << ", " << (t - config_.t_setup) << "n " << active_voltage;

            // Hold active state
            oss << ", " << (t + config_.t_hold) << "n " << active_voltage;
        }

        // Return to idle (read state)
        oss << ", " << (operations.back().first + config_.t_hold + 0.01) << "n 0";
    }
    oss << ", " << t_end << "n 0)\n\n";
    
    return oss.str();
}

std::string SpiceSimulator::generate_address_signals(
    const std::vector<uint64_t>& addresses,
    const std::vector<double>& times,
    double t_end) const 
{
    std::ostringstream oss;
    
    oss << "* ===================================================================\n";
    oss << "* Address Signals\n";
    oss << "* ===================================================================\n";
    
    for (uint64_t bit = 0; bit < config_.addr_bits; ++bit) {
        oss << "VA" << bit << " A[" << bit << "] vss PWL(0n 0, ";
        
        for (size_t i = 0; i < addresses.size(); ++i) {
            uint64_t A = addresses[i];
            double t = times[i];
            bool bit_val = (A >> bit) & 1;
            double voltage = bit_val ? config_.v_dd : 0.0;
            
            oss << "'" << t - config_.t_setup - 0.01 << "n' ";
            if (i > 0) {
                bool prev_bit = (addresses[i-1] >> bit) & 1;
                oss << (prev_bit ? config_.v_dd : 0.0);
            } else {
                oss << "0";
            }
            oss << ", '" << t - config_.t_setup << "n' " << voltage << ", ";
        }
        
        oss << t_end << "n ";
        bool last_bit = (addresses.back() >> bit) & 1;
        oss << (last_bit ? config_.v_dd : 0.0) << ")\n";
    }
    
    oss << "\n";
    return oss.str();
}

std::string SpiceSimulator::generate_data_signals(
    const std::vector<uint64_t>& write_data,
    const std::vector<double>& write_times,
    double t_end) const 
{
    std::ostringstream oss;
    
    oss << "* ===================================================================\n";
    oss << "* Data Input Signals\n";
    oss << "* ===================================================================\n";
    
    for (uint64_t bit = 0; bit < config_.data_bits; ++bit) {
        oss << "VD" << bit << " D[" << bit << "] vss PWL(0n 0, ";
        
        for (size_t i = 0; i < write_data.size(); ++i) {
            uint64_t data = write_data[i];
            double t = write_times[i];
            bool bit_val = (data >> bit) & 1;
            double voltage = bit_val ? config_.v_dd : 0.0;
            
            oss << "'" << t - config_.t_setup - 0.01 << "n' ";
            if (i > 0) {
                bool prev_bit = (write_data[i-1] >> bit) & 1;
                oss << (prev_bit ? config_.v_dd : 0.0);
            } else {
                oss << "0";
            }
            oss << ", '" << t - config_.t_setup << "n' " << voltage << ", ";
        }
        
        oss << t_end << "n ";
        bool last_bit = (write_data.back() >> bit) & 1;
        oss << (last_bit ? config_.v_dd : 0.0) << ")\n";
    }
    
    oss << "\n";
    return oss.str();
}

std::string SpiceSimulator::generate_sram_instance() const {
    std::ostringstream oss;
    
    oss << "* ===================================================================\n";
    oss << "* SRAM Instance\n";
    oss << "* ===================================================================\n";
    oss << "X_sram vdd vss clk rst_n ce_n we_n oe_n ";
    
    // Address ports
    for (uint64_t i = 0; i < config_.addr_bits; ++i) {
        oss << "A[" << i << "] ";
    }
    
    // Data input ports
    for (uint64_t i = 0; i < config_.data_bits; ++i) {
        oss << "D[" << i << "] ";
    }
    
    // Data output ports
    for (uint64_t i = 0; i < config_.data_bits; ++i) {
        oss << "Q[" << i << "] ";
    }
    
    oss << get_sram_cell_name() << "\n\n";
    
    return oss.str();
}

std::string SpiceSimulator::generate_measurements(
    const std::vector<uint64_t>& addresses,
    const std::vector<double>& read0_times,
    const std::vector<double>& read1_times) const 
{
    std::ostringstream oss;
    
    oss << "* ===================================================================\n";
    oss << "* BIST Verification Measurements\n";
    oss << "* ===================================================================\n\n";
    
    // Read 0 verification
    oss << "* --- Phase 2: Verify Read 0 (expect all Q bits = 0V) ---\n";
    for (size_t i = 0; i < read0_times.size(); ++i) {
        double t_meas = read0_times[i] + config_.t_clk * 0.8;
        for (uint64_t bit = 0; bit < config_.data_bits; ++bit) {
            oss << ".MEAS TRAN read0_addr" << addresses[i] << "_Q" << bit 
                << " FIND V(Q[" << bit << "]) AT='" << t_meas << "n'\n";
        }
    }
    oss << "\n";
    
    // Read 1 verification
    oss << "* --- Phase 4: Verify Read 1 (expect all Q bits = " << config_.v_dd << "V) ---\n";
    for (size_t i = 0; i < read1_times.size(); ++i) {
        double t_meas = read1_times[i] + config_.t_clk * 0.8;
        for (uint64_t bit = 0; bit < config_.data_bits; ++bit) {
            oss << ".MEAS TRAN read1_addr" << addresses[i] << "_Q" << bit 
                << " FIND V(Q[" << bit << "]) AT='" << t_meas << "n'\n";
        }
    }
    oss << "\n";
    
    return oss.str();
}

std::string SpiceSimulator::generate_sim_control(double t_end) const {
    std::ostringstream oss;
    
    oss << "* ===================================================================\n";
    oss << "* Simulation Control\n";
    oss << "* ===================================================================\n";
    oss << ".TRAN 0.01n " << t_end << "n\n";
    oss << ".OPTIONS POST=1\n";
    oss << ".OPTIONS finesim_output=fsdb\n\n";
    oss << ".END\n";
    
    return oss.str();
}

double SpiceSimulator::calculate_timeline(
    const std::vector<uint64_t>& addresses,
    std::vector<double>& write0_times,
    std::vector<double>& read0_times,
    std::vector<double>& write1_times,
    std::vector<double>& read1_times) const 
{
    write0_times.clear();
    read0_times.clear();
    write1_times.clear();
    read1_times.clear();
    
    double t_start = 2.0;
    double t_current = t_start;
    
    // Phase 1: Write 0 to all test addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        write0_times.push_back(t_current);
        t_current += config_.t_clk;  // One cycle per write
    }
    
    // Phase 2: Read and verify 0
    for (size_t i = 0; i < addresses.size(); ++i) {
        read0_times.push_back(t_current);
        t_current += config_.t_clk;  // One cycle per read
    }
    
    // Phase 3: Write 1 to all test addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        write1_times.push_back(t_current);
        t_current += config_.t_clk;  // One cycle per write
    }
    
    // Phase 4: Read and verify 1
    for (size_t i = 0; i < addresses.size(); ++i) {
        read1_times.push_back(t_current);
        t_current += config_.t_clk;  // One cycle per read
    }
    
    return t_current + 2.0;  // Add some margin
}

bool SpiceSimulator::generate_bist_testbench(const std::string& output_file) {
    LOGI << "========================================";
    LOGI << "Generating BIST Testbench (Full)";
    LOGI << "========================================";
    
    uint64_t num_addresses = 1UL << config_.addr_bits;
    
    LOGI << "Configuration:";
    LOGI << "  Address bits: " << config_.addr_bits;
    LOGI << "  Data bits: " << config_.data_bits;
    LOGI << "  Total addresses: " << num_addresses;
    LOGI << "  SRAM cell: " << get_sram_cell_name();
    
    // Generate address sequence (all addresses)
    std::vector<uint64_t> addresses;
    for (uint64_t i = 0; i < num_addresses; ++i) {
        addresses.push_back(i);
    }
    
    // Calculate timeline
    std::vector<double> write0_times, read0_times, write1_times, read1_times;
    double t_end = calculate_timeline(addresses, write0_times, read0_times, 
                                       write1_times, read1_times);
    
    LOGI << "  Simulation time: " << t_end << "ns";
    
    // Build chronologically ordered operation sequences
    // Phase 1: Write 0, Phase 2: Read 0, Phase 3: Write 1, Phase 4: Read 1
    std::vector<double> all_write_times;
    std::vector<uint64_t> write_data;
    std::vector<double> all_read_times;
    std::vector<double> all_op_times;
    std::vector<uint64_t> all_addresses;
    
    uint64_t all_ones = (config_.data_bits >= 64) ? ~0ULL
                                                 : ((1ULL << config_.data_bits) - 1ULL);
    
    // Phase 1: Write 0 to all addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_write_times.push_back(write0_times[i]);
        write_data.push_back(0);
        all_op_times.push_back(write0_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 2: Read 0 from all addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_read_times.push_back(read0_times[i]);
        all_op_times.push_back(read0_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 3: Write 1 to all addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_write_times.push_back(write1_times[i]);
        write_data.push_back(all_ones);
        all_op_times.push_back(write1_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 4: Read 1 from all addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_read_times.push_back(read1_times[i]);
        all_op_times.push_back(read1_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Generate testbench
    std::ofstream file(output_file);
    if (!file.is_open()) {
        LOGE << "Failed to create testbench file: " << output_file;
        return false;
    }
    
    file << generate_header();
    file << generate_power_supplies();
    file << "* ===================================================================\n";
    file << "* Timing Parameters\n";
    file << "* ===================================================================\n";
    file << ".param T_CLK = " << config_.t_clk << "n\n";
    file << ".param T_SETUP = " << config_.t_setup << "n\n";
    file << ".param T_HOLD = " << config_.t_hold << "n\n\n";
    file << generate_clock_reset(2.0, t_end);
    file << generate_control_signals(all_write_times, all_read_times, t_end);
    file << generate_address_signals(all_addresses, all_op_times, t_end);
    file << generate_data_signals(write_data, all_write_times, t_end);
    file << generate_sram_instance();
    file << generate_measurements(addresses, read0_times, read1_times);
    file << generate_sim_control(t_end);
    
    file.close();
    
    LOGI << "✓ Generated testbench: " << output_file;
    return true;
}

bool SpiceSimulator::generate_single_bit_testbench(const std::string& output_file, uint64_t bit_idx) {
    LOGI << "========================================";
    LOGI << "Generating Single-Bit BIST Testbench";
    LOGI << "========================================";
    
    if (bit_idx >= config_.addr_bits) {
        LOGE << "Invalid bit index: " << bit_idx << " (max: " << (config_.addr_bits - 1) << ")";
        return false;
    }
    
    LOGI << "Configuration:";
    LOGI << "  Address bits: " << config_.addr_bits;
    LOGI << "  Data bits: " << config_.data_bits;
    LOGI << "  Testing address bit A[" << bit_idx << "]";
    LOGI << "  SRAM cell: " << get_sram_cell_name();
    
    // Test only 2 addresses: 0 and (1 << bit_idx)
    std::vector<uint64_t> addresses;
    addresses.push_back(0);
    addresses.push_back(1UL << bit_idx);
    
    LOGI << "  Test addresses: 0 and " << (1UL << bit_idx);
    
    // Calculate timeline for 2 addresses
    std::vector<double> write0_times, read0_times, write1_times, read1_times;
    double t_end = calculate_timeline(addresses, write0_times, read0_times, 
                                       write1_times, read1_times);
    
    LOGI << "  Simulation time: " << t_end << "ns";
    
    // Build operation sequences
    std::vector<double> all_write_times;
    std::vector<uint64_t> write_data;
    std::vector<double> all_read_times;
    std::vector<double> all_op_times;
    std::vector<uint64_t> all_addresses;
    
    uint64_t all_ones = (config_.data_bits >= 64) ? ~0ULL
                                                 : ((1ULL << config_.data_bits) - 1ULL);
    
    // Phase 1: Write 0
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_write_times.push_back(write0_times[i]);
        write_data.push_back(0);
        all_op_times.push_back(write0_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 2: Read 0
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_read_times.push_back(read0_times[i]);
        all_op_times.push_back(read0_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 3: Write 1
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_write_times.push_back(write1_times[i]);
        write_data.push_back(all_ones);
        all_op_times.push_back(write1_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 4: Read 1
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_read_times.push_back(read1_times[i]);
        all_op_times.push_back(read1_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Generate testbench file
    std::ofstream file(output_file);
    if (!file.is_open()) {
        LOGE << "Failed to create testbench file: " << output_file;
        return false;
    }
    
    file << "* BIST Testbench for SRAM - Bit " << bit_idx << " Test\n";
    file << "* Tests address bit A[" << bit_idx << "] toggling\n";
    file << "* Test addresses: 0 and " << (1UL << bit_idx) << "\n\n";
    
    file << generate_header();
    file << generate_power_supplies();
    file << "* ===================================================================\n";
    file << "* Timing Parameters\n";
    file << "* ===================================================================\n";
    file << ".param T_CLK = " << config_.t_clk << "n\n";
    file << ".param T_SETUP = " << config_.t_setup << "n\n";
    file << ".param T_HOLD = " << config_.t_hold << "n\n\n";
    file << generate_clock_reset(2.0, t_end);
    file << generate_control_signals(all_write_times, all_read_times, t_end);
    file << generate_address_signals(all_addresses, all_op_times, t_end);
    file << generate_data_signals(write_data, all_write_times, t_end);
    file << generate_sram_instance();
    file << generate_measurements(addresses, read0_times, read1_times);
    file << generate_sim_control(t_end);
    
    file.close();
    
    LOGI << "✓ Generated single-bit testbench: " << output_file;
    return true;
}

std::vector<std::string> SpiceSimulator::generate_parallel_testbenches(const std::string& output_dir) {
    LOGI << "========================================";
    LOGI << "Generating Parallel BIST Testbenches";
    LOGI << "========================================";
    
    std::string out_dir = output_dir.empty() ? config_.output_dir : output_dir;
    std::vector<std::string> testbench_files;
    
    LOGI << "Configuration:";
    LOGI << "  Address bits: " << config_.addr_bits;
    LOGI << "  Data bits: " << config_.data_bits;
    LOGI << "  Generating " << config_.addr_bits << " testbenches (one per address bit)";
    
    for (uint64_t bit = 0; bit < config_.addr_bits; ++bit) {
        std::ostringstream filename;
        filename << out_dir << "/sram_bist_tb_bit" << bit << ".sp";
        
        if (generate_single_bit_testbench(filename.str(), bit)) {
            testbench_files.push_back(filename.str());
            LOGI << "  [" << (bit + 1) << "/" << config_.addr_bits << "] Generated bit" << bit << " testbench";
        } else {
            LOGE << "  Failed to generate testbench for bit " << bit;
        }
    }
    
    LOGI << "✓ Generated " << testbench_files.size() << " parallel testbenches";
    return testbench_files;
}

bool SpiceSimulator::generate_random_testbench(const std::string& output_file, double test_percentage, uint64_t seed) {
    LOGI << "========================================";
    LOGI << "Generating Random BIST Testbench";
    LOGI << "========================================";
    
    // 驗證參數
    if (test_percentage <= 0.0 || test_percentage > 100.0) {
        LOGE << "Invalid test percentage: " << test_percentage << " (must be 0-100)";
        return false;
    }
    
    uint64_t num_addresses = 1UL << config_.addr_bits;
    uint64_t num_test_addr = static_cast<uint64_t>((num_addresses * test_percentage) / 100.0);
    
    // 至少測試 1 個地址
    if (num_test_addr == 0) {
        num_test_addr = 1;
    }
    
    // 不要超過總地址數
    if (num_test_addr > num_addresses) {
        num_test_addr = num_addresses;
    }
    
    LOGI << "Configuration:";
    LOGI << "  Address bits: " << config_.addr_bits;
    LOGI << "  Data bits: " << config_.data_bits;
    LOGI << "  Total addresses: " << num_addresses;
    LOGI << "  Test percentage: " << test_percentage << "%";
    LOGI << "  Addresses to test: " << num_test_addr;
    LOGI << "  SRAM cell: " << get_sram_cell_name();
    
    // 設定隨機數生成器
    std::mt19937_64 rng;
    if (seed == 0) {
        // 使用隨機種子
        std::random_device rd;
        seed = rd();
        rng.seed(seed);
        LOGI << "  Random seed: " << seed << " (auto-generated)";
    } else {
        rng.seed(seed);
        LOGI << "  Random seed: " << seed << " (user-specified)";
    }
    
    // 生成唯一的隨機地址列表
    std::set<uint64_t> address_set;
    std::uniform_int_distribution<uint64_t> dist(0, num_addresses - 1);
    
    LOGI << "Generating random addresses...";
    while (address_set.size() < num_test_addr) {
        uint64_t A = dist(rng);
        address_set.insert(A);
    }
    
    // 轉換為 vector 並排序（方便除錯和結果分析）
    std::vector<uint64_t> addresses(address_set.begin(), address_set.end());
    std::sort(addresses.begin(), addresses.end());
    
    // 顯示前幾個和後幾個地址作為樣本
    LOGI << "Sample addresses (first 5):";
    for (size_t i = 0; i < std::min(size_t(5), addresses.size()); ++i) {
        LOGI << "  [" << i << "] = " << addresses[i];
    }
    if (addresses.size() > 10) {
        LOGI << "  ...";
        LOGI << "Sample addresses (last 5):";
        for (size_t i = addresses.size() - 5; i < addresses.size(); ++i) {
            LOGI << "  [" << i << "] = " << addresses[i];
        }
    }
    
    // 計算時間軸
    std::vector<double> write0_times, read0_times, write1_times, read1_times;
    double t_end = calculate_timeline(addresses, write0_times, read0_times, 
                                       write1_times, read1_times);
    
    LOGI << "  Simulation time: " << t_end << "ns";
    
    // 建立操作序列
    std::vector<double> all_write_times;
    std::vector<uint64_t> write_data;
    std::vector<double> all_read_times;
    std::vector<double> all_op_times;
    std::vector<uint64_t> all_addresses;
    
    uint64_t all_ones = (config_.data_bits >= 64) ? ~0ULL
                                                 : ((1ULL << config_.data_bits) - 1ULL);
    
    // Phase 1: Write 0
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_write_times.push_back(write0_times[i]);
        write_data.push_back(0);
        all_op_times.push_back(write0_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 2: Read 0
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_read_times.push_back(read0_times[i]);
        all_op_times.push_back(read0_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 3: Write 1
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_write_times.push_back(write1_times[i]);
        write_data.push_back(all_ones);
        all_op_times.push_back(write1_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 4: Read 1
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_read_times.push_back(read1_times[i]);
        all_op_times.push_back(read1_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // 生成 testbench 文件
    std::ofstream file(output_file);
    if (!file.is_open()) {
        LOGE << "Failed to create testbench file: " << output_file;
        return false;
    }
    
    file << "* Random BIST Testbench for SRAM\n";
    file << "* Tests " << test_percentage << "% of memory locations\n";
    file << "* Random seed: " << seed << "\n";
    file << "* Testing " << num_test_addr << " out of " << num_addresses << " addresses\n\n";
    
    file << generate_header();
    file << generate_power_supplies();
    file << "* ===================================================================\n";
    file << "* Timing Parameters\n";
    file << "* ===================================================================\n";
    file << ".param T_CLK = " << config_.t_clk << "n\n";
    file << ".param T_SETUP = " << config_.t_setup << "n\n";
    file << ".param T_HOLD = " << config_.t_hold << "n\n\n";
    file << generate_clock_reset(2.0, t_end);
    file << generate_control_signals(all_write_times, all_read_times, t_end);
    file << generate_address_signals(all_addresses, all_op_times, t_end);
    file << generate_data_signals(write_data, all_write_times, t_end);
    file << generate_sram_instance();
    file << generate_measurements(addresses, read0_times, read1_times);
    file << generate_sim_control(t_end);
    
    file.close();
    
    LOGI << "✓ Generated random testbench: " << output_file;
    LOGI << "  Coverage: " << test_percentage << "% (" << num_test_addr 
         << " / " << num_addresses << " addresses)";
    LOGI << "  Random seed: " << seed << " (use this seed to reproduce results)";
    
    return true;
}

bool SpiceSimulator::generate_quick_testbench(const std::string& output_file, int num_test_addr) {
    LOGI << "========================================";
    LOGI << "Generating Quick BIST Testbench";
    LOGI << "========================================";
    
    uint64_t num_addresses = 1UL << config_.addr_bits;
    
    LOGI << "Configuration:";
    LOGI << "  Address bits: " << config_.addr_bits;
    LOGI << "  Data bits: " << config_.data_bits;
    LOGI << "  Testing " << num_test_addr << " addresses (out of " << num_addresses << ")";
    LOGI << "  SRAM cell: " << get_sram_cell_name();
    
    // Generate test address sequence (corner cases + a few random)
    std::vector<uint64_t> addresses;
    addresses.push_back(0);  // First address
    addresses.push_back(num_addresses - 1);  // Last address
    
    // Add a few more addresses if requested
    for (int i = 2; i < num_test_addr && i < (int)num_addresses; ++i) {
        addresses.push_back(i * (num_addresses / num_test_addr));
    }
    
    // Rest of the generation is similar to full testbench
    std::vector<double> write0_times, read0_times, write1_times, read1_times;
    double t_end = calculate_timeline(addresses, write0_times, read0_times, 
                                       write1_times, read1_times);
    
    LOGI << "  Simulation time: " << t_end << "ns";
    
    // Build chronologically ordered operation sequences
    // Phase 1: Write 0, Phase 2: Read 0, Phase 3: Write 1, Phase 4: Read 1
    std::vector<double> all_write_times;
    std::vector<uint64_t> write_data;
    std::vector<double> all_read_times;
    std::vector<double> all_op_times;
    std::vector<uint64_t> all_addresses;
    
    uint64_t all_ones = (config_.data_bits >= 64) ? ~0ULL
                                                 : ((1ULL << config_.data_bits) - 1ULL);
    
    // Phase 1: Write 0 to all addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_write_times.push_back(write0_times[i]);
        write_data.push_back(0);
        all_op_times.push_back(write0_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 2: Read 0 from all addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_read_times.push_back(read0_times[i]);
        all_op_times.push_back(read0_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 3: Write 1 to all addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_write_times.push_back(write1_times[i]);
        write_data.push_back(all_ones);
        all_op_times.push_back(write1_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Phase 4: Read 1 from all addresses
    for (size_t i = 0; i < addresses.size(); ++i) {
        all_read_times.push_back(read1_times[i]);
        all_op_times.push_back(read1_times[i]);
        all_addresses.push_back(addresses[i]);
    }
    
    // Generate testbench file
    std::ofstream file(output_file);
    if (!file.is_open()) {
        LOGE << "Failed to create testbench file: " << output_file;
        return false;
    }
    
    file << generate_header();
    file << generate_power_supplies();
    file << "* ===================================================================\n";
    file << "* Timing Parameters\n";
    file << "* ===================================================================\n";
    file << ".param T_CLK = " << config_.t_clk << "n\n";
    file << ".param T_SETUP = " << config_.t_setup << "n\n";
    file << ".param T_HOLD = " << config_.t_hold << "n\n\n";
    file << generate_clock_reset(2.0, t_end);
    file << generate_control_signals(all_write_times, all_read_times, t_end);
    file << generate_address_signals(all_addresses, all_op_times, t_end);
    file << generate_data_signals(write_data, all_write_times, t_end);
    file << generate_sram_instance();
    file << generate_measurements(addresses, read0_times, read1_times);
    file << generate_sim_control(t_end);
    
    file.close();
    
    LOGI << "✓ Generated quick testbench: " << output_file;
    return true;
}

bool SpiceSimulator::run_simulation(const std::string& testbench_file, int num_threads) {
    LOGI << "========================================";
    LOGI << "Running SPICE Simulation";
    LOGI << "========================================";
    
    LOGI << "Testbench: " << testbench_file;
    LOGI << "Threads: " << num_threads;
    
    // Extract base name and directory
    size_t last_slash = testbench_file.find_last_of("/\\");
    std::string dir = (last_slash != std::string::npos) ? 
                      testbench_file.substr(0, last_slash) : ".";
    std::string base_name = (last_slash != std::string::npos) ?
                            testbench_file.substr(last_slash + 1) : testbench_file;
    
    // Remove .sp extension for output name
    if (base_name.size() > 3 && base_name.substr(base_name.size() - 3) == ".sp") {
        base_name = base_name.substr(0, base_name.size() - 3);
    }
    
    std::string output_base = dir + "/" + base_name;
    
    // Build finesim command
    std::ostringstream cmd;
    cmd << "tcsh -c 'cd " << dir << " && finesim -np " << num_threads 
        << " -w " << testbench_file << " -o " << output_base << "'";
    
    LOGI << "Command: " << cmd.str();
    
    int result = system(cmd.str().c_str());
    
    if (result != 0) {
        LOGE << "Simulation failed with exit code: " << result;
        return false;
    }
    
    LOGI << "✓ Simulation completed successfully";
    return true;
}

std::map<std::string, double> SpiceSimulator::parse_measurements(const std::string& mt0_file) {
    std::map<std::string, double> measurements;
    
    LOGI << "Parsing measurements from: " << mt0_file;
    
    std::ifstream file(mt0_file);
    if (!file.is_open()) {
        LOGE << "Failed to open measurement file: " << mt0_file;
        return measurements;
    }
    
    std::vector<std::string> names;
    std::vector<double> values;
    
    std::string line;
    int line_num = 0;
    
    while (std::getline(file, line)) {
        line_num++;
        
        // Skip empty lines
        if (line.empty()) {
            continue;
        }
        
        // Skip metadata lines (starting with $, ., or *)
        if (line[0] == '$' || line[0] == '.' || line[0] == '*') {
            continue;
        }
        
        // First data line should be measurement names
        if (names.empty()) {
            std::istringstream iss(line);
            std::string name;
            while (iss >> name) {
                // Skip non-measurement columns (like temper, alter#)
                if (name != "temper" && name != "alter#") {
                    names.push_back(name);
                } else {
                    names.push_back("");  // placeholder for columns to skip
                }
            }
            LOGI << "Found " << names.size() << " measurement columns";
            continue;
        }
        
        // Second data line should be values
        std::istringstream iss(line);
        double value;
        int col = 0;
        while (iss >> value) {
            if (col < (int)names.size() && !names[col].empty()) {
                measurements[names[col]] = value;
            }
            col++;
        }
        
        // Usually only one data row, but continue reading if there are more
    }
    
    file.close();
    
    LOGI << "Parsed " << measurements.size() << " measurements";
    
    // Log a few samples for verification
    int count = 0;
    for (const auto& entry : measurements) {
        if (count++ < 5) {
            LOGI << "  " << entry.first << " = " << entry.second;
        }
    }
    if (measurements.size() > 5) {
        LOGI << "  ... and " << (measurements.size() - 5) << " more";
    }
    
    return measurements;
}

bool SpiceSimulator::verify_results(const std::map<std::string, double>& measurements) {
    LOGI << "========================================";
    LOGI << "Verifying BIST Results";
    LOGI << "========================================";
    
    if (measurements.empty()) {
        LOGE << "No measurements to verify!";
        return false;
    }
    
    int read0_pass = 0, read0_fail = 0;
    int read1_pass = 0, read1_fail = 0;
    
    for (const auto& entry : measurements) {
        const std::string& name = entry.first;
        double value = entry.second;
        
        if (name.find("read0_") == 0) {
            // Should be close to 0V
            if (value <= config_.v_low_max) {
                read0_pass++;
            } else {
                read0_fail++;
                LOGW << "Read 0 FAIL: " << name << " = " << value 
                     << "V (expected < " << config_.v_low_max << "V)";
            }
        } else if (name.find("read1_") == 0) {
            // Should be close to VDD
            if (value >= config_.v_high_min) {
                read1_pass++;
            } else {
                read1_fail++;
                LOGW << "Read 1 FAIL: " << name << " = " << value 
                     << "V (expected > " << config_.v_high_min << "V)";
            }
        }
    }
    
    int total_read0 = read0_pass + read0_fail;
    int total_read1 = read1_pass + read1_fail;
    
    LOGI << "BIST Test Summary:";
    LOGI << "  Read 0 phase: " << read0_pass << "/" << total_read0 << " passed";
    if (read0_fail > 0) {
        LOGW << "    " << read0_fail << " tests FAILED";
    }
    LOGI << "  Read 1 phase: " << read1_pass << "/" << total_read1 << " passed";
    if (read1_fail > 0) {
        LOGW << "    " << read1_fail << " tests FAILED";
    }
    
    bool all_pass = (read0_fail == 0) && (read1_fail == 0);
    
    if (all_pass) {
        LOGI << "✓ ALL BIST TESTS PASSED!";
    } else {
        LOGE << "✗ BIST TESTS FAILED (" << (read0_fail + read1_fail) << " failures)";
    }
    
    return all_pass;
}

bool SpiceSimulator::run_bist_verification(bool quick_mode) {
    LOGI << "";
    LOGI << "========================================================================";
    LOGI << "SRAM BIST Verification Flow";
    LOGI << "========================================================================";
    
    // Generate testbench
    std::string testbench_file = config_.output_dir + "/sram_bist_tb";
    if (quick_mode) {
        testbench_file += "_quick.sp";
        if (!generate_quick_testbench(testbench_file, 4)) {
            LOGE << "Failed to generate quick testbench";
            return false;
        }
    } else {
        testbench_file += ".sp";
        if (!generate_bist_testbench(testbench_file)) {
            LOGE << "Failed to generate testbench";
            return false;
        }
    }
    
    // Run simulation
    if (!run_simulation(testbench_file, 8)) {
        LOGE << "Simulation failed";
        return false;
    }
    
    // Parse measurements
    std::string base_name = testbench_file;
    if (base_name.size() > 3 && base_name.substr(base_name.size() - 3) == ".sp") {
        base_name = base_name.substr(0, base_name.size() - 3);
    }
    std::string mt0_file = base_name + ".mt0";
    
    auto measurements = parse_measurements(mt0_file);
    
    // Verify results
    bool success = verify_results(measurements);
    
    LOGI << "";
    LOGI << "========================================================================";
    if (success) {
        LOGI << "✓ BIST Verification PASSED";
    } else {
        LOGI << "✗ BIST Verification FAILED";
    }
    LOGI << "========================================================================";
    
    return success;
}

} // namespace OpenFinRAM
