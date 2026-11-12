#include "gxps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static gx_caps fake_caps(void)
{
    gx_caps caps;
    memset(&caps, 0, sizeof(caps));
    caps.global_memory = UINT64_C(8) * 1024 * 1024 * 1024;
    caps.max_allocation = UINT64_C(2) * 1024 * 1024 * 1024;
    return caps;
}

static int parse(int count, char **values, gx_config *config)
{
    char error[GX_ERROR_SIZE];
    return gx_config_parse(count, values, config, error, sizeof(error));
}

static void test_bytes(void)
{
    uint64_t bytes = 0;
    CHECK(!gx_parse_bytes("256M", &bytes) && bytes == UINT64_C(256) * 1024 * 1024);
    CHECK(!gx_parse_bytes("1G", &bytes) && bytes == UINT64_C(1) * 1024 * 1024 * 1024);
    CHECK(!gx_parse_bytes("2GiB", &bytes) && bytes == UINT64_C(2) * 1024 * 1024 * 1024);
    CHECK(!gx_parse_bytes("17k", &bytes) && bytes == 17 * 1024);
    CHECK(!gx_parse_bytes("64B", &bytes) && bytes == 64);
    CHECK(!gx_parse_bytes("42", &bytes) && bytes == 42);
    CHECK(gx_parse_bytes("0M", &bytes));
    CHECK(gx_parse_bytes("-1G", &bytes));
    CHECK(gx_parse_bytes("1.5G", &bytes));
    CHECK(gx_parse_bytes("1GBgarbage", &bytes));
    CHECK(gx_parse_bytes("1BB", &bytes));
    CHECK(gx_parse_bytes("18446744073709551615G", &bytes));
    CHECK(gx_parse_bytes("18446744073709551616", &bytes));
    CHECK(gx_parse_bytes("", &bytes));
}

static void test_parser(void)
{
    gx_config config;
    char *automatic[] = {"gx", "bench", "-pauto", "-t10", "-m1G"};
    CHECK(!parse(5, automatic, &config));
    CHECK(config.automatic_precisions && config.precision_mask == GX_ALL_PRECISIONS);
    CHECK(config.memory_budget == UINT64_C(1073741824));
    for (unsigned i = 0; i < GX_PRECISION_COUNT; ++i) CHECK(config.seconds[i] == 10);

    char *defaults[] = {"gx", "-m256M"};
    CHECK(!parse(2, defaults, &config) && config.automatic_precisions);
    char *overrides[] = {"gx", "bench", "-t", "fp16=20,int8=15", "-t", "3", "-t", "int8=7", "-p", "FP16,int8"};
    CHECK(!parse(10, overrides, &config));
    CHECK(!config.automatic_precisions);
    CHECK(config.precision_mask == ((1u << GX_FP16) | (1u << GX_INT8)));
    CHECK(config.seconds[GX_FP16] == 20 && config.seconds[GX_INT8] == 7 && config.seconds[GX_FP32] == 3);

    char *probe[] = {"gx", "probe", "-d0", "-n37", "-w0"};
    CHECK(!parse(5, probe, &config) && config.mode == GX_PROBE && config.matrix_n == 37);

    char *bad_lists[] = {"gx", "-p", "fp16,,fp32"};
    CHECK(parse(3, bad_lists, &config));
    char *bad_precision[] = {"gx", "-p", "fp8"};
    CHECK(parse(3, bad_precision, &config));
    char *bad_override[] = {"gx", "-t", "fp16=2,,int8=3"};
    CHECK(parse(3, bad_override, &config));
    char *trailing[] = {"gx", "-t", "fp16=2,"};
    CHECK(parse(3, trailing, &config));
    char *nan_time[] = {"gx", "-t", "nan"};
    CHECK(parse(3, nan_time, &config));
    char *infinite[] = {"gx", "-t", "inf"};
    CHECK(parse(3, infinite, &config));
    char *zero_time[] = {"gx", "-t0"};
    CHECK(parse(2, zero_time, &config));
    char *zero_n[] = {"gx", "-n0"};
    CHECK(parse(2, zero_n, &config));
    char *large_n[] = {"gx", "-n4294967296"};
    CHECK(parse(2, large_n, &config));
    char *negative_device[] = {"gx", "-d-1"};
    CHECK(parse(2, negative_device, &config));
    char *unknown[] = {"gx", "-Q"};
    CHECK(parse(2, unknown, &config));
    char *missing[] = {"gx", "-m"};
    CHECK(parse(2, missing, &config));
}

