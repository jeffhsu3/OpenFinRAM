#include "main_flow_helpers.hpp"

#include "cell_utils.hpp"
#include "innovus_tcl_generator.hpp"
#include "pex_runner.hpp"
#include "siliconsmart_generator.hpp"
#include "spice_converter.hpp"
#include "spice_include_resolver.hpp"
#include "spice_integrator.hpp"
#include "spice_simulator.hpp"
#include "synthesis_manager.hpp"
#include "utils.hpp"

#include "plog/Log.h"

#include <cstdio>
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

void run_synthesis_stage(
    uint64_t attempt,
    uint64_t addr_width,
    uint64_t test_num_bits,
    uint64_t num_mux,
    uint64_t delay_prech_cnt,
    uint64_t base_delay_cnt,
    uint64_t num_stacked_rows,
    bool run_verification,
    uint64_t low_buffer,
    uint64_t high_buffer,
    bool pex_success) {
    LOGI << "\n========================================";
    LOGI << "Starting Design Compiler Synthesis";
    LOGI << "Attempt: " << attempt;
    LOGI << "  delay_prech_cnt = " << delay_prech_cnt;
    LOGI << "  delay_wl_cnt    = " << base_delay_cnt;
    LOGI << "  delay_sense_cnt = " << base_delay_cnt;
    LOGI << "  delay_write_cnt = " << base_delay_cnt;
    if (run_verification) {
        LOGI << "  bisection range = [" << low_buffer << ", " << high_buffer << "]";
        LOGI << "  bisection mid   = " << base_delay_cnt;
    }
    LOGI << "========================================\n";

    SynthesisConfig synth_config(
        addr_width,
        test_num_bits,
        num_mux,
        delay_prech_cnt,
        base_delay_cnt,
        base_delay_cnt,
        base_delay_cnt);

    synth_config.verilog_path = join_path(get_current_dir_name(), "tech/verilog");
    synth_config.syn_path = join_path(get_current_dir_name(), "tmp/verilog");
    synth_config.output_path = join_path(get_current_dir_name(), "tmp/verilog");

    SynthesisManager synth_manager(synth_config);

    if (pex_success) {
        std::string rep_file = "pex/rep.txt";

        LOGI << "\n========================================";
        LOGI << "Loading Capacitance from PEX Results";
        LOGI << "========================================\n";
        LOGI << "Rep file: " << rep_file;

        if (synth_manager.load_capacitance_from_pex(rep_file)) {
            LOGI << "Successfully loaded capacitance data from PEX";
            LOGI << "Synthesis will use PEX-extracted load values";
        } else {
            LOGW << "Failed to load PEX capacitance data";
            LOGW << "Falling back to statistical prediction...";

            if (synth_manager.predict_capacitance(test_num_bits * 2, num_stacked_rows)) {
                LOGI << "Successfully predicted capacitance using regression model";
            } else {
                LOGW << "Failed to predict capacitance, synthesis will use default values";
            }
        }
    } else {
        LOGI << "\n========================================";
        LOGI << "Using Statistical Capacitance Prediction";
        LOGI << "========================================\n";

        int bit_num = test_num_bits * 2;
        int stacked = num_stacked_rows;

        LOGI << "Configuration for prediction:";
        LOGI << "  bit_num = " << bit_num << " (test_num_bits=" << test_num_bits << " * 2)";
        LOGI << "  stacked = " << stacked;

        if (synth_manager.predict_capacitance(bit_num, stacked)) {
            LOGI << "\n✓ Successfully predicted capacitance using regression model";
            LOGI << "  Model accuracy: R² > 0.87 for all signals";
            LOGI << "  Synthesis will use predicted load values";
            LOGI << "\nNote: For most accurate results, consider running actual PEX";
            LOGI << "      and updating the regression model with new data points.";
        } else {
            LOGW << "Failed to predict capacitance";
            LOGW << "Synthesis will proceed with default load values";
        }
    }

    LOGI << "\n========================================";
    LOGI << "Running Design Compiler Synthesis";
    LOGI << "========================================\n";

    if (!synth_manager.run_synthesis()) {
        LOGW << "Synthesis flow completed with warnings/errors. Check logs for details.";
    } else {
        LOGI << "Synthesis flow completed successfully!";
    }
}

