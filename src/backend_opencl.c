#include "gx_backend.h"

#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#define CL_SILENCE_DEPRECATION
#include <OpenCL/opencl.h>
#include <OpenCL/cl_ext.h>
#else
#include <CL/cl.h>
#include <CL/cl_ext.h>
#endif

#include "gemm_source.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    cl_device_id device;
    cl_context context;
    cl_command_queue queue;
    gx_caps caps;
} opencl_device;

typedef struct {
    cl_program program;
    cl_kernel kernel;
} opencl_kernel;

typedef struct {
    opencl_kernel *kernel;
    cl_mem a, b, c;
    gx_precision precision;
    gx_plan plan;
    uint64_t live_bytes;
} opencl_problem;

static int cl_error(char *error, size_t size, const char *operation, cl_int code)
{
    snprintf(error, size, "%s: OpenCL error %d", operation, (int)code);
    return -1;
}

static void device_string(cl_device_id device, cl_device_info key, char *output, size_t size)
{
    size_t needed = 0;
    output[0] = '\0';
    if (clGetDeviceInfo(device, key, 0, NULL, &needed) || !needed) return;
    char *value = calloc(needed + 1, 1);
    if (!value) return;
    if (!clGetDeviceInfo(device, key, needed, value, NULL)) snprintf(output, size, "%s", value);
    free(value);
}

static bool has_extension(const char *extensions, const char *name)
{
    size_t length = strlen(name);
    const char *position = extensions;
    while ((position = strstr(position, name)) != NULL) {
        if ((position == extensions || position[-1] == ' ') &&
            (position[length] == ' ' || position[length] == '\0')) return true;
        position += length;
    }
    return false;
}

static void opencl_close(void *opaque)
{
    opencl_device *device = opaque;
    if (!device) return;
    if (device->queue) { clFinish(device->queue); clReleaseCommandQueue(device->queue); }
    if (device->context) clReleaseContext(device->context);
    free(device);
}

static int opencl_open(unsigned index, void **opaque, gx_caps *caps, char *error, size_t size)
{
    *opaque = NULL;
    cl_uint count = 0;
    cl_int code = clGetPlatformIDs(0, NULL, &count);
    if (code || !count) {
        snprintf(error, size, "No OpenCL platform (error %d). Check the ICD/runtime and GPU permissions "
                 "(Linux: read/write access to render nodes, usually the render group).", (int)code);
        return -1;
    }
    cl_platform_id *platforms = calloc(count, sizeof(*platforms));
    if (!platforms) { snprintf(error, size, "Out of host memory"); return -1; }
    code = clGetPlatformIDs(count, platforms, NULL);
    if (code) { free(platforms); return cl_error(error, size, "List platforms", code); }
    cl_device_id selected = NULL;
    unsigned current = 0;
    for (cl_uint p = 0; p < count && !selected; ++p) {
        cl_uint devices_count = 0;
        code = clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 0, NULL, &devices_count);
        if (code == CL_DEVICE_NOT_FOUND) continue;
        if (code) { free(platforms); return cl_error(error, size, "List GPU count", code); }
        cl_device_id *devices = calloc(devices_count, sizeof(*devices));
        if (!devices) { free(platforms); snprintf(error, size, "Out of host memory"); return -1; }
        code = clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, devices_count, devices, NULL);
        if (!code) {
            for (cl_uint d = 0; d < devices_count; ++d, ++current) {
                if (current == index) { selected = devices[d]; break; }
            }
        }
        free(devices);
        if (code) { free(platforms); return cl_error(error, size, "List GPUs", code); }
    }
    free(platforms);
    if (!selected) { snprintf(error, size, "GPU index %u not found (CPU devices excluded)", index); return -1; }
    opencl_device *device = calloc(1, sizeof(*device));
    if (!device) { snprintf(error, size, "Out of host memory"); return -1; }
    device->device = selected;
    memset(caps, 0, sizeof(*caps));
    snprintf(caps->backend, sizeof(caps->backend), "opencl");
    cl_bool little = CL_TRUE;
    cl_ulong memory = 0, allocation = 0;
    size_t group = 0;
    code = clGetDeviceInfo(selected, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(memory), &memory, NULL);
    if (!code) code = clGetDeviceInfo(selected, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(allocation), &allocation, NULL);
    if (!code) code = clGetDeviceInfo(selected, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(group), &group, NULL);
    if (code) { opencl_close(device); return cl_error(error, size, "Required device limits", code); }
    clGetDeviceInfo(selected, CL_DEVICE_ENDIAN_LITTLE, sizeof(little), &little, NULL);
    uint16_t endian_test = 1;
    bool host_little = *(unsigned char *)&endian_test != 0;
    if ((little != CL_FALSE) != host_little) {
        snprintf(error, size, "Different host/device endianness is not implemented by this backend");
        opencl_close(device); return -1;
    }
    caps->global_memory = memory;
    caps->max_allocation = allocation;
    caps->max_work_group = group;
    size_t extension_length = 0;
    clGetDeviceInfo(selected, CL_DEVICE_EXTENSIONS, 0, NULL, &extension_length);
    char *extensions = calloc(extension_length + 1, 1);
    if (!extensions) { opencl_close(device); snprintf(error, size, "Out of host memory"); return -1; }
    clGetDeviceInfo(selected, CL_DEVICE_EXTENSIONS, extension_length, extensions, NULL);
    caps->supported[GX_FP16] = has_extension(extensions, "cl_khr_fp16");
    cl_device_fp_config single = 0, wide = 0;
    clGetDeviceInfo(selected, CL_DEVICE_SINGLE_FP_CONFIG, sizeof(single), &single, NULL);
    clGetDeviceInfo(selected, CL_DEVICE_DOUBLE_FP_CONFIG, sizeof(wide), &wide, NULL);
    caps->supported[GX_FP32] = single != 0;
    caps->supported[GX_FP64] = wide != 0 && has_extension(extensions, "cl_khr_fp64");
    caps->supported[GX_INT8] = caps->supported[GX_INT16] = caps->supported[GX_INT32] = true;
    char profile[64];
    device_string(selected, CL_DEVICE_PROFILE, profile, sizeof(profile));
    cl_uint long_width = 0;
    if (strcmp(profile, "FULL_PROFILE") != 0)
        clGetDeviceInfo(selected, CL_DEVICE_NATIVE_VECTOR_WIDTH_LONG, sizeof(long_width), &long_width, NULL);
    caps->supported[GX_INT64] = strcmp(profile, "FULL_PROFILE") == 0 || long_width != 0;
    caps->int8_dot = has_extension(extensions, "cl_khr_integer_dot_product");