static void test_budgets(void)
{
    gx_config config;
    gx_config_defaults(&config);
    gx_caps caps = fake_caps();
    gx_plan plan;
    char error[GX_ERROR_SIZE];
    config.memory_budget = UINT64_C(1073741824);
    for (unsigned i = 0; i < GX_PRECISION_COUNT; ++i) {
        CHECK(!gx_plan_matrix(&config, &caps, (gx_precision)i, &plan, error, sizeof(error)));
        CHECK(plan.n == GX_AUTO_MAX_N);
        CHECK(plan.total_bytes == plan.a_bytes + plan.b_bytes + plan.c_bytes);
        CHECK(plan.total_bytes <= config.memory_budget);
    }
    config.memory_budget = 1024 * 1024;
    CHECK(!gx_plan_matrix(&config, &caps, GX_FP64, &plan, error, sizeof(error)));
    CHECK(plan.n == 208 && plan.total_bytes == UINT64_C(1038336));
    config.matrix_n = 37;
    CHECK(!gx_plan_matrix(&config, &caps, GX_INT8, &plan, error, sizeof(error)));
    CHECK(plan.total_bytes == 37u * 37u * 6u);
    config.matrix_n = 32768;
    CHECK(gx_plan_matrix(&config, &caps, GX_FP64, &plan, error, sizeof(error)));
    config.matrix_n = 0;
    caps.max_allocation = 8192;
    CHECK(!gx_plan_matrix(&config, &caps, GX_FP64, &plan, error, sizeof(error)));
    CHECK(plan.n == 32 && plan.c_bytes == 8192);
    caps = fake_caps();
    config.memory_budget = 64;
    CHECK(gx_plan_matrix(&config, &caps, GX_FP64, &plan, error, sizeof(error)));
    config.matrix_n = 1;
    CHECK(!gx_plan_matrix(&config, &caps, GX_FP64, &plan, error, sizeof(error)));
    CHECK(plan.total_bytes == 24);
    config.matrix_n = 0;
    config.memory_budget = UINT64_C(1073741824);
    caps.global_memory = 2 * 1024 * 1024;
    CHECK(!gx_plan_matrix(&config, &caps, GX_FP64, &plan, error, sizeof(error)));
    CHECK(plan.effective_budget == 1024 * 1024 && plan.total_bytes <= plan.effective_budget);

    caps = fake_caps();
    uint32_t random = 1234567;
    for (unsigned i = 0; i < 2000; ++i) {
        random = random * 1664525u + 1013904223u;
        config.memory_budget = (random % 1048576u) + 1;
        gx_precision precision = (gx_precision)(i % GX_PRECISION_COUNT);
        if (!gx_plan_matrix(&config, &caps, precision, &plan, error, sizeof(error))) {
            CHECK(plan.total_bytes <= config.memory_budget);
            CHECK(plan.a_bytes <= caps.max_allocation && plan.c_bytes <= caps.max_allocation);
            CHECK(plan.n % GX_TILE == 0);
        }
    }
}