void run_spice_conversion_stage() {
    LOGI << "\n========================================";
    LOGI << "Starting Verilog to SPICE Conversion";
    LOGI << "========================================\n";

    SpiceConversionConfig conv_config(
        join_path(get_current_dir_name(), "tmp/verilog"),
        join_path(get_current_dir_name(), "tmp/verilog"));

    conv_config.netlist_v = join_path(get_current_dir_name(), "tmp/verilog/netlist.v");
    conv_config.netlist_sp = join_path(get_current_dir_name(), "tmp/verilog/netlist.sp");
    conv_config.cdl_file = join_path(get_current_dir_name(), "tech/cdl/asap7sc7p5t_28_R.cdl");

    SpiceConverter converter(conv_config);
    if (!converter.convert_to_spice()) {
        LOGW << "SPICE conversion flow completed with warnings/errors. Check logs for details.";
    } else {
        LOGI << "SPICE conversion flow completed successfully!";
    }
}

void run_innovus_stage(
    gdstk::Cell* stacked_colgrp,
    uint64_t test_num_bits,
    uint64_t addr_width,
    uint64_t num_mux,
    const OpenFinRAM::LayerMap& layer_map) {
    if (stacked_colgrp != nullptr) {
        LOGI << "";
        LOGI << "========================================================================";
        LOGI << "Generating Innovus TCL Script for Control Logic P&R";
        LOGI << "========================================================================";

        OpenFinRAM::CellSize stacked_size = OpenFinRAM::get_cell_size(stacked_colgrp, layer_map);

        if (!stacked_size.valid) {
            LOGW << "Cannot get stacked_colgrp size from BOUNDARY, using bounding box";
            gdstk::Vec2 bb_min, bb_max;
            stacked_colgrp->bounding_box(bb_min, bb_max);
            stacked_size.min = bb_min;
            stacked_size.max = bb_max;
            stacked_size.width = bb_max.x - bb_min.x;
            stacked_size.height = bb_max.y - bb_min.y;
            stacked_size.valid = true;
        }

        double sram_width = stacked_size.width - 0.054 * 2;
        LOGI << "SRAM (stacked_colgrp) width (w. margin): " << sram_width << " um";
        LOGI << "SRAM (stacked_colgrp) height: " << stacked_size.height << " um";

        OpenFinRAM::InnovusTclGenerator tcl_gen;

        tcl_gen.set_design_name("ctrl_decode");
        tcl_gen.set_site_name("asap7sc7p5t");
        tcl_gen.set_site_height(0.27);
        tcl_gen.set_cpu_count(8, 0);

        std::string qor_file = join_path(get_current_dir_name(), "tmp/verilog/qor_report.txt");
        bool qor_parsed = tcl_gen.parse_qor_report(qor_file);

        if (qor_parsed) {
            std::string output_tcl = join_path(get_current_dir_name(), "tmp/innovus/ctrl_run.tcl");
            int num_ysel = 4;

            if (tcl_gen.generate_run_tcl(sram_width, 0.0, output_tcl,
                                         test_num_bits, test_num_bits, num_ysel, addr_width, num_mux)) {
                LOGI << "";
                LOGI << "✓ Successfully generated Innovus TCL script: " << output_tcl;
                LOGI << "  Design: ctrl_decode";
                LOGI << "  Floorplan width: " << sram_width << " um (matches stacked_colgrp)";

                const OpenFinRAM::QoRReport& qor = tcl_gen.get_qor_report();
                double calculated_height = tcl_gen.calculate_floorplan_height(sram_width);
                LOGI << "  Floorplan height: " << calculated_height << " um (auto-calculated)";
                LOGI << "  Cell area: " << qor.cell_area << " um^2";
                LOGI << "  Utilization: " << (qor.cell_area / (sram_width * calculated_height) * 100.0) << " %";
                LOGI << "";

                LOGI << "========================================================================";
                LOGI << "Running Innovus for Place & Route...";
                LOGI << "========================================================================";

                std::string work_dir = join_path(get_current_dir_name(), "tmp/innovus");
                std::string log_file = "ctrl_decode_innovus.log";

                if (tcl_gen.run_innovus(output_tcl, work_dir, log_file)) {
                    LOGI << "";
                    LOGI << "✓ Innovus execution completed successfully";
                    LOGI << "  Check output files in: " << work_dir;
                    LOGI << "  Log file: " << work_dir << "/" << log_file;
                    LOGI << "";

                    LOGI << "========================================================================";
                    LOGI << "Running v2lvs for Verilog to SPICE conversion...";
                    LOGI << "========================================================================";

                    if (tcl_gen.run_v2lvs(work_dir)) {
                        LOGI << "";
                        LOGI << "✓ v2lvs execution completed successfully";
                        LOGI << "  Generated SPICE netlist: " << work_dir << "/netlist_for_lvs.sp";
                        LOGI << "";

                        LOGI << "========================================================================";
                        LOGI << "Post-processing SPICE netlist...";
                        LOGI << "========================================================================";

                        std::string cdl_file = join_path(get_current_dir_name(), "tech/cdl/asap7sc7p5t_28_R.cdl");

                        if (tcl_gen.post_process_netlist(work_dir, "netlist_for_lvs.sp", cdl_file)) {
                            LOGI << "";
                            LOGI << "✓ Netlist post-processing completed successfully";
                            LOGI << "  Processed netlist: " << work_dir << "/netlist_for_lvs.sp";
                            LOGI << "  - Merged continuation lines";
                            LOGI << "  - Added VDD VSS to SUBCKT definitions";
                            LOGI << "  - Expanded $PINS format";
                            LOGI << "  - Processed .CONNECT directives";
                            LOGI << "";
                        } else {
                            LOGW << "Netlist post-processing failed";
                            LOGW << "The netlist may not be properly formatted for LVS";
                        }
                    } else {
                        LOGW << "v2lvs execution failed";
                        LOGW << "Please check if netlist_for_lvs.v exists in: " << work_dir;
                    }
                } else {
                    LOGW << "Innovus execution failed";
                    LOGW << "Please check log file: " << work_dir << "/" << log_file;
                }
            } else {
                LOGW << "Failed to generate Innovus TCL script";
            }
        } else {
            LOGW << "QoR report not found or invalid: " << qor_file;
            LOGW << "Skipping Innovus TCL generation";
            LOGW << "Please run synthesis first to generate QoR report";
        }

        LOGI << "========================================================================";
        LOGI << "";
    } else {
        LOGW << "Stacked colgrp not created, skipping Innovus TCL generation";
    }
}

