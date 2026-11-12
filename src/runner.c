#include "gx_backend.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void set_status(gx_result *result, const char *status)
{
    snprintf(result->status, sizeof(result->status), "%s", status);
}

static int verify_cell(const gx_backend_ops *backend, void *device, void *problem,
                       gx_precision precision, uint32_t n, uint32_t seed,
                       uint32_t row, uint32_t column, const void *cached,
                       gx_result *result)
{
    unsigned char cell[8];
    double actual;
    if (cached) actual = gx_load_accumulator(precision, cached, (size_t)row * n + column);
    else {
        if (backend->read(device, problem, (size_t)row * n + column, 1, cell,
                          result->reason, sizeof(result->reason))) return -1;
        actual = gx_load_accumulator(precision, cell, 0);
    }
    double expected = gx_reference(precision, n, row, column, seed);
    double absolute, relative;
    bool good = gx_compare(precision, expected, actual, &absolute, &relative);
    if (absolute > result->max_abs_error) result->max_abs_error = absolute;
    if (relative > result->max_rel_error) result->max_rel_error = relative;
    ++result->verified_cells;
    if (!good) {
        snprintf(result->reason, sizeof(result->reason),
                 "Mismatch at C[%u,%u]: expected %.17g, got %.17g", row, column, expected, actual);
        return -1;
    }
    return 0;
}

static int verify_problem(const gx_backend_ops *backend, void *device, void *problem,
                          gx_precision precision, uint32_t n, uint32_t touched_rows,
                          uint32_t seed, gx_result *result)
{
    if (!touched_rows || touched_rows > n) {
        snprintf(result->reason, sizeof(result->reason), "No valid completed rows to verify");
        return -1;
    }
    if (n <= 64) {
        unsigned bytes = gx_precision_get(precision)->accumulator_bytes;
        void *cells = malloc((size_t)touched_rows * n * bytes);
        if (!cells) { snprintf(result->reason, sizeof(result->reason), "Verification allocation failed"); return -1; }
        if (backend->read(device, problem, 0, (size_t)touched_rows * n, cells,
                          result->reason, sizeof(result->reason))) { free(cells); return -1; }
        for (uint32_t row = 0; row < touched_rows; ++row) {
            for (uint32_t column = 0; column < n; ++column) {
                if (verify_cell(backend, device, problem, precision, n, seed, row, column, cells, result)) {
                    free(cells); return -1;
                }
            }
        }
        free(cells);
    } else {
        uint64_t seen[64];
        uint64_t elements = (uint64_t)touched_rows * n;
        uint64_t state = UINT64_C(0x9e3779b97f4a7c15) ^ seed;
        for (unsigned sample = 0; sample < 64; ++sample) {
            uint64_t index;
            if (sample == 0) index = 0;
            else if (sample == 1) index = n - 1;
            else if (sample == 2) index = (uint64_t)(touched_rows - 1) * n;
            else if (sample == 3) index = elements - 1;
            else {
                bool duplicate;
                do {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    index = state % elements;
                    duplicate = false;
                    for (unsigned i = 0; i < sample; ++i) if (seen[i] == index) duplicate = true;
                } while (duplicate);
            }
            seen[sample] = index;
            if (verify_cell(backend, device, problem, precision, n, seed,
                (uint32_t)(index / n), (uint32_t)(index % n), NULL, result)) return -1;
        }
    }
    result->verified = true;
    return 0;
}

static int small_check(const gx_backend_ops *backend, void *device, void *kernel,
                       const gx_config *config, const gx_caps *caps, gx_result *result,
                       uint32_t seed, gx_plan *small_plan)
{
    gx_config small_config = *config;
    small_config.matrix_n = result->plan.n < 32 ? result->plan.n : 32;
    if (gx_plan_matrix(&small_config, caps, result->precision, small_plan,
                       result->reason, sizeof(result->reason))) return -1;
    void *problem = NULL;
    int failed = backend->create(device, kernel, result->precision, small_plan, &problem,
                                 result->reason, sizeof(result->reason));
    if (failed) return -1;
    gx_result tiny;
    memset(&tiny, 0, sizeof(tiny));
    uint64_t ns = 0;
    failed = backend->initialize(device, problem, seed, result->reason, sizeof(result->reason)) ||
             backend->execute(device, problem, 0, small_plan->n, &ns, result->reason, sizeof(result->reason));
    if (!failed && verify_problem(backend, device, problem, result->precision, small_plan->n,
                                  small_plan->n, seed, &tiny)) {
        snprintf(result->reason, sizeof(result->reason), "%s", tiny.reason);
        failed = -1;
    }
    backend->destroy(device, problem);
    if (small_plan->total_bytes > result->peak_gpu_bytes) result->peak_gpu_bytes = small_plan->total_bytes;
    if (failed) return -1;
    result->self_test_passed = true;
    result->self_test_n = small_plan->n;
    result->self_test_cells = tiny.verified_cells;
    return 0;
}

