#include "main_flow_helpers.hpp"

#include "siliconsmart_generator.hpp"
#include "spice_include_resolver.hpp"
#include "spice_simulator.hpp"
#include "utils.hpp"

#include "plog/Log.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
bool path_exists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

bool remove_directory_recursive(const std::string& path, std::string* error) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        if (error) {
            *error = std::string("lstat failed: ") + std::strerror(errno);
        }
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (::unlink(path.c_str()) != 0) {
            if (error) {
                *error = std::string("unlink failed: ") + std::strerror(errno);
            }
            return false;
        }
        return true;
    }

    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        if (error) {
            *error = std::string("opendir failed: ") + std::strerror(errno);
        }
        return false;
    }

    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
            continue;
        }

        std::string child = path;
        if (!child.empty() && child[child.size() - 1] != '/') {
            child += '/';
        }
        child += name;

        if (!remove_directory_recursive(child, error)) {
            ::closedir(dir);
            return false;
        }
    }

    ::closedir(dir);
    if (::rmdir(path.c_str()) != 0) {
        if (error) {
            *error = std::string("rmdir failed: ") + std::strerror(errno);
        }
        return false;
    }

    return true;
}
}  // namespace

bool decide_base_delay_count(
    bool run_verification,
    bool& force_best_run,
    uint64_t low_buffer,
    uint64_t high_buffer,
    uint64_t best_pass_buffer,
    uint64_t min_buffer_cnt,
    uint64_t max_buffer_cnt,
    uint64_t& base_delay_cnt) {
    if (run_verification) {
        if (force_best_run) {
            base_delay_cnt = best_pass_buffer;
        } else if (low_buffer > high_buffer) {
            if (best_pass_buffer > 0) {
                force_best_run = true;
                base_delay_cnt = best_pass_buffer;
                LOGI << "Bisection complete. Re-running with best buffer count = " << best_pass_buffer;
            } else {
                LOGE << "Bisection failed: no passing buffer count in [" << min_buffer_cnt << ", " << max_buffer_cnt << "]";
                return false;
            }
        } else {
            base_delay_cnt = low_buffer + (high_buffer - low_buffer) / 2;
        }
    } else {
        base_delay_cnt = 10;
    }

    return true;
}

bool update_bisection_on_failure(
    uint64_t base_delay_cnt,
    uint64_t max_buffer_cnt,
    uint64_t best_pass_buffer,
    uint64_t& low_buffer,
    uint64_t high_buffer,
    bool& force_best_run,
    const char* failure_reason) {
    if (base_delay_cnt >= max_buffer_cnt && best_pass_buffer == 0) {
        LOGE << "Bisection failed: max_buffer_cnt=" << max_buffer_cnt << " also failed";
        return false;
    }

    low_buffer = base_delay_cnt + 1;
    if (low_buffer > high_buffer && best_pass_buffer > 0) {
        force_best_run = true;
    }
    LOGW << "Bisection update: " << failure_reason << ", new range = [" << low_buffer << ", " << high_buffer << "]";
    return true;
}

bool update_bisection_on_success(
    uint64_t base_delay_cnt,
    uint64_t low_buffer,
    bool force_best_run,
    uint64_t& best_pass_buffer,
    uint64_t& high_buffer) {
    LOGI << "SiliconSmart log check passed (no 'Error:   Task').";
    best_pass_buffer = base_delay_cnt;

    if (force_best_run) {
        LOGI << "Bisection complete: minimal passing buffer count = " << best_pass_buffer;
        return false;
    }

    if (base_delay_cnt == 0) {
        high_buffer = 0;
    } else {
        high_buffer = base_delay_cnt - 1;
    }
    LOGI << "Bisection update: pass, new range = [" << low_buffer << ", " << high_buffer << "]";

    if (low_buffer > high_buffer) {
        LOGI << "Bisection complete: minimal passing buffer count = " << best_pass_buffer;
        return false;
    }

    return true;
}

