#include "unit.h"
#include "dbrain/fix.h"

int main(void)
{
    CHECK(fix_selftest());
    CHECK_EQ(FIX_ONE, 32768);
    CHECK_EQ(FIX_RATIO(4, 100), 1311);          /* 0.04 rounds up from 1310.72 */
    CHECK_EQ(FIX_RATIO(1, 3), 10923);           /* 10922.67 */
    CHECK_EQ(FIX_RATIO(2, 100), 655);           /* 655.36 */
    CHECK_EQ(FIX_INT(-65), -65 * 32768);
    CHECK_EQ(fix_from_int(-1), -32768);
    CHECK_EQ(fix_to_int(FIX_INT(7) + 1), 7);
    CHECK_EQ(fix_to_int(-1), -1);

    /* rounding of products: half rounds up */
    CHECK_EQ(fix_mul(1, FIX_HALF), 1);          /* 0.5 * 2^-15 -> rounds to 2^-15 */
    CHECK_EQ(fix_mul(-1, FIX_HALF), 0);         /* -0.5 * 2^-15 -> rounds up to 0 */
    CHECK_EQ(fix_mul(FIX_INT(3), FIX_INT(-4)), FIX_INT(-12));
    CHECK_EQ(fix_mul(FIX_RATIO(1, 2), FIX_RATIO(1, 2)), FIX_RATIO(1, 4));

    /* wrap vs saturate */
    CHECK_EQ(fix_mul(FIX_INT(300), FIX_INT(300)), (accum_t)(uint32_t)((int64_t)90000 * 32768));
    CHECK_EQ(fix_mul_sat(FIX_INT(300), FIX_INT(300)), FIX_MAX);
    CHECK_EQ(fix_mul_sat(FIX_INT(-300), FIX_INT(300)), FIX_MIN);
    CHECK_EQ(fix_add_sat(FIX_MAX - 1, 1), FIX_MAX);
    CHECK_EQ(fix_add_sat(FIX_MAX, 1), FIX_MAX);
    CHECK_EQ(fix_sub_sat(FIX_MIN, 1), FIX_MIN);
    CHECK_EQ(fix_clamp(5, -2, 3), 3);
    CHECK_EQ(fix_clamp(-5, -2, 3), -2);
    CHECK_EQ(fix_clamp(1, -2, 3), 1);
    UNIT_END();
}