static void run_precision(const gx_backend_ops *backend, void *device, const gx_caps *caps,
                           const gx_config *config, gx_result *result)
{
    const double setup_begin = gx_now();
    void *kernel = NULL, *problem = NULL;
    const uint32_t seed = 0x31415926u ^ (unsigned)(result->precision + 1) * 0x9e3779b9u;
    if (!result->declared_supported) {
        set_status(result, "unsupported");
        snprintf(result->reason, sizeof(result->reason), "Device does not declare the required type/arithmetic capability");
        return;
    }
    if (gx_plan_matrix(config, caps, result->precision, &result->plan,
                       result->reason, sizeof(result->reason))) { set_status(result, "budget_error"); return; }
    if (backend->compile(device, result->precision, &kernel, &result->kernel,
                          result->reason, sizeof(result->reason))) { set_status(result, "compile_failed"); return; }
    result->compiled = true;
    gx_plan small_plan;
    if (small_check(backend, device, kernel, config, caps, result, seed ^ 0xabcdef01u, &small_plan)) {
        set_status(result, "self_test_failed"); goto cleanup;
    }
    if (config->mode == GX_PROBE) {
        result->plan = small_plan;
        result->verified_cells = result->self_test_cells;
        result->verified = true;
        result->setup_seconds = gx_now() - setup_begin;
        set_status(result, "passed"); goto cleanup;
    }
    if (gx_interrupted()) { set_status(result, "interrupted"); goto cleanup; }
    if (backend->create(device, kernel, result->precision, &result->plan, &problem,
                         result->reason, sizeof(result->reason))) { set_status(result, "allocation_failed"); goto cleanup; }
    result->peak_gpu_bytes = result->plan.total_bytes;
    if (backend->initialize(device, problem, seed, result->reason, sizeof(result->reason))) {
        set_status(result, "initialization_failed"); goto cleanup;
    }
    result->setup_seconds = gx_now() - setup_begin;
    const double calibration_begin = gx_now();
    uint32_t first_rows = result->plan.n < GX_TILE ? result->plan.n : GX_TILE;
    uint64_t sample_ns = 0;
    if (backend->execute(device, problem, 0, first_rows, &sample_ns, result->reason, sizeof(result->reason))) {
        set_status(result, "runtime_failed"); goto cleanup;
    }
    double estimated_rows = first_rows * (40000000.0 / (double)sample_ns);
    if (estimated_rows > 512) estimated_rows = 512;
    uint32_t batch_rows = (uint32_t)estimated_rows;
    batch_rows = batch_rows / GX_TILE * GX_TILE;
    if (batch_rows < GX_TILE) batch_rows = GX_TILE;
    if (batch_rows > result->plan.n) batch_rows = result->plan.n;
    result->batch_rows = batch_rows;
    double warmup_begin = gx_now();
    while (!gx_interrupted() && gx_now() - warmup_begin < config->warmup_seconds) {
        if (backend->execute(device, problem, 0, batch_rows, &sample_ns, result->reason, sizeof(result->reason))) {
            set_status(result, "runtime_failed"); goto cleanup;
        }
    }
    result->calibration_seconds = gx_now() - calibration_begin;
    if (gx_interrupted()) { set_status(result, "interrupted"); goto cleanup; }

    if (config->verbose) fprintf(stderr, "  %-5s N=%u GPU=%.2f MiB/%0.2f MiB, %s, target %.2fs\n",
        gx_precision_get(result->precision)->name, result->plan.n,
        result->plan.total_bytes / 1048576.0, config->memory_budget / 1048576.0,
        result->kernel.path, result->requested_seconds);
    double cpu_begin = gx_cpu_time();
    double begin = gx_now();
    double next_progress = 1;
    uint32_t row = 0;
    uint64_t written_rows = 0;
    bool failed = false;
    while (!gx_interrupted() && gx_now() - begin < result->requested_seconds) {
        uint32_t rows = result->plan.n - row;
        if (rows > batch_rows) rows = batch_rows;
        uint64_t ns = 0, operations = 0;
        if (!gx_operation_count(result->plan.n, rows, &operations) ||
            operations > UINT64_MAX - result->operations) {
            snprintf(result->reason, sizeof(result->reason), "Operation counter overflow"); failed = true; break;
        }
        if (backend->execute(device, problem, row, rows, &ns, result->reason, sizeof(result->reason))) {
            failed = true; break;
        }
        result->operations += operations;
        result->gpu_seconds += ns / 1e9;
        ++result->batches;
        written_rows += rows;
        row += rows;
        if (row == result->plan.n) row = 0;
        double elapsed = gx_now() - begin;
        if (config->verbose && elapsed >= next_progress) {
            fprintf(stderr, "    %5.1fs  %.3f %s  (device time)\n", elapsed,
                   result->operations / result->gpu_seconds / 1e9,
                   gx_precision_get(result->precision)->floating ? "GFLOPS" : "GOPS");
            next_progress = floor(elapsed) + 1;
        }
    }
    result->wall_seconds = gx_now() - begin;
    result->cpu_seconds = gx_cpu_time() - cpu_begin;
    result->touched_rows = written_rows > result->plan.n ? result->plan.n : (uint32_t)written_rows;
    if (failed) { set_status(result, "runtime_failed"); goto cleanup; }
    if (!result->operations) {
        set_status(result, gx_interrupted() ? "interrupted" : "runtime_failed");
        snprintf(result->reason, sizeof(result->reason), "No timed batch completed"); goto cleanup;
    }
    double verification_begin = gx_now();
    if (verify_problem(backend, device, problem, result->precision, result->plan.n,
                        result->touched_rows, seed, result)) set_status(result, "verification_failed");
    else set_status(result, gx_interrupted() ? "interrupted" : "passed");
    result->verification_seconds = gx_now() - verification_begin;

cleanup:
    if (problem) backend->destroy(device, problem);
    if (kernel) backend->release_kernel(device, kernel);
}

