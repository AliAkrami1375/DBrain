#include "dbrain/fix.h"

bool fix_selftest(void)
{
    volatile int32_t m1 = -1;
    volatile int64_t m2 = -2;
    volatile int32_t big = INT32_MIN;

    if (sizeof(accum_t) != 4) return false;
    if ((uint32_t)m1 != 0xFFFFFFFFu) return false;          /* two's complement */
    if ((m1 >> 1) != -1) return false;                       /* arithmetic shift, 32-bit */
    if ((m2 >> 1) != -1) return false;                       /* arithmetic shift, 64-bit */
    if ((accum_t)(uint32_t)((int64_t)big - 1) != INT32_MAX) return false; /* wrap on narrowing */

    if (fix_mul(FIX_ONE, FIX_ONE) != FIX_ONE) return false;
    if (fix_mul(FIX_INT(3), FIX_RATIO(1, 2)) != FIX_RATIO(3, 2)) return false;
    if (fix_mul(FIX_INT(-3), FIX_RATIO(1, 2)) != -FIX_RATIO(3, 2)) return false;
    if (fix_add_sat(FIX_MAX, FIX_ONE) != FIX_MAX) return false;
    if (fix_sub_sat(FIX_MIN, FIX_ONE) != FIX_MIN) return false;
    if (fix_to_int(FIX_RATIO(-1, 2)) != -1) return false;    /* floor */
    return true;
}

accum_t fix_mul_fn(accum_t a, accum_t b)     { return fix_mul(a, b); }
accum_t fix_mul_sat_fn(accum_t a, accum_t b) { return fix_mul_sat(a, b); }