#if defined(CL_DEVICE_INTEGER_DOT_PRODUCT_CAPABILITIES_KHR) && defined(CL_DEVICE_INTEGER_DOT_PRODUCT_INPUT_4x8BIT_KHR)
    cl_device_integer_dot_product_capabilities_khr dot_caps = 0;
    if (caps->int8_dot && !clGetDeviceInfo(selected, CL_DEVICE_INTEGER_DOT_PRODUCT_CAPABILITIES_KHR,
                                         sizeof(dot_caps), &dot_caps, NULL))
        caps->int8_dot = (dot_caps & CL_DEVICE_INTEGER_DOT_PRODUCT_INPUT_4x8BIT_KHR) != 0;
#endif
    free(extensions);
    device->context = clCreateContext(NULL, 1, &selected, NULL, NULL, &code);
    if (code) { opencl_close(device); return cl_error(error, size, "Create GPU context", code); }
    device->queue = clCreateCommandQueue(device->context, selected, CL_QUEUE_PROFILING_ENABLE, &code);
    if (code) { opencl_close(device); return cl_error(error, size, "Create serial profiling queue", code); }
    device->caps = *caps;
    *opaque = device;
    return 0;
}

static void opencl_release_kernel(void *device, void *opaque)
{
    (void)device;
    opencl_kernel *kernel = opaque;
    if (!kernel) return;
    if (kernel->kernel) clReleaseKernel(kernel->kernel);
    if (kernel->program) clReleaseProgram(kernel->program);
    free(kernel);
}

static int build_kernel(opencl_device *device, gx_precision precision, bool dot,
                         opencl_kernel **output, char *error, size_t size)
{
    static const char *input_types[GX_PRECISION_COUNT] = {"half", "float", "double", "char", "short", "int", "long"};
    static const char *acc_types[GX_PRECISION_COUNT] = {"half", "float", "double", "int", "int", "int", "long"};
    opencl_kernel *kernel = calloc(1, sizeof(*kernel));
    if (!kernel) { snprintf(error, size, "Out of host memory"); return -1; }
    const char *source = (const char *)gx_kernel_source;
    cl_int code = 0;
    kernel->program = clCreateProgramWithSource(device->context, 1, &source, NULL, &code);
    if (code) { opencl_release_kernel(device, kernel); return cl_error(error, size, "Create program", code); }
    char options[256];
    snprintf(options, sizeof(options),
             "-cl-std=%s -DGX_IN=%s -DGX_ACC=%s -DGX_HALF=%d -DGX_DOUBLE=%d -DGX_FLOAT=%d -DGX_DOT=%d",
             dot ? "CL3.0" : "CL1.2", input_types[precision], acc_types[precision],
             precision == GX_FP16, precision == GX_FP64,
             gx_precision_get(precision)->floating, dot);
    code = clBuildProgram(kernel->program, 1, &device->device, options, NULL, NULL);
    if (code) {
        size_t needed = 0;
        clGetProgramBuildInfo(kernel->program, device->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &needed);
        char *log = calloc(needed + 1, 1);
        if (log) clGetProgramBuildInfo(kernel->program, device->device, CL_PROGRAM_BUILD_LOG, needed, log, NULL);
        snprintf(error, size, "Kernel build error %d: %s", (int)code, log ? log : "no build log");
        free(log);
        opencl_release_kernel(device, kernel);
        return -1;
    }
    kernel->kernel = clCreateKernel(kernel->program, "gx_gemm", &code);
    if (code) { opencl_release_kernel(device, kernel); return cl_error(error, size, "Create GEMM kernel", code); }
    size_t maximum = 0;
    clGetKernelWorkGroupInfo(kernel->kernel, device->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(maximum), &maximum, NULL);
    if (maximum < 64) {
        snprintf(error, size, "Kernel cannot run a 64-work-item tile");
        opencl_release_kernel(device, kernel); return -1;
    }
    *output = kernel;
    return 0;
}

