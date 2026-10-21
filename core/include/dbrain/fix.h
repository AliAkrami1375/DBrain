/*
 * s16.15 fixed-point arithmetic.
 *
 * Every neuron and synapse computation in DBrain uses this format so that a
 * neuron produces bit-identical state on every chip in the cluster and on the
 * host reference simulator. The layout is the ISO/IEC TR 18037 "accum" format
 * that SpiNNaker's production stack uses: one sign bit, 16 integer bits and
 * 15 fraction bits, range [-65536, 65536) with a resolution of 2^-15.
 *
 * Platform requirements, verified at runtime by fix_selftest():
 *   - two's-complement int32_t / int64_t,
 *   - arithmetic right shift of negative signed integers.
 * GCC for Xtensa (ESP32, ESP32-S2/S3, ESP8266), RISC-V (ESP32-C2/C3/C5/C6)
 * and every host compiler we build with satisfies both.
 *
 * Rounding: fix_mul rounds half up (adds 2^14 before the shift). Plain
 * arithmetic wraps on overflow like the underlying int32_t; the *_sat variants
 * saturate. The simulation core uses the saturating variants wherever an
 * input is not bounded by construction.
 */
#ifndef DBRAIN_FIX_H
#define DBRAIN_FIX_H

#include <stdint.h>
#include <stdbool.h>

typedef int32_t accum_t;

#define FIX_FRAC_BITS 15
#define FIX_ONE       ((accum_t)1 << FIX_FRAC_BITS)
#define FIX_HALF      ((accum_t)1 << (FIX_FRAC_BITS - 1))
#define FIX_MAX       ((accum_t)INT32_MAX)
#define FIX_MIN       ((accum_t)INT32_MIN)

/* Integer constant to accum. Valid for -65536 <= i < 65536. */
#define FIX_INT(i) ((accum_t)((int32_t)(i) * (int32_t)FIX_ONE))

/* Rational constant num/den to accum, nearest with ties away from zero. den > 0. */
#define FIX_RATIO_POS(num, den) \
    ((accum_t)(((int64_t)(num) * (int64_t)FIX_ONE * 2 + (int64_t)(den)) / (2 * (int64_t)(den))))
#define FIX_RATIO(num, den) \
    (((num) < 0) ? -FIX_RATIO_POS(-(num), den) : FIX_RATIO_POS(num, den))

static inline accum_t fix_from_int(int32_t i)
{
    return (accum_t)((uint32_t)i << FIX_FRAC_BITS);
}

/* Floor toward negative infinity. */
static inline int32_t fix_to_int(accum_t a)
{
    return a >> FIX_FRAC_BITS;
}

static inline accum_t fix_mul(accum_t a, accum_t b)
{
    int64_t p = (int64_t)a * (int64_t)b;
    p += (int64_t)1 << (FIX_FRAC_BITS - 1);
    return (accum_t)(uint32_t)(p >> FIX_FRAC_BITS);
}

static inline accum_t fix_clamp64(int64_t v)
{
    if (v > (int64_t)FIX_MAX) return FIX_MAX;
    if (v < (int64_t)FIX_MIN) return FIX_MIN;
    return (accum_t)v;
}

static inline accum_t fix_mul_sat(accum_t a, accum_t b)
{
    int64_t p = (int64_t)a * (int64_t)b;
    p += (int64_t)1 << (FIX_FRAC_BITS - 1);
    return fix_clamp64(p >> FIX_FRAC_BITS);
}

static inline accum_t fix_add_sat(accum_t a, accum_t b)
{
    return fix_clamp64((int64_t)a + (int64_t)b);
}

static inline accum_t fix_sub_sat(accum_t a, accum_t b)
{
    return fix_clamp64((int64_t)a - (int64_t)b);
}

static inline accum_t fix_clamp(accum_t a, accum_t lo, accum_t hi)
{
    if (a < lo) return lo;
    if (a > hi) return hi;
    return a;
}

/* Returns true when the platform meets the requirements listed above. */
bool fix_selftest(void);

/* Out-of-line copies of the inline helpers, for foreign-language bindings. */
accum_t fix_mul_fn(accum_t a, accum_t b);
accum_t fix_mul_sat_fn(accum_t a, accum_t b);

#endif /* DBRAIN_FIX_H */