bool run_spice_simulation_verification(
    bool run_verification,
    bool use_random_mode,
    bool use_parallel_mode,
    double random_test_percentage,
    uint64_t random_seed,
    uint64_t addr_width,
    uint64_t num_stacked_rows,
    uint64_t num_wl) {
    bool verification_passed = false;

    if (run_verification) {
        LOGI << "\n========================================";
        LOGI << "Starting SPICE Simulation Verification";
        LOGI << "========================================\n";

        OpenFinRAM::SpiceSimConfig sim_config;
        sim_config.addr_bits = addr_width;
        sim_config.data_bits = num_stacked_rows;
        sim_config.num_wordlines = num_wl * 2;
        sim_config.sram_netlist = "./sram.sp";
        sim_config.output_dir = ".";
        sim_config.t_clk = 10.0;

        OpenFinRAM::SpiceSimulator simulator(sim_config);

        verification_passed = false;
        if (use_random_mode) {
            LOGI << "Using random BIST mode";
            LOGI << "Test coverage: " << random_test_percentage << "% of memory";
            LOGI << "Total addresses: " << (1UL << addr_width);
            LOGI << "Addresses to test: " << static_cast<uint64_t>((1UL << addr_width) * random_test_percentage / 100.0);
            LOGI << "Random seed: " << (random_seed == 0 ? "auto-generated" : std::to_string(random_seed)) << "\n";

            std::string random_tb_file = sim_config.output_dir + "/sram_bist_tb_random.sp";

            if (simulator.generate_random_testbench(random_tb_file, random_test_percentage, random_seed)) {
                LOGI << "\n✓ Generated random testbench: " << random_tb_file;
                LOGI << "Running simulation...";

                if (simulator.run_simulation(random_tb_file, 8)) {
                    std::string mt0_file = random_tb_file;
                    if (mt0_file.size() > 3 && mt0_file.substr(mt0_file.size() - 3) == ".sp") {
                        mt0_file = mt0_file.substr(0, mt0_file.size() - 3);
                    }
                    mt0_file += ".mt0";

                    auto measurements = simulator.parse_measurements(mt0_file);
                    verification_passed = simulator.verify_results(measurements);
                } else {
                    LOGE << "Simulation failed";
                    verification_passed = false;
                }
            } else {
                LOGE << "Failed to generate random testbench";
                verification_passed = false;
            }
        } else if (use_parallel_mode) {
            LOGI << "Using parallel BIST mode - generating per-bit testbenches";
            LOGI << "This will generate " << addr_width << " testbenches (one per address bit)";
            LOGI << "Each testbench tests only 2 addresses for faster simulation";
            LOGI << "You can run these in parallel for maximum speed\n";

            std::vector<std::string> testbench_files = simulator.generate_parallel_testbenches();

            if (testbench_files.empty()) {
                LOGE << "Failed to generate parallel testbenches";
                verification_passed = false;
            } else {
                LOGI << "\n✓ Generated " << testbench_files.size() << " testbench files:";
                for (const auto& tb_file : testbench_files) {
                    LOGI << "  - " << tb_file;
                }

                LOGI << "\nTo run simulations in parallel, use:";
                LOGI << "  Option 1: Run all sequentially:";
                for (size_t i = 0; i < testbench_files.size(); ++i) {
                    LOGI << "    finesim -np 8 -w " << testbench_files[i]
                         << " -o sram_bist_tb_bit" << i;
                }
                LOGI << "\n  Option 2: Run in parallel (example with GNU parallel):";
                LOGI << "    parallel -j 4 'finesim -np 8 -w {} -o {.}' ::: sram_bist_tb_bit*.sp";

                LOGI << "\n  Option 3: Run first testbench only as a quick check:";
                LOGI << "    finesim -np 8 -w " << testbench_files[0]
                     << " -o sram_bist_tb_bit0";

                LOGI << "\nRunning first testbench (bit 0) as verification sample...";
                if (simulator.run_simulation(testbench_files[0], 8)) {
                    std::string mt0_file = testbench_files[0];
                    if (mt0_file.size() > 3 && mt0_file.substr(mt0_file.size() - 3) == ".sp") {
                        mt0_file = mt0_file.substr(0, mt0_file.size() - 3);
                    }
                    mt0_file += ".mt0";

                    auto measurements = simulator.parse_measurements(mt0_file);
                    verification_passed = simulator.verify_results(measurements);

                    if (verification_passed) {
                        LOGI << "✓ Sample testbench (bit 0) PASSED";
                        LOGI << "  All " << testbench_files.size()
                             << " testbenches are ready for parallel execution";
                    } else {
                        LOGW << "✗ Sample testbench (bit 0) FAILED";
                    }
                }
            }
        } else {
            LOGI << "Using quick BIST mode (testing 4 addresses)...";
            verification_passed = simulator.run_bist_verification(true);
        }
    } else {
        LOGW << "Skipping SPICE Simulation Verification (run_verification=0).";
        verification_passed = true;
    }

    if (run_verification) {
        LOGI << "\n========================================";
        if (verification_passed) {
            LOGI << "✓ SRAM verification PASSED - functional correctness confirmed!";
        } else {
            LOGW << "✗ SRAM verification FAILED - check simulation results";
        }
        LOGI << "========================================";
    } else {
        LOGW << "SRAM verification skipped (run_verification=0).";
    }

    return verification_passed;
}