static int opencl_compile(void *opaque, gx_precision precision, void **output,
                           gx_kernel_info *info, char *error, size_t size)
{
    opencl_device *device = opaque;
    opencl_kernel *kernel = NULL;
    memset(info, 0, sizeof(*info));
    if (precision == GX_INT8 && device->caps.int8_dot) {
        char dot_error[GX_ERROR_SIZE];
        if (!build_kernel(device, precision, true, &kernel, dot_error, sizeof(dot_error))) {
            snprintf(info->path, sizeof(info->path), "opencl-int8-dot-int32");
            snprintf(info->detail, sizeof(info->detail), "cl_khr_integer_dot_product char4 dot, INT32 accumulation");
        } else {
            snprintf(info->detail, sizeof(info->detail), "Dot kernel unavailable; explicit widened fallback: %.420s", dot_error);
        }
    }
    if (!kernel && build_kernel(device, precision, false, &kernel, error, size)) return -1;
    if (!info->path[0]) {
        const char *kind = precision == GX_INT8 || precision == GX_INT16 ? "widened-int32" :
                           gx_precision_get(precision)->floating ? "fma" : "integer-mul-add";
        snprintf(info->path, sizeof(info->path), "opencl-%s-%s", gx_precision_get(precision)->name, kind);
        if (!info->detail[0]) snprintf(info->detail, sizeof(info->detail), "%s; compiler-selected device instructions",
                                      gx_precision_get(precision)->arithmetic);
    }
    *output = kernel;
    return 0;
}

static void opencl_destroy(void *opaque, void *problem_opaque)
{
    opencl_device *device = opaque;
    opencl_problem *problem = problem_opaque;
    if (!problem) return;
    clFinish(device->queue);
    if (problem->a) clReleaseMemObject(problem->a);
    if (problem->b) clReleaseMemObject(problem->b);
    if (problem->c) clReleaseMemObject(problem->c);
    free(problem);
}

static int allocate(opencl_device *device, opencl_problem *problem, uint64_t bytes,
                     cl_mem *buffer, char *error, size_t size)
{
    if (bytes > SIZE_MAX || bytes > device->caps.max_allocation ||
        problem->live_bytes > problem->plan.effective_budget ||
        bytes > problem->plan.effective_budget - problem->live_bytes) {
        snprintf(error, size, "Explicit GPU allocation would exceed the memory budget");
        return -1;
    }
    cl_int code = 0;
    *buffer = clCreateBuffer(device->context, CL_MEM_READ_WRITE, (size_t)bytes, NULL, &code);
    if (code) return cl_error(error, size, "Allocate GPU matrix", code);
    problem->live_bytes += bytes;
    return 0;
}

static int opencl_create(void *opaque, void *kernel, gx_precision precision,
                          const gx_plan *plan, void **output, char *error, size_t size)
{
    opencl_device *device = opaque;
    *output = NULL;
    opencl_problem *problem = calloc(1, sizeof(*problem));
    if (!problem) { snprintf(error, size, "Out of host memory"); return -1; }
    problem->kernel = kernel;
    problem->precision = precision;
    problem->plan = *plan;
    if (allocate(device, problem, plan->a_bytes, &problem->a, error, size) ||
        allocate(device, problem, plan->b_bytes, &problem->b, error, size) ||
        allocate(device, problem, plan->c_bytes, &problem->c, error, size)) {
        opencl_destroy(device, problem);
        return -1;
    }
    *output = problem;
    return 0;
}

