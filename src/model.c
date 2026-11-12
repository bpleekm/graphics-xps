#include "gxps.h"

#include <math.h>
#include <string.h>

static const gx_precision_info precisions[GX_PRECISION_COUNT] = {
    {"fp16", "FP16 x FP16 -> FP16 (fma)", 2, 2, true},
    {"fp32", "FP32 x FP32 -> FP32 (fma)", 4, 4, true},
    {"fp64", "FP64 x FP64 -> FP64 (fma)", 8, 8, true},
    {"int8", "INT8 x INT8 -> INT32", 1, 4, false},
    {"int16", "INT16 x INT16 -> INT32", 2, 4, false},
    {"int32", "INT32 x INT32 -> INT32", 4, 4, false},
    {"int64", "INT64 x INT64 -> INT64", 8, 8, false},
};

const gx_precision_info *gx_precision_get(gx_precision precision)
{
    return &precisions[precision];
}

uint16_t gx_float_to_half(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint16_t sign = (uint16_t)((bits >> 16) & 0x8000u);
    uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t fraction = bits & 0x7fffffu;
    if (exponent == 255) return (uint16_t)(sign | 0x7c00u | (fraction ? 0x0200u : 0));
    int shifted = (int)exponent - 127 + 15;
    if (shifted >= 31) return (uint16_t)(sign | 0x7c00u);
    if (shifted <= 0) {
        if (shifted < -10) return sign;
        fraction |= 0x800000u;
        unsigned shift = (unsigned)(14 - shifted);
        uint32_t rounded = fraction >> shift;
        uint32_t remainder = fraction & ((1u << shift) - 1u);
        uint32_t half = 1u << (shift - 1);
        if (remainder > half || (remainder == half && (rounded & 1u))) ++rounded;
        return (uint16_t)(sign | rounded);
    }
    uint32_t rounded = fraction >> 13;
    uint32_t remainder = fraction & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (rounded & 1u))) ++rounded;
    if (rounded == 0x400u) { rounded = 0; ++shifted; }
    if (shifted >= 31) return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | ((unsigned)shifted << 10) | rounded);
}

float gx_half_to_float(uint16_t value)
{
    uint32_t sign = (uint32_t)(value & 0x8000u) << 16;
    unsigned exponent = (value >> 10) & 31u;
    uint32_t fraction = value & 0x3ffu;
    uint32_t bits;
    if (!exponent) {
        if (!fraction) bits = sign;
        else {
            int unbiased = -14;
            while (!(fraction & 0x400u)) { fraction <<= 1; --unbiased; }
            fraction &= 0x3ffu;
            bits = sign | (uint32_t)(unbiased + 127) << 23 | fraction << 13;
        }
    } else if (exponent == 31) bits = sign | 0x7f800000u | fraction << 13;
    else bits = sign | (exponent + 112u) << 23 | fraction << 13;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

double gx_input_value(gx_precision precision, uint32_t row, uint32_t column,
                      uint32_t seed, bool matrix_b)
{
    uint32_t x = row * 0x9e3779b9u ^ column * 0x85ebca6bu ^ seed ^
                 (matrix_b ? 0xc2b2ae35u : 0x27d4eb2fu);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    int value = (int)(x % 7u) - 3;
    return gx_precision_get(precision)->floating ? value / 16.0 : value;
}

void gx_store_input(gx_precision precision, void *destination, size_t index, double value)
{
    switch (precision) {
    case GX_FP16: ((uint16_t *)destination)[index] = gx_float_to_half((float)value); break;
    case GX_FP32: ((float *)destination)[index] = (float)value; break;
    case GX_FP64: ((double *)destination)[index] = value; break;
    case GX_INT8: ((int8_t *)destination)[index] = (int8_t)value; break;
    case GX_INT16: ((int16_t *)destination)[index] = (int16_t)value; break;
    case GX_INT32: ((int32_t *)destination)[index] = (int32_t)value; break;
    case GX_INT64: ((int64_t *)destination)[index] = (int64_t)value; break;
    default: break;
    }
}

double gx_load_accumulator(gx_precision precision, const void *source, size_t index)
{
    const unsigned char *bytes = source;
    switch (precision) {
    case GX_FP16: { uint16_t value; memcpy(&value, bytes + index * 2, 2); return gx_half_to_float(value); }
    case GX_FP32: { float value; memcpy(&value, bytes + index * 4, 4); return value; }
    case GX_FP64: { double value; memcpy(&value, bytes + index * 8, 8); return value; }
    case GX_INT8: case GX_INT16: case GX_INT32: {
        int32_t value; memcpy(&value, bytes + index * 4, 4); return value;
    }
    case GX_INT64: { int64_t value; memcpy(&value, bytes + index * 8, 8); return (double)value; }
    default: return NAN;
    }
}

double gx_reference(gx_precision precision, uint32_t n, uint32_t row, uint32_t column,
                    uint32_t seed)
{
    float single = 0;
    double wide = 0;
    int64_t integer = 0;
    for (uint32_t k = 0; k < n; ++k) {
        double a = gx_input_value(precision, row, k, seed, false);
        double b = gx_input_value(precision, k, column, seed, true);
        switch (precision) {
        case GX_FP16:
            single = gx_half_to_float(gx_float_to_half(fmaf((float)a, (float)b, single)));
            break;
        case GX_FP32: single = fmaf((float)a, (float)b, single); break;
        case GX_FP64: wide = fma(a, b, wide); break;
        default: integer += (int64_t)a * (int64_t)b; break;
        }
    }
    if (precision == GX_FP16 || precision == GX_FP32) return single;
    return precision == GX_FP64 ? wide : (double)integer;
}

bool gx_compare(gx_precision precision, double expected, double actual,
                double *absolute, double *relative)
{
    *absolute = fabs(actual - expected);
    *relative = *absolute / fmax(fabs(expected), 1e-12);
    if (!isfinite(actual) || !isfinite(expected)) return false;
    if (!gx_precision_get(precision)->floating) return actual == expected;
    double atol = precision == GX_FP16 ? 1e-3 : precision == GX_FP32 ? 1e-5 : 1e-11;
    double rtol = precision == GX_FP16 ? 2e-3 : precision == GX_FP32 ? 5e-6 : 1e-12;
    return *absolute <= atol + rtol * fabs(expected);
}
