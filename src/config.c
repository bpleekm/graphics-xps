#include "gxps.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *error, size_t size, const char *message)
{
    snprintf(error, size, "%s", message);
    return -1;
}

static bool equal_case(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 'a' - 'A');
        if (y >= 'A' && y <= 'Z') y = (char)(y + 'a' - 'A');
        if (x != y) return false;
    }
    return *a == *b;
}

int gx_precision_parse(const char *name, gx_precision *precision)
{
    for (unsigned i = 0; i < GX_PRECISION_COUNT; ++i) {
        if (equal_case(name, gx_precision_get((gx_precision)i)->name)) {
            *precision = (gx_precision)i;
            return 0;
        }
    }
    return -1;
}

int gx_parse_bytes(const char *text, uint64_t *bytes)
{
    if (!text || *text < '0' || *text > '9') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long number = strtoull(text, &end, 10);
    if (errno || number == 0) return -1;
    uint64_t multiplier = 1;
    if (*end) {
        char suffix = *end++;
        if (suffix >= 'a' && suffix <= 'z') suffix = (char)(suffix - 'a' + 'A');
        if (suffix == 'K') multiplier = UINT64_C(1024);
        else if (suffix == 'M') multiplier = UINT64_C(1024) * 1024;
        else if (suffix == 'G') multiplier = UINT64_C(1024) * 1024 * 1024;
        else if (suffix != 'B') return -1;
        if (*end && !equal_case(end, "B") && !equal_case(end, "iB")) return -1;
        if (suffix == 'B' && *end) return -1;
    }
    if ((uint64_t)number > UINT64_MAX / multiplier) return -1;
    *bytes = (uint64_t)number * multiplier;
    return 0;
}

void gx_config_defaults(gx_config *config)
{
    memset(config, 0, sizeof(*config));
    config->mode = GX_BENCH;
    config->precision_mask = GX_ALL_PRECISIONS;
    config->automatic_precisions = true;
    config->memory_budget = UINT64_C(256) * 1024 * 1024;
    config->backend = "auto";
    config->warmup_seconds = 0.2;
    for (unsigned i = 0; i < GX_PRECISION_COUNT; ++i) config->seconds[i] = 10;
}

static int parse_seconds(const char *text, double *seconds)
{
    if (!text || *text == '\0') return -1;
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno || *end || !isfinite(value) || value < 0.01 || value > 86400) return -1;
    *seconds = value;
    return 0;
}

static int parse_duration_overrides(const char *text, double values[GX_PRECISION_COUNT],
                                    unsigned *mask)
{
    if (!*text || text[strlen(text) - 1] == ',') return -1;
    char *copy = strdup(text);
    if (!copy) return -1;
    char *cursor = copy;
    int result = 0;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ',');
        if (next) *next++ = '\0';
        char *equals = strchr(cursor, '=');
        if (!equals) { result = -1; break; }
        *equals++ = '\0';
        gx_precision precision;
        double seconds;
        if (gx_precision_parse(cursor, &precision) || parse_seconds(equals, &seconds)) {
            result = -1;
            break;
        }
        values[precision] = seconds;
        *mask |= 1u << precision;
        cursor = next;
    }
    free(copy);
    return result;
}

static int parse_precisions(const char *text, unsigned *mask)
{
    if (equal_case(text, "auto") || equal_case(text, "all")) {
        *mask = GX_ALL_PRECISIONS;
        return 0;
    }
    if (!*text || text[strlen(text) - 1] == ',') return -1;
    char *copy = strdup(text);
    if (!copy) return -1;
    char *cursor = copy;
    *mask = 0;
    int result = 0;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ',');
        if (next) *next++ = '\0';
        gx_precision precision;
        if (gx_precision_parse(cursor, &precision)) { result = -1; break; }
        *mask |= 1u << precision;
        cursor = next;
    }
    free(copy);
    return result;
}

static int parse_unsigned(const char *text, unsigned long long *value)
{
    if (!text || *text < '0' || *text > '9') return -1;
    char *end = NULL;
    errno = 0;
    *value = strtoull(text, &end, 10);
    return errno || *end ? -1 : 0;
}

