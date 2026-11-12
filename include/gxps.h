#ifndef GXPS_H
#define GXPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GX_VERSION "0.1.2"
#define GX_ERROR_SIZE 2048
#define GX_TILE 16u
#define GX_AUTO_MAX_N 4096u
#define GX_MAX_N 32768u
#define GX_ALL_PRECISIONS ((1u << GX_PRECISION_COUNT) - 1u)

typedef enum {
    GX_FP16, GX_FP32, GX_FP64, GX_INT8, GX_INT16, GX_INT32, GX_INT64,
    GX_PRECISION_COUNT
} gx_precision;

typedef enum { GX_BENCH, GX_PROBE } gx_mode;

typedef struct {
    const char *name;
    const char *arithmetic;
    unsigned input_bytes;
    unsigned accumulator_bytes;
    bool floating;
} gx_precision_info;

typedef struct {
    gx_mode mode;
    unsigned precision_mask;
    bool automatic_precisions;
    double seconds[GX_PRECISION_COUNT];
    uint64_t memory_budget;
    uint32_t matrix_n;
    unsigned device_index;
    const char *backend;
    const char *output;
    double warmup_seconds;
    bool help;
    bool version;
    bool verbose;
} gx_config;

typedef struct {
    char backend[32];
    uint64_t global_memory;
    uint64_t max_allocation;
    size_t max_work_group;
    bool supported[GX_PRECISION_COUNT];
    bool int8_dot;
} gx_caps;

typedef struct {
    uint32_t n;
    uint64_t requested_budget;
    uint64_t effective_budget;
    uint64_t a_bytes, b_bytes, c_bytes;
    uint64_t total_bytes;
} gx_plan;

typedef struct {
    char path[96];
    char detail[512];
} gx_kernel_info;

typedef struct {
    gx_precision precision;
    char status[32];
    char reason[GX_ERROR_SIZE];
    gx_kernel_info kernel;
    gx_plan plan;
    bool declared_supported;
    bool compiled;
    bool self_test_passed;
    bool verified;
    double requested_seconds;
    double setup_seconds;
    double calibration_seconds;
    double wall_seconds;
    double gpu_seconds;
    double cpu_seconds;
    double verification_seconds;
    uint64_t operations;
    uint64_t batches;
    uint64_t verified_cells;
    uint64_t self_test_cells;
    uint64_t peak_gpu_bytes;
    uint32_t batch_rows;
    uint32_t touched_rows;
    uint32_t self_test_n;
    double max_abs_error;
    double max_rel_error;
} gx_result;

typedef struct {
    char status[32];
    char error[GX_ERROR_SIZE];
    uint64_t peak_rss_bytes;
    gx_caps caps;
    bool have_device;
    gx_result results[GX_PRECISION_COUNT];
    size_t result_count;
} gx_report;

const gx_precision_info *gx_precision_get(gx_precision precision);
int gx_precision_parse(const char *name, gx_precision *precision);
int gx_parse_bytes(const char *text, uint64_t *bytes);
void gx_config_defaults(gx_config *config);
int gx_config_parse(int argc, char **argv, gx_config *config, char *error, size_t size);
void gx_usage(FILE *stream);
int gx_plan_matrix(const gx_config *config, const gx_caps *caps, gx_precision precision,
                   gx_plan *plan, char *error, size_t size);
bool gx_operation_count(uint32_t n, uint32_t rows, uint64_t *count);
uint16_t gx_float_to_half(float value);
float gx_half_to_float(uint16_t value);
double gx_input_value(gx_precision precision, uint32_t row, uint32_t column,
                      uint32_t seed, bool matrix_b);
void gx_store_input(gx_precision precision, void *destination, size_t index, double value);
double gx_load_accumulator(gx_precision precision, const void *source, size_t index);
double gx_reference(gx_precision precision, uint32_t n, uint32_t row, uint32_t column,
                    uint32_t seed);
bool gx_compare(gx_precision precision, double expected, double actual,
                double *absolute, double *relative);
double gx_now(void);
double gx_cpu_time(void);
uint64_t gx_peak_rss(void);
void gx_install_signals(void);
bool gx_interrupted(void);
int gx_lock_device(const char *backend, unsigned index, char *error, size_t size);
void gx_unlock_device(int descriptor);
void gx_report_init(gx_report *report);
void gx_report_write(FILE *stream, const gx_report *report, const gx_config *config);
int gx_report_save(const gx_report *report, const gx_config *config,
                   char *error, size_t size);
void gx_table_header(FILE *stream, gx_mode mode);
void gx_table_row(FILE *stream, gx_mode mode, const gx_result *result);
void gx_table_footer(FILE *stream, gx_mode mode);

#ifdef __cplusplus
}
#endif

#endif
