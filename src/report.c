#include "gxps.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void string(FILE *stream, const char *value)
{
    fputc('"', stream);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', stream); fputc(*p, stream); }
        else if (*p < 32) fprintf(stream, "\\u%04x", (unsigned)*p);
        else fputc(*p, stream);
    }
    fputc('"', stream);
}

static const char *boolean(bool value) { return value ? "true" : "false"; }

static const char *bench_border = "+-----------+--------------------+-------+----------+----------+-------------+-------------+--------+----------------------------------+\n";
static const char *probe_border = "+-----------+----------+----------+----------+--------------------+----------------------------------+\n";

void gx_table_header(FILE *stream, gx_mode mode)
{
    if (mode == GX_PROBE) {
        fputs(probe_border, stream);
        fprintf(stream, "| %-9s | %-8s | %-8s | %-8s | %-18s | %-32s |\n",
                "Precision", "Declared", "Compiled", "Checked", "Status", "Kernel path");
        fputs(probe_border, stream);
    } else {
        fputs(bench_border, stream);
        fprintf(stream, "| %-9s | %-18s | %5s | %8s | %8s | %11s | %11s | %-6s | %-32s |\n",
                "Precision", "Status", "N", "GPU MiB", "Time (s)", "GPU rate", "Wall rate", "Unit", "Kernel path");
        fputs(bench_border, stream);
    }
}

void gx_table_row(FILE *stream, gx_mode mode, const gx_result *result)
{
    const gx_precision_info *info = gx_precision_get(result->precision);
    if (mode == GX_PROBE) {
        fprintf(stream, "| %-9s | %-8s | %-8s | %-8s | %-18.18s | %-32.32s |\n", info->name,
                result->declared_supported ? "yes" : "no", result->compiled ? "yes" : "-",
                result->verified ? "PASS" : "-", result->status,
                result->kernel.path[0] ? result->kernel.path : "-");
        return;
    }
    char n[24] = "-", memory[32] = "-", seconds[32] = "-", gpu[32] = "-", wall[32] = "-";
    if (result->plan.n) {
        snprintf(n, sizeof(n), "%u", result->plan.n);
        snprintf(memory, sizeof(memory), "%.2f", result->plan.total_bytes / 1048576.0);
    }
    if (result->wall_seconds > 0) snprintf(seconds, sizeof(seconds), "%.3f", result->wall_seconds);
    if (result->gpu_seconds > 0) snprintf(gpu, sizeof(gpu), "%.3f", result->operations / result->gpu_seconds / 1e9);
    if (result->wall_seconds > 0) snprintf(wall, sizeof(wall), "%.3f", result->operations / result->wall_seconds / 1e9);
    fprintf(stream, "| %-9s | %-18.18s | %5s | %8s | %8s | %11s | %11s | %-6s | %-32.32s |\n",
            info->name, result->status, n, memory, seconds, gpu, wall,
            info->floating ? "GFLOPS" : "GOPS", result->kernel.path[0] ? result->kernel.path : "-");
}

void gx_table_footer(FILE *stream, gx_mode mode)
{
    fputs(mode == GX_PROBE ? probe_border : bench_border, stream);
}

static void rate(FILE *stream, uint64_t operations, double seconds)
{
    if (seconds > 0 && isfinite(seconds)) fprintf(stream, "%.12g", operations / seconds / 1e9);
    else fputs("null", stream);
}

void gx_report_init(gx_report *report)
{
    memset(report, 0, sizeof(*report));
    snprintf(report->status, sizeof(report->status), "running");
}

