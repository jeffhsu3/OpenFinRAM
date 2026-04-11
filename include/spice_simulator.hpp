#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace OpenFinRAM {

/**
 * @brief Configuration for SPICE simulation
 */
struct SpiceSimConfig {
    uint64_t addr_bits;         // Number of address bits
    uint64_t data_bits;         // Number of data bits
    uint64_t num_wordlines;     // Number of wordlines (top + bottom)
    
    std::string sram_netlist;   // Path to SRAM netlist (sram.sp)
    std::string output_dir;     // Output directory for testbenches
    std::string pdk_model;      // Path to PDK model file
    
    // Timing parameters (in nanoseconds)
    double t_clk;               // Clock period
    double t_setup;             // Setup time
    double t_hold;              // Hold time
    double t_rst;               // Reset release time
    
    // Voltage thresholds for verification
    double v_dd;                // Supply voltage
    double v_low_max;           // Maximum voltage for logic '0'
    double v_high_min;          // Minimum voltage for logic '1'
    
    SpiceSimConfig() 
        : addr_bits(8)
        , data_bits(4)
        , num_wordlines(32)
        , pdk_model("/home/s1111534/asap7/asap7_pdk_r1p7/models/hspice/7nm_TT.pm")
        , t_clk(1.0)
        , t_setup(0.2)
        , t_hold(0.5)
        , t_rst(0.5)
        , v_dd(0.7)
        , v_low_max(0.15)
        , v_high_min(0.55)
    {}
};

/**
 * @brief SPICE Simulator for SRAM verification
 * 
 * Generates BIST testbenches and runs SPICE simulations
 * to verify SRAM functionality.
 */
class SpiceSimulator {
public:
    /**
     * @brief Constructor
     * @param config Simulation configuration
     */
    explicit SpiceSimulator(const SpiceSimConfig& config);
    
    /**
     * @brief Generate BIST testbench for complete SRAM verification
     * Tests all addresses with Write 0, Read 0, Write 1, Read 1 pattern
     * @param output_file Output testbench file path
     * @return true if successful
     */
    bool generate_bist_testbench(const std::string& output_file);
    
    /**
     * @brief Generate simplified BIST testbench for quick verification
     * Only tests a few representative addresses
     * @param output_file Output testbench file path
     * @param num_test_addr Number of addresses to test (default: 4)
     * @return true if successful
     */
    bool generate_quick_testbench(const std::string& output_file, int num_test_addr = 4);
    
    /**
     * @brief Generate BIST testbench for a single address bit
     * Tests address 0 and address (1 << bit_idx) to verify one address bit toggle
     * Enables parallel testing by generating multiple small testbenches
     * @param output_file Output testbench file path
     * @param bit_idx Address bit index to test (0 to addr_bits-1)
     * @return true if successful
     */
    bool generate_single_bit_testbench(const std::string& output_file, uint64_t bit_idx);
    
    /**
     * @brief Generate multiple parallel BIST testbenches (one per address bit)
     * Each testbench tests only one address bit toggle for faster parallel simulation
     * @param output_dir Output directory for testbench files
     * @return Vector of generated testbench file paths
     */
    std::vector<std::string> generate_parallel_testbenches(const std::string& output_dir = "");
    
    /**
     * @brief Generate BIST testbench with random address selection
     * Tests a specified percentage of memory locations with randomly selected addresses
     * Useful for realistic usage patterns and faster verification
     * @param output_file Output testbench file path
     * @param test_percentage Percentage of addresses to test (0.0-100.0)
     * @param seed Random seed for reproducibility (0 = use random seed)
     * @return true if successful
     */
    bool generate_random_testbench(const std::string& output_file, double test_percentage, uint64_t seed = 0);
    
    /**
     * @brief Run SPICE simulation using FineSim
     * @param testbench_file Path to testbench file
     * @param num_threads Number of parallel threads for simulation
     * @return true if simulation completed successfully
     */
    bool run_simulation(const std::string& testbench_file, int num_threads = 8);
    
    /**
     * @brief Parse measurement results from .mt0 file
     * @param mt0_file Path to .mt0 measurement file
     * @return Map of measurement name to value
     */
    std::map<std::string, double> parse_measurements(const std::string& mt0_file);
    
    /**
     * @brief Verify BIST results
     * @param measurements Measurement results from simulation
     * @return true if all tests passed
     */
    bool verify_results(const std::map<std::string, double>& measurements);
    
    /**
     * @brief Run complete BIST verification flow
     * Generates testbench, runs simulation, and verifies results
     * @param quick_mode Use quick testbench (fewer addresses)
     * @return true if verification passed
     */
    bool run_bist_verification(bool quick_mode = false);
    
    /**
     * @brief Get SRAM cell name from configuration
     * @return SRAM subcircuit name (e.g., "sram_x32x4")
     */
    std::string get_sram_cell_name() const;

private:
    SpiceSimConfig config_;
    
    /**
     * @brief Generate testbench header with includes and parameters
     */
    std::string generate_header() const;
    
    /**
     * @brief Generate power supply definitions
     */
    std::string generate_power_supplies() const;
    
    /**
     * @brief Generate clock and reset signals
     * @param t_start Simulation start time
     * @param t_end Simulation end time
     */
    std::string generate_clock_reset(double t_start, double t_end) const;
    
    /**
     * @brief Generate control signals (ce_n, we_n)
     * ce_n: Chip Enable (active low)
     * we_n: Write Enable (active low for write, high for read)
     * @param write_times Vector of write operation times
     * @param read_times Vector of read operation times
     * @param t_end Simulation end time
     */
    std::string generate_control_signals(
        const std::vector<double>& write_times,
        const std::vector<double>& read_times,
        double t_end) const;
    
    /**
     * @brief Generate address signals
     * @param addresses Vector of addresses to test
     * @param times Vector of operation times
     * @param t_end Simulation end time
     */
    std::string generate_address_signals(
        const std::vector<uint64_t>& addresses,
        const std::vector<double>& times,
        double t_end) const;
    
    /**
     * @brief Generate data input signals
     * @param write_data Vector of data values (0 or all 1s)
     * @param write_times Vector of write operation times
     * @param t_end Simulation end time
     */
    std::string generate_data_signals(
        const std::vector<uint64_t>& write_data,
        const std::vector<double>& write_times,
        double t_end) const;
    
    /**
     * @brief Generate SRAM instance
     */
    std::string generate_sram_instance() const;
    
    /**
     * @brief Generate measurement statements
     * @param addresses Vector of addresses to verify
     * @param read0_times Times for Read 0 verification
     * @param read1_times Times for Read 1 verification
     */
    std::string generate_measurements(
        const std::vector<uint64_t>& addresses,
        const std::vector<double>& read0_times,
        const std::vector<double>& read1_times) const;
    
    /**
     * @brief Generate simulation control statements
     * @param t_end Simulation end time
     */
    std::string generate_sim_control(double t_end) const;
    
    /**
     * @brief Calculate test pattern timeline
     * @param addresses Addresses to test
     * @param write0_times Output: Write 0 operation times
     * @param read0_times Output: Read 0 verification times
     * @param write1_times Output: Write 1 operation times
     * @param read1_times Output: Read 1 verification times
     * @return Total simulation time
     */
    double calculate_timeline(
        const std::vector<uint64_t>& addresses,
        std::vector<double>& write0_times,
        std::vector<double>& read0_times,
        std::vector<double>& write1_times,
        std::vector<double>& read1_times) const;
    
    /**
     * @brief Print simulation summary
     */
    void print_summary(const std::map<std::string, double>& measurements) const;
};

} // namespace OpenFinRAM