void consolidate_output_artifacts(uint64_t test_num_bits, uint64_t num_stacked_rows, uint64_t num_mux) {
    std::string results_dir = "results";
    if (!directory_exists(results_dir) && !create_directory(results_dir, nullptr)) {
        LOGW << "Failed to create results directory: " << results_dir;
    } else {
        LOGI << "Results directory created: " << results_dir;
    }

    std::string timestamp = get_current_timestamp();
    std::string output_gds_path = join_path("tmp/innovus", "ctrl_decode.gds.tmp");
    std::string final_gds_name = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows) + "x" + std::to_string(num_mux) + ".gds";
    std::string final_gds_path = join_path(join_path(results_dir, timestamp), final_gds_name);
    if (!directory_exists(join_path(results_dir, timestamp)) && !create_directory(join_path(results_dir, timestamp), nullptr)) {
        LOGW << "Failed to create timestamped results directory: " << join_path(results_dir, timestamp);
    } else {
        if (std::rename(output_gds_path.c_str(), final_gds_path.c_str()) != 0) {
            LOGW << "Failed to move generated GDS file to final location: " << final_gds_path;
        } else {
            LOGI << "Generated GDS file moved to: " << final_gds_path;
        }
    }

    {
        const std::string input_sp = join_path(get_current_dir_name(), "sram.sp");
        const std::string output_sp = join_path(get_current_dir_name(), "sram_flat.sp");
        std::string error;

        LOGI << "Expanding SPICE includes: " << input_sp << " -> " << output_sp;

        if (!SpiceIncludeResolver::resolve_to_file(input_sp, output_sp, error)) {
            LOGW << "Failed to expand SPICE includes: " << error;
        } else {
            LOGI << "Successfully generated flat SPICE netlist: " << output_sp;
        }
    }

    std::string output_sp_path = "sram_flat.sp";
    std::string final_sp_name = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows) + "x" + std::to_string(num_mux) + ".sp";
    std::string final_sp_path = join_path(join_path(results_dir, timestamp), final_sp_name);
    if (std::rename(output_sp_path.c_str(), final_sp_path.c_str()) != 0) {
        LOGW << "Failed to move generated SP file to final location: " << final_sp_path;
    } else {
        LOGI << "Generated SP file moved to: " << final_sp_path;
    }
}

bool run_siliconsmart_and_check(
    uint64_t attempt,
    uint64_t test_num_bits,
    uint64_t num_stacked_rows,
    uint64_t addr_width) {
    {
        const std::string input_sp = "./sram.sp";
        const std::string output_sp = "./sram_flat.sp";
        std::string error;

        LOGI << "Expanding SPICE includes: " << input_sp << " -> " << output_sp;

        if (!SpiceIncludeResolver::resolve_to_file(input_sp, output_sp, error)) {
            LOGW << "Failed to expand SPICE includes: " << error;
        } else {
            LOGI << "Successfully generated flat SPICE netlist: " << output_sp;
        }
    }

    const std::string sis_dir = "./sis" + std::stoi(attempt < 10 ? "0" : "") + std::to_string(attempt);
    if (path_exists(sis_dir)) {
        LOGI << "Removing existing SiliconSmart directory: " << sis_dir;
        std::string remove_error;
        if (!remove_directory_recursive(sis_dir, &remove_error)) {
            LOGW << "Failed to remove SiliconSmart directory: " << remove_error;
        }
    }

    std::string sram_cell_name = "sram_x" + std::to_string(test_num_bits * 2) + "x" + std::to_string(num_stacked_rows);

    OpenFinRAM::SiliconSmartConfig sis_config;
    sis_config.cell_name = sram_cell_name;
    sis_config.addr_width = addr_width;
    sis_config.data_width = num_stacked_rows;
    sis_config.sis_dir = sis_dir;
    sis_config.flat_spice_path = "./sram_flat.sp";
    sis_config.configure_template_path = sis_dir + "/configure.tcl";

    OpenFinRAM::SiliconSmartGenerator sis_gen;
    bool sis_ok = sis_gen.generate(sis_config);
    if (!sis_ok) {
        LOGW << "Failed to generate SiliconSmart files";
    } else {
        sis_ok = sis_gen.run_siliconsmart(sis_config);
        if (!sis_ok) {
            LOGW << "Failed to run SiliconSmart";
        }
    }

    bool sis_has_error = !sis_ok;
    const std::string sis_log_path = sis_dir + "/testcase/sis.log";
    std::ifstream sis_log(sis_log_path);
    if (!sis_log.is_open()) {
        LOGW << "Cannot open SiliconSmart log: " << sis_log_path;
        sis_has_error = true;
    } else {
        std::string line;
        while (std::getline(sis_log, line)) {
            if (line.find("Error:   Task") != std::string::npos) {
                sis_has_error = true;
                break;
            }
        }
    }

    return !sis_has_error;
}