static void test_numeric(void)
{
    CHECK(gx_float_to_half(1) == 0x3c00);
    CHECK(gx_float_to_half(-0.0f) == 0x8000);
    CHECK(gx_float_to_half(65504) == 0x7bff);
    CHECK(gx_float_to_half(65520) == 0x7c00);
    CHECK(gx_float_to_half(ldexpf(1, -24)) == 1);
    CHECK(gx_float_to_half(ldexpf(1, -25)) == 0);
    CHECK(gx_float_to_half(3 * ldexpf(1, -25)) == 2);
    CHECK(gx_float_to_half(1.00048828125f) == 0x3c00);
    CHECK(gx_float_to_half(1.00146484375f) == 0x3c02);
    for (unsigned bits = 0; bits <= 65535; ++bits) {
        float value = gx_half_to_float((uint16_t)bits);
        if ((bits & 0x7c00) == 0x7c00 && (bits & 0x3ff)) CHECK(isnan(value));
        else CHECK(gx_float_to_half(value) == bits);
    }
    double absolute, relative;
    CHECK(gx_compare(GX_INT64, 12345, 12345, &absolute, &relative));
    CHECK(!gx_compare(GX_INT64, 12345, 12346, &absolute, &relative));
    CHECK(!gx_compare(GX_FP32, 1, NAN, &absolute, &relative));
    CHECK(!gx_compare(GX_FP64, 1, INFINITY, &absolute, &relative));
    CHECK(!gx_compare(GX_FP32, 1, 1.1, &absolute, &relative));
    double integer = gx_reference(GX_INT32, 37, 2, 11, 99);
    CHECK(gx_reference(GX_INT8, 37, 2, 11, 99) == integer);
    CHECK(gx_reference(GX_INT16, 37, 2, 11, 99) == integer);
    CHECK(gx_reference(GX_INT64, 37, 2, 11, 99) == integer);
    CHECK(gx_reference(GX_FP32, 37, 2, 11, 99) == integer / 256);
    CHECK(gx_reference(GX_FP64, 37, 2, 11, 99) == integer / 256);
    CHECK(gx_reference(GX_FP16, 37, 2, 11, 99) == integer / 256);
    uint64_t operations;
    CHECK(gx_operation_count(37, 19, &operations) && operations == 52022);
    CHECK(!gx_operation_count(0, 1, &operations));
    CHECK(!gx_operation_count(37, 38, &operations));
    CHECK(!gx_operation_count(UINT32_MAX, UINT32_MAX, &operations));
}

static void test_table(void)
{
    char *buffer = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&buffer, &length);
    CHECK(stream != NULL);
    gx_result result;
    memset(&result, 0, sizeof(result));
    result.precision = GX_FP32;
    snprintf(result.status, sizeof(result.status), "passed");
    result.plan.n = 32;
    result.plan.total_bytes = 32 * 32 * 12;
    result.operations = 2000000000;
    result.gpu_seconds = 1;
    result.wall_seconds = 2;
    gx_table_header(stream, GX_BENCH);
    gx_table_row(stream, GX_BENCH, &result);
    result.precision = GX_INT8;
    gx_table_row(stream, GX_BENCH, &result);
    gx_table_footer(stream, GX_BENCH);
    CHECK(fclose(stream) == 0 && length > 0);
    CHECK(strstr(buffer, "GFLOPS") && strstr(buffer, "GOPS"));
    CHECK(strstr(buffer, "2.000") && strstr(buffer, "1.000"));
    free(buffer);

    buffer = NULL;
    stream = open_memstream(&buffer, &length);
    CHECK(stream != NULL);
    memset(&result, 0, sizeof(result));
    result.precision = GX_FP64;
    snprintf(result.status, sizeof(result.status), "unsupported");
    gx_table_header(stream, GX_BENCH);
    gx_table_row(stream, GX_BENCH, &result);
    gx_table_footer(stream, GX_BENCH);
    CHECK(fclose(stream) == 0);
    CHECK(strstr(buffer, "unsupported") && !strstr(buffer, "0.000"));
    free(buffer);
}

static void test_minimal_report(void)
{
    gx_report report;
    gx_report_init(&report);
    report.have_device = true;
    snprintf(report.caps.backend, sizeof(report.caps.backend), "opencl");
    report.caps.global_memory = UINT64_C(8) * 1024 * 1024 * 1024;
    gx_config config;
    gx_config_defaults(&config);
    char *buffer = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&buffer, &length);
    CHECK(stream != NULL);
    gx_report_write(stream, &report, &config);
    CHECK(fclose(stream) == 0 && length > 0);
    CHECK(strstr(buffer, "\"metadata_policy\": \"minimal\"") != NULL);
    CHECK(strstr(buffer, "\"resources\"") != NULL);
    CHECK(strstr(buffer, "\"started_utc\"") == NULL && strstr(buffer, "\"finished_utc\"") == NULL);
    CHECK(strstr(buffer, "\"name\"") == NULL && strstr(buffer, "\"hostname\"") == NULL);
    CHECK(strstr(buffer, "\"host\"") == NULL && strstr(buffer, "\"system\"") == NULL);
    CHECK(strstr(buffer, "\"driver\"") == NULL && strstr(buffer, "\"global_memory_bytes\"") == NULL);
    free(buffer);
}

int main(void)
{
    test_bytes();
    test_parser();
    test_budgets();
    test_numeric();
    test_table();
    test_minimal_report();
    printf("PASS: %u parser, budget, precision, half-conversion and accounting checks\n", checks);
    return 0;
}