int gx_config_parse(int argc, char **argv, gx_config *config, char *error, size_t size)
{
    gx_config_defaults(config);
    if (argc > 1 && argv[1][0] != '-') {
        if (strcmp(argv[1], "probe") == 0) config->mode = GX_PROBE;
        else if (strcmp(argv[1], "bench") != 0) return fail(error, size, "Expected probe or bench");
        --argc;
        ++argv;
    }
    double default_seconds = 10;
    unsigned duration_mask = 0;
    double overrides[GX_PRECISION_COUNT] = {0};
#ifdef __GLIBC__
    optind = 0; /* Reset GNU getopt's internal state for repeated parser tests. */
#else
    optind = 1;
#ifdef __APPLE__
    extern int optreset;
    optreset = 1;
#endif
#endif
    opterr = 0;
    int option;
    while ((option = getopt(argc, argv, "hm:p:t:n:o:b:d:w:Vv")) != -1) {
        unsigned long long integer = 0;
        switch (option) {
        case 'h': config->help = true; break;
        case 'V': config->version = true; break;
        case 'v': config->verbose = true; break;
        case 'm':
            if (gx_parse_bytes(optarg, &config->memory_budget))
                return fail(error, size, "Invalid -m budget; use e.g. 256M or 1G");
            break;
        case 'p':
            if (parse_precisions(optarg, &config->precision_mask))
                return fail(error, size, "Invalid -p precision list");
            config->automatic_precisions = equal_case(optarg, "auto") || equal_case(optarg, "all");
            break;
        case 't':
            if (strchr(optarg, '=')) {
                if (parse_duration_overrides(optarg, overrides, &duration_mask))
                    return fail(error, size, "Invalid -t override; use fp16=20,int8=15");
            } else if (parse_seconds(optarg, &default_seconds))
                return fail(error, size, "Invalid -t duration; use 0.01..86400 seconds");
            break;
        case 'n':
            if (parse_unsigned(optarg, &integer) || integer < 1 || integer > GX_MAX_N)
                return fail(error, size, "Invalid -n dimension; use 1..32768");
            config->matrix_n = (uint32_t)integer;
            break;
        case 'o':
            if (!*optarg) return fail(error, size, "Empty -o path");
            config->output = optarg;
            break;
        case 'b': config->backend = optarg; break;
        case 'd':
            if (parse_unsigned(optarg, &integer) || integer > UINT_MAX)
                return fail(error, size, "Invalid -d GPU index");
            config->device_index = (unsigned)integer;
            break;
        case 'w': {
            char *end = NULL;
            errno = 0;
            config->warmup_seconds = strtod(optarg, &end);
            if (errno || end == optarg || *end || !isfinite(config->warmup_seconds) ||
                config->warmup_seconds < 0 || config->warmup_seconds > 60)
                return fail(error, size, "Invalid -w warmup; use 0..60 seconds");
            break;
        }
        default: return fail(error, size, "Unknown option or missing option value; use -h");
        }
    }
    if (optind != argc) return fail(error, size, "Unexpected positional argument");
    for (unsigned i = 0; i < GX_PRECISION_COUNT; ++i)
        config->seconds[i] = duration_mask & (1u << i) ? overrides[i] : default_seconds;
    return 0;
}

void gx_usage(FILE *stream)
{
    fprintf(stream,
        "graphics-xps %s: serial GPU FP/INT GEMM benchmark\n"
        "Usage: graphics-xps [bench|probe] [options]\n"
        "  -m 256M       Maximum explicit GPU A+B+C/workspace bytes (M/G are binary)\n"
        "  -p LIST       fp16,fp32,fp64,int8,int16,int32,int64; omitted = auto\n"
        "  -t 10         Default timed seconds per precision (0.01..86400)\n"
        "  -t fp16=20,int8=15  Per-precision overrides; repeatable, overrides win\n"
        "  -n N          Explicit square matrix size; must fit -m; omitted = auto\n"
        "  -b auto       Backend: auto or opencl\n"
        "  -d 0          GPU index within backend; CPU devices are excluded\n"
        "  -w 0.2        Warmup seconds, excluded from -t\n"
        "  -o FILE       New JSON report; parent directory must exist\n"
        "  -V            Version\n"
        "  -v            Verbose setup/progress on stderr; default stdout is a table\n"
        "  -h            Help\n"
        "probe builds kernels and verifies small matrices, without a timed benchmark.\n"
        "No arguments prints help. Each precision/kernel runs sequentially.\n", GX_VERSION);
}

static bool matrix_bytes(uint32_t n, gx_precision precision, gx_plan *plan)
{
    const gx_precision_info *info = gx_precision_get(precision);
    uint64_t elements = (uint64_t)n * n;
    unsigned bytes = 2 * info->input_bytes + info->accumulator_bytes;
    if (elements > UINT64_MAX / bytes) return false;
    plan->n = n;
    plan->a_bytes = plan->b_bytes = elements * info->input_bytes;
    plan->c_bytes = elements * info->accumulator_bytes;
    plan->total_bytes = elements * bytes;
    return plan->a_bytes <= SIZE_MAX && plan->c_bytes <= SIZE_MAX;
}

static bool plan_fits(const gx_plan *plan, const gx_caps *caps, uint64_t budget)
{
    return plan->total_bytes <= budget && plan->a_bytes <= caps->max_allocation &&
           plan->b_bytes <= caps->max_allocation && plan->c_bytes <= caps->max_allocation;
}

int gx_plan_matrix(const gx_config *config, const gx_caps *caps, gx_precision precision,
                   gx_plan *plan, char *error, size_t size)
{
    memset(plan, 0, sizeof(*plan));
    uint64_t budget = config->memory_budget;
    if (caps->global_memory / 2 < budget) budget = caps->global_memory / 2;
    if (config->matrix_n) {
        if (!matrix_bytes(config->matrix_n, precision, plan) || !plan_fits(plan, caps, budget))
            return fail(error, size, "Explicit -n matrix exceeds -m or device allocation limits");
    } else {
        uint32_t low = 0, high = GX_AUTO_MAX_N / GX_TILE;
        while (low < high) {
            uint32_t middle = low + (high - low + 1) / 2;
            gx_plan candidate = {0};
            if (matrix_bytes(middle * GX_TILE, precision, &candidate) && plan_fits(&candidate, caps, budget))
                low = middle;
            else high = middle - 1;
        }
        if (!low) return fail(error, size, "Budget cannot fit the minimum auto matrix (N=16)");
        if (!matrix_bytes(low * GX_TILE, precision, plan)) return fail(error, size, "Matrix size overflow");
    }
    plan->requested_budget = config->memory_budget;
    plan->effective_budget = budget;
    return 0;
}

bool gx_operation_count(uint32_t n, uint32_t rows, uint64_t *count)
{
    uint64_t square = (uint64_t)n * n;
    if (!n || !rows || rows > n || square > UINT64_MAX / 2 / rows) return false;
    *count = 2 * square * rows;
    return true;
}