static int opencl_initialize(void *opaque, void *problem_opaque, uint32_t seed,
                              char *error, size_t size)
{
    opencl_device *device = opaque;
    opencl_problem *problem = problem_opaque;
    unsigned input_bytes = gx_precision_get(problem->precision)->input_bytes;
    size_t elements = (size_t)problem->plan.n * problem->plan.n;
    size_t chunk_elements = (1024u * 1024u) / input_bytes;
    void *chunk = malloc(chunk_elements * input_bytes);
    if (!chunk) { snprintf(error, size, "Cannot allocate 1 MiB host initialization buffer"); return -1; }
    for (unsigned matrix = 0; matrix < 2; ++matrix) {
        for (size_t first = 0; first < elements; first += chunk_elements) {
            size_t count = elements - first;
            if (count > chunk_elements) count = chunk_elements;
            for (size_t i = 0; i < count; ++i) {
                size_t index = first + i;
                double value = gx_input_value(problem->precision, (uint32_t)(index / problem->plan.n),
                    (uint32_t)(index % problem->plan.n), seed, matrix != 0);
                gx_store_input(problem->precision, chunk, i, value);
            }
            cl_int code = clEnqueueWriteBuffer(device->queue, matrix ? problem->b : problem->a, CL_TRUE,
                first * input_bytes, count * input_bytes, chunk, 0, NULL, NULL);
            if (code) { free(chunk); return cl_error(error, size, "Initialize GPU input", code); }
        }
    }
    free(chunk);
    uint16_t zero = 0;
    cl_int code = clEnqueueFillBuffer(device->queue, problem->c, &zero, sizeof(zero), 0,
                                     (size_t)problem->plan.c_bytes, 0, NULL, NULL);
    if (!code) code = clFinish(device->queue);
    return code ? cl_error(error, size, "Initialize GPU output", code) : 0;
}

static int opencl_execute(void *opaque, void *problem_opaque, uint32_t row_start, uint32_t rows,
                           uint64_t *device_ns, char *error, size_t size)
{
    opencl_device *device = opaque;
    opencl_problem *problem = problem_opaque;
    cl_kernel kernel = problem->kernel->kernel;
    cl_uint n = problem->plan.n, start = row_start, count = rows;
    if (!rows || row_start >= n || rows > n - row_start) {
        snprintf(error, size, "Invalid GEMM batch bounds"); return -1;
    }
    cl_int code = clSetKernelArg(kernel, 0, sizeof(problem->a), &problem->a);
    if (!code) code = clSetKernelArg(kernel, 1, sizeof(problem->b), &problem->b);
    if (!code) code = clSetKernelArg(kernel, 2, sizeof(problem->c), &problem->c);
    if (!code) code = clSetKernelArg(kernel, 3, sizeof(n), &n);
    if (!code) code = clSetKernelArg(kernel, 4, sizeof(start), &start);
    if (!code) code = clSetKernelArg(kernel, 5, sizeof(count), &count);
    if (code) return cl_error(error, size, "Set GEMM arguments", code);
    const size_t local[2] = {8, 8};
    const size_t global[2] = {((size_t)n + 15) / 16 * 8, ((size_t)rows + 15) / 16 * 8};
    cl_event event = NULL;
    code = clEnqueueNDRangeKernel(device->queue, kernel, 2, NULL, global, local, 0, NULL, &event);
    if (code) return cl_error(error, size, "Submit GEMM batch", code);
    code = clWaitForEvents(1, &event);
    cl_ulong begin = 0, end = 0;
    if (!code) code = clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(begin), &begin, NULL);
    if (!code) code = clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(end), &end, NULL);
    clReleaseEvent(event);
    if (code) return cl_error(error, size, "Complete/profile GEMM batch", code);
    if (end <= begin) { snprintf(error, size, "Invalid device profiling timestamps"); return -1; }
    *device_ns = end - begin;
    return 0;
}

static int opencl_read(void *opaque, void *problem_opaque, size_t first, size_t count,
                        void *destination, char *error, size_t size)
{
    opencl_device *device = opaque;
    opencl_problem *problem = problem_opaque;
    size_t elements = (size_t)problem->plan.n * problem->plan.n;
    if (first > elements || count > elements - first) {
        snprintf(error, size, "Readback outside matrix bounds"); return -1;
    }
    unsigned bytes = gx_precision_get(problem->precision)->accumulator_bytes;
    cl_int code = clEnqueueReadBuffer(device->queue, problem->c, CL_TRUE, first * bytes,
                                     count * bytes, destination, 0, NULL, NULL);
    return code ? cl_error(error, size, "Read verification cells", code) : 0;
}

const gx_backend_ops gx_opencl_backend = {
    "opencl", opencl_open, opencl_close, opencl_compile, opencl_release_kernel,
    opencl_create, opencl_destroy, opencl_initialize, opencl_execute, opencl_read
};