void gx_report_write(FILE *stream, const gx_report *report, const gx_config *config)
{
    fprintf(stream, "{\n  \"schema_version\": 1,\n  \"program_version\": \"%s\",\n  \"status\": ", GX_VERSION);
    string(stream, report->status);
    fputs(",\n  \"error\": ", stream); string(stream, report->error);
    fputs(",\n  \"metadata_policy\": \"minimal\"", stream);
    fprintf(stream, ",\n  \"resources\": {\"peak_process_rss_bytes\": %" PRIu64 "},\n", report->peak_rss_bytes);
    fprintf(stream, "  \"config\": {\"mode\": \"%s\", \"memory_budget_bytes\": %" PRIu64
            ", \"explicit_matrix_n\": %u, \"auto_matrix_n_ceiling\": %u, \"device_index\": %u, "
            "\"warmup_seconds\": %.9g, \"precision_mask\": %u, \"precision_selection\": \"%s\", \"seconds\": {",
            config->mode == GX_PROBE ? "probe" : "bench", config->memory_budget, config->matrix_n,
            GX_AUTO_MAX_N, config->device_index, config->warmup_seconds, config->precision_mask,
            config->automatic_precisions ? "auto" : "explicit");
    for (unsigned i = 0; i < GX_PRECISION_COUNT; ++i)
        fprintf(stream, "%s\"%s\": %.9g", i ? ", " : "", gx_precision_get((gx_precision)i)->name, config->seconds[i]);
    fputs("}},\n  \"device\": ", stream);
    if (report->have_device) {
        const gx_caps *caps = &report->caps;
        fputs("{\"backend\": ", stream); string(stream, caps->backend);
        fprintf(stream, ", \"int8_dot_extension\": %s, \"capabilities\": {", boolean(caps->int8_dot));
        for (unsigned i = 0; i < GX_PRECISION_COUNT; ++i)
            fprintf(stream, "%s\"%s\": {\"declared\": %s}",
                    i ? ", " : "", gx_precision_get((gx_precision)i)->name,
                    boolean(caps->supported[i]));
        fputs("}}", stream);
    } else fputs("null", stream);
    fputs(",\n  \"definitions\": {"
          "\"operations\": \"2*M*N*K useful GEMM operations; FMA or multiply+add counts as 2\", "
          "\"gpu_gops\": \"completed useful operations / sum of device kernel execution seconds / 1e9\", "
          "\"wall_gops\": \"same operations / timed host wall seconds / 1e9\", "
          "\"duration\": \"host wall target, excluding setup, calibration/warmup and verification; finish current batch\", "
          "\"memory\": \"simultaneously live explicit GPU data buffers; excludes CPU buffers and hidden driver allocations\", "
          "\"verification\": \"full small-matrix check before timing; full result for N<=64, otherwise 64 unique cells including corners\", "
          "\"integer\": \"signed inputs; INT8/INT16 accumulate in INT32; widened and dot paths are labeled separately\", "
          "\"peak\": \"measured tiled GEMM performance, not a theoretical or vendor-library peak\", "
          "\"serial\": true},\n  \"results\": [\n", stream);
    for (size_t i = 0; i < report->result_count; ++i) {
        const gx_result *result = &report->results[i];
        const gx_precision_info *info = gx_precision_get(result->precision);
        fprintf(stream, "%s    {\"precision\": \"%s\", \"arithmetic\": ", i ? ",\n" : "", info->name);
        string(stream, info->arithmetic);
        fputs(", \"status\": ", stream); string(stream, result->status);
        fputs(", \"reason\": ", stream); string(stream, result->reason);
        fputs(", \"kernel_path\": ", stream); string(stream, result->kernel.path);
        fputs(", \"kernel_detail\": ", stream); string(stream, result->kernel.detail);
        fprintf(stream, ", \"declared_supported\": %s, \"compiled\": %s, \"self_test_passed\": %s, "
                "\"self_test_n\": %u, \"self_test_cells\": %" PRIu64 ", \"verified\": %s, "
                "\"matrix_n\": %u, \"a_bytes\": %" PRIu64 ", \"b_bytes\": %" PRIu64
                ", \"c_bytes\": %" PRIu64 ", \"gpu_bytes\": %" PRIu64 ", \"peak_gpu_bytes\": %" PRIu64
                ", \"batch_rows\": %u, \"touched_rows\": %u, "
                "\"requested_seconds\": %.9g, \"setup_seconds\": %.12g, \"calibration_seconds\": %.12g, "
                "\"wall_seconds\": %.12g, \"gpu_seconds\": %.12g, \"cpu_seconds\": %.12g, "
                "\"verification_seconds\": %.12g, \"operations\": %" PRIu64 ", \"batches\": %" PRIu64
                ", \"verified_cells\": %" PRIu64 ", \"max_abs_error\": %.12g, \"max_rel_error\": %.12g, "
                "\"unit\": \"%s\", \"gpu_gops\": ",
                boolean(result->declared_supported), boolean(result->compiled), boolean(result->self_test_passed),
                result->self_test_n, result->self_test_cells, boolean(result->verified), result->plan.n,
                result->plan.a_bytes, result->plan.b_bytes, result->plan.c_bytes, result->plan.total_bytes,
                result->peak_gpu_bytes, result->batch_rows, result->touched_rows,
                result->requested_seconds, result->setup_seconds, result->calibration_seconds,
                result->wall_seconds, result->gpu_seconds, result->cpu_seconds, result->verification_seconds,
                result->operations, result->batches, result->verified_cells, result->max_abs_error, result->max_rel_error,
                info->floating ? "GFLOPS" : "GOPS");
        rate(stream, result->operations, result->gpu_seconds);
        fputs(", \"wall_gops\": ", stream); rate(stream, result->operations, result->wall_seconds);
        fputc('}', stream);
    }
    fputs("\n  ]\n}\n", stream);
}

int gx_report_save(const gx_report *report, const gx_config *config, char *error, size_t size)
{
    if (!config->output) return 0;
    size_t length = strlen(config->output) + 64;
    char *temporary = malloc(length);
    if (!temporary) { snprintf(error, size, "Cannot allocate report path"); return -1; }
    snprintf(temporary, length, "%s.tmp-%lu", config->output, (unsigned long)getpid());
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
        snprintf(error, size, "Cannot create report temporary file: %s", strerror(errno)); free(temporary); return -1;
    }
    FILE *stream = fdopen(descriptor, "w");
    if (!stream) {
        close(descriptor); unlink(temporary); free(temporary);
        snprintf(error, size, "Cannot open report stream"); return -1;
    }
    gx_report_write(stream, report, config);
    int failed = ferror(stream);
    if (fclose(stream)) failed = 1;
    if (!failed && rename(temporary, config->output)) failed = 1;
    if (failed) {
        snprintf(error, size, "Cannot finish report: %s", strerror(errno));
        unlink(temporary);
    }
    free(temporary);
    return failed ? -1 : 0;
}
