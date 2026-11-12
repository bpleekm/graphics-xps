#ifndef GX_BACKEND_H
#define GX_BACKEND_H

#include "gxps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend-neutral C ABI. No OpenCL/Metal/CUDA/HIP types cross this boundary.
 * execute() is synchronous: one completed batch, with device execution ns.
 * create() must account for ALL explicit device data buffers against the plan.
 */
typedef struct gx_backend_ops {
    const char *name;
    int (*open)(unsigned index, void **device, gx_caps *caps, char *error, size_t size);
    void (*close)(void *device);
    int (*compile)(void *device, gx_precision precision, void **kernel,
                   gx_kernel_info *info, char *error, size_t size);
    void (*release_kernel)(void *device, void *kernel);
    int (*create)(void *device, void *kernel, gx_precision precision,
                  const gx_plan *plan, void **problem, char *error, size_t size);
    void (*destroy)(void *device, void *problem);
    int (*initialize)(void *device, void *problem, uint32_t seed, char *error, size_t size);
    int (*execute)(void *device, void *problem, uint32_t row_start, uint32_t rows,
                   uint64_t *device_ns, char *error, size_t size);
    int (*read)(void *device, void *problem, size_t first, size_t count,
                void *destination, char *error, size_t size);
} gx_backend_ops;

extern const gx_backend_ops gx_opencl_backend;
const gx_backend_ops *gx_backend_find(const char *name);
int gx_run(const gx_config *config, gx_report *report);

#ifdef __cplusplus
}
#endif

#endif
