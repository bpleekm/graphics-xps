/* Portable 16x16 output tile, 8x8 work-items, four accumulators/work-item.
 * Types and optional extensions are selected by the OpenCL backend.
 * No vendor identifiers or host platform assumptions belong in this kernel.
 */
#if GX_HALF
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#endif
#if GX_DOUBLE
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#endif
#if GX_DOT
#pragma OPENCL EXTENSION cl_khr_integer_dot_product : enable
#endif

#if GX_FLOAT
#define GX_MAC(a, b, c) fma((GX_ACC)(a), (GX_ACC)(b), (c))
#else
#define GX_MAC(a, b, c) ((c) + (GX_ACC)(a) * (GX_ACC)(b))
#endif

__kernel void gx_gemm(__global const GX_IN *a, __global const GX_IN *b,
                      __global GX_ACC *c, uint n, uint row_start, uint rows)
{
    __local GX_IN tile_a[16][16];
    __local GX_IN tile_b[16][16];
    const uint x = get_local_id(0), y = get_local_id(1);
    const uint linear = y * 8 + x;
    const uint column = get_group_id(0) * 16 + x * 2;
    const uint batch_row = get_group_id(1) * 16 + y * 2;
    const uint row = row_start + batch_row;
    const uint group_row = row_start + get_group_id(1) * 16;
    const uint group_column = get_group_id(0) * 16;
    GX_ACC c00 = 0, c01 = 0, c10 = 0, c11 = 0;

    for (uint base = 0; base < n; base += 16) {
        for (uint element = linear; element < 256; element += 64) {
            const uint r = element / 16, k = element % 16;
            const uint ar = group_row + r;
            tile_a[r][k] = ar < n && ar < row_start + rows && base + k < n
                ? a[(size_t)ar * n + base + k] : (GX_IN)0;
            tile_b[r][k] = base + r < n && group_column + k < n
                ? b[(size_t)(base + r) * n + group_column + k] : (GX_IN)0;
        }
        barrier(CLK_LOCAL_MEM_FENCE);

#if GX_DOT
        for (uint k = 0; k < 16; k += 4) {
            char4 a0 = vload4(0, &tile_a[y * 2][k]);
            char4 a1 = vload4(0, &tile_a[y * 2 + 1][k]);
            char4 b0 = (char4)(tile_b[k][x * 2], tile_b[k + 1][x * 2],
                               tile_b[k + 2][x * 2], tile_b[k + 3][x * 2]);
            char4 b1 = (char4)(tile_b[k][x * 2 + 1], tile_b[k + 1][x * 2 + 1],
                               tile_b[k + 2][x * 2 + 1], tile_b[k + 3][x * 2 + 1]);
            c00 += dot(a0, b0);
            c01 += dot(a0, b1);
            c10 += dot(a1, b0);
            c11 += dot(a1, b1);
        }
#else
        for (uint k = 0; k < 16; ++k) {
            GX_IN a0 = tile_a[y * 2][k], a1 = tile_a[y * 2 + 1][k];
            GX_IN b0 = tile_b[k][x * 2], b1 = tile_b[k][x * 2 + 1];
            c00 = GX_MAC(a0, b0, c00);
            c01 = GX_MAC(a0, b1, c01);
            c10 = GX_MAC(a1, b0, c10);
            c11 = GX_MAC(a1, b1, c11);
        }
#endif
        barrier(CLK_LOCAL_MEM_FENCE);
    }

    if (batch_row < rows && row < n) {
        if (column < n) c[(size_t)row * n + column] = c00;
        if (column + 1 < n) c[(size_t)row * n + column + 1] = c01;
    }
    if (batch_row + 1 < rows && row + 1 < n) {
        if (column < n) c[(size_t)(row + 1) * n + column] = c10;
        if (column + 1 < n) c[(size_t)(row + 1) * n + column + 1] = c11;
    }
}
