#pragma once

#include <cstdint>

bool decide_base_delay_count(
    bool run_verification,
    bool& force_best_run,
    uint64_t low_buffer,
    uint64_t high_buffer,
    uint64_t best_pass_buffer,
    uint64_t min_buffer_cnt,
    uint64_t max_buffer_cnt,
    uint64_t& base_delay_cnt);

bool update_bisection_on_failure(
    uint64_t base_delay_cnt,
    uint64_t max_buffer_cnt,
    uint64_t best_pass_buffer,
    uint64_t& low_buffer,
    uint64_t high_buffer,
    bool& force_best_run,
    const char* failure_reason);

bool update_bisection_on_success(
    uint64_t base_delay_cnt,
    uint64_t low_buffer,
    bool force_best_run,
    uint64_t& best_pass_buffer,
    uint64_t& high_buffer);

bool run_spice_simulation_verification(
    bool run_verification,
    bool use_random_mode,
    bool use_parallel_mode,
    double random_test_percentage,
    uint64_t random_seed,
    uint64_t addr_width,
    uint64_t num_stacked_rows,
    uint64_t num_wl);

void consolidate_output_artifacts(uint64_t test_num_bits, uint64_t num_stacked_rows, uint64_t num_mux);

bool run_siliconsmart_and_check(
    uint64_t attempt,
    uint64_t test_num_bits,
    uint64_t num_stacked_rows,
    uint64_t addr_width);