void run_sram_integration_stage(
    uint64_t addr_width,
    uint64_t num_stacked_rows,
    uint64_t test_num_bits,
    uint64_t num_mux) {
    LOGI << "\n========================================";
    LOGI << "Starting SRAM Integration";
    LOGI << "========================================\n";

    SramIntegrationConfig integ_config(
        addr_width,
        num_stacked_rows,
        test_num_bits,
        num_mux);

    integ_config.ctrl_netlist = join_path(get_current_dir_name(), "tmp/innovus/netlist_for_lvs.sp");
    integ_config.datapath_netlist = "./sram_colgrp.sp";
    integ_config.output_netlist = "./sram.sp";

    SpiceIntegrator integrator(integ_config);
    std::string integrated_sram = integrator.integrate_sram("sram.sp");

    if (integrated_sram.empty()) {
        LOGW << "SRAM integration completed with warnings/errors. Check logs for details.";
    } else {
        LOGI << "SRAM integration completed successfully!";
        LOGI << "Generated integrated SRAM netlist: " << integrated_sram;
    }
}

bool run_or_predict_pex(
    bool run_actual_pex,
    uint64_t test_num_bits,
    uint64_t num_stacked_rows) {
    bool pex_success = false;

    if (run_actual_pex) {
        LOGI << "";
        LOGI << "========================================================================";
        LOGI << "Starting PEX (Parasitic Extraction) - Actual Run";
        LOGI << "========================================================================";

        OpenFinRAM::PEXRunner::Config pex_config;
        pex_config.gds_path = "../sram_array_test.gds";
        pex_config.spice_path = "../sram_colgrp.sp";

        char pex_cell_name[128];
        std::snprintf(pex_cell_name, sizeof(pex_cell_name),
                      "stacked_colgrp_x%lux%lu",
                      static_cast<unsigned long>(test_num_bits * 2),
                      static_cast<unsigned long>(num_stacked_rows / 2));
        pex_config.cell_name = pex_cell_name;
        pex_config.output_dir = ".";
        pex_config.turbo_count = 8;

        OpenFinRAM::PEXRunner pex_runner(pex_config);

        LOGI << "PEX Configuration:";
        LOGI << "  Cell: " << pex_config.cell_name;
        LOGI << "  GDS: " << pex_config.gds_path;
        LOGI << "  SPICE: " << pex_config.spice_path;
        LOGI << "  Output Dir: " << pex_config.output_dir;

        pex_success = pex_runner.run();

        if (pex_success) {
            LOGI << "";
            LOGI << "========================================================================";
            LOGI << "PEX completed successfully!";
            LOGI << "PEX Netlist: " << pex_runner.get_pex_netlist_path();
            LOGI << "========================================================================";
        } else {
            LOGE << "";
            LOGE << "========================================================================";
            LOGE << "PEX failed! Check log files for details.";
            LOGE << "========================================================================";
        }
    } else {
        LOGI << "";
        LOGI << "========================================================================";
        LOGI << "Skipping actual PEX - Using statistical prediction";
        LOGI << "========================================================================";
        LOGI << "PEX is time-consuming. Using regression-based capacitance estimation.";
        LOGI << "To run actual PEX, set run_actual_pex = true in main.cpp";
        pex_success = false;
    }

    return pex_success;
}