int gx_run(const gx_config *config, gx_report *report)
{
    const gx_backend_ops *backend = gx_backend_find(config->backend);
    if (!backend) {
        snprintf(report->error, sizeof(report->error), "Backend '%s' is not built; available: opencl", config->backend);
        snprintf(report->status, sizeof(report->status), "failed");
        return 1;
    }
    int lock = gx_lock_device(backend->name, config->device_index, report->error, sizeof(report->error));
    if (lock < 0) { snprintf(report->status, sizeof(report->status), "failed"); return 4; }
    void *device = NULL;
    if (backend->open(config->device_index, &device, &report->caps, report->error, sizeof(report->error))) {
        snprintf(report->status, sizeof(report->status), "failed"); gx_unlock_device(lock); return 1;
    }
    report->have_device = true;
    printf("graphics-xps %s | backend=%s | GPU index=%u\n", GX_VERSION, backend->name,
           config->device_index);
    printf("GPU budget <= %.2f MiB | precision=%s | serial execution\n",
           config->memory_budget / 1048576.0,
           config->automatic_precisions ? "auto" : "explicit filter");
    gx_table_header(stdout, config->mode);
    unsigned passed = 0, errors = 0;
    for (unsigned i = 0; i < GX_PRECISION_COUNT && !gx_interrupted(); ++i) {
        if (!(config->precision_mask & (1u << i))) continue;
        gx_result *result = &report->results[report->result_count++];
        memset(result, 0, sizeof(*result));
        result->precision = (gx_precision)i;
        result->requested_seconds = config->seconds[i];
        result->declared_supported = report->caps.supported[i];
        run_precision(backend, device, &report->caps, config, result);
        bool good = strcmp(result->status, "passed") == 0;
        bool unsupported = strcmp(result->status, "unsupported") == 0;
        if (good) ++passed;
        else if (!unsupported) ++errors;
        gx_table_row(stdout, config->mode, result);
        fflush(stdout);
        char save_error[GX_ERROR_SIZE];
        report->peak_rss_bytes = gx_peak_rss();
        if (gx_report_save(report, config, save_error, sizeof(save_error))) {
            snprintf(report->error, sizeof(report->error), "%s", save_error);
            ++errors;
            break;
        }
    }
    gx_table_footer(stdout, config->mode);
    for (size_t i = 0; i < report->result_count; ++i) {
        const gx_result *result = &report->results[i];
        if (result->reason[0]) printf("%s: %s\n", gx_precision_get(result->precision)->name, result->reason);
        if (config->verbose && result->kernel.detail[0])
            fprintf(stderr, "%s path: %s\n", gx_precision_get(result->precision)->name, result->kernel.detail);
    }
    backend->close(device);
    gx_unlock_device(lock);
    int code = gx_interrupted() ? 130 : errors ? 1 : passed ? 0 : 3;
    snprintf(report->status, sizeof(report->status), "%s",
             code == 130 ? "interrupted" : code ? "failed" : "completed");
    if (code == 3) snprintf(report->error, sizeof(report->error), "No requested precision is supported");
    return code;
}
