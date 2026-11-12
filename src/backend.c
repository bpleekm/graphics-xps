#include "gx_backend.h"

#include <string.h>

/* Register future C-ABI backend implementations here. Platform-specific APIs
 * remain in their own translation units and Makefile source/link sections. */
static const gx_backend_ops *const backends[] = {
    &gx_opencl_backend,
};

const gx_backend_ops *gx_backend_find(const char *name)
{
    if (strcmp(name, "auto") == 0) return backends[0];
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); ++i)
        if (strcmp(name, backends[i]->name) == 0) return backends[i];
    return NULL;
}
