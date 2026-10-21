#include "unit.h"
#include "dbrain/slot.h"

int main(void)
{
    slot_plan_t p;
    uint32_t a;

    /* a full 250-byte broadcast at 1 Mbps occupies about 2.5-2.9 ms */
    a = slot_airtime_us(PHY_RATE_1M, 250);
    CHECK(a >= 2500 && a <= 2900);
    CHECK(slot_airtime_us(PHY_RATE_1M, 32) < 1200);
    CHECK(slot_airtime_us(PHY_RATE_24M, 250) < 400);
    CHECK(slot_airtime_us(PHY_RATE_54M, 250) < slot_airtime_us(PHY_RATE_24M, 250));
    CHECK(slot_airtime_us(PHY_RATE_6M, 250) < slot_airtime_us(PHY_RATE_1M, 250));
    CHECK_EQ(slot_bundle_bytes(119), 250);

    /* two boards at 1 Mbps: a 32-event bundle is about 1.5 ms of air, three slots, ~6x slowdown */
    slot_plan_build(&p, 2, PHY_RATE_1M, 32, SLOT_GUARD_US, 0);
    CHECK_EQ(p.slots, 3);
    CHECK_EQ(p.slot_events, 32);
    CHECK_EQ(p.slot_len_us, 1572);
    CHECK(p.tick_period_us >= 4000 && p.tick_period_us <= 6000);
    CHECK_EQ(p.tx_window_us, TICK_INTEGRATE_US);
    CHECK_EQ(slot_offset_us(&p, 0), TICK_INTEGRATE_US);
    CHECK_EQ(slot_offset_us(&p, 2), TICK_INTEGRATE_US + 2u * p.slot_len_us);
    CHECK(slot_offset_us(&p, 2) + p.slot_len_us <= p.tick_period_us);

    /* forty ESP32 boards at 24 Mbps stay inside a 20 ms tick */
    slot_plan_build(&p, 40, PHY_RATE_24M, 32, SLOT_GUARD_US, 0);
    CHECK_EQ(p.slots, 41);
    CHECK(p.tick_period_us <= 20000);
    CHECK(slot_offset_us(&p, 40) + p.slot_len_us <= p.tick_period_us);

    /* a software-timestamp node widens every slot */
    {
        slot_plan_t q;
        slot_plan_build(&q, 2, PHY_RATE_1M, 32, SLOT_GUARD_SOFT_US, 0);
        CHECK_EQ(q.slot_len_us, (uint16_t)(slot_airtime_us(PHY_RATE_1M, 76) + SLOT_GUARD_SOFT_US));
    }

    /* the minimum period wins when the airtime would allow faster */
    slot_plan_build(&p, 1, PHY_RATE_54M, 8, SLOT_GUARD_US, 50000);
    CHECK_EQ(p.tick_period_us, 50000);
    slot_plan_build(&p, 0, PHY_RATE_54M, 0, SLOT_GUARD_US, 0);
    CHECK_EQ(p.slot_events, SLOT_EVENTS_DEFAULT);
    CHECK_EQ(p.slots, 1);
    slot_plan_build(&p, 1, PHY_RATE_1M, 200, SLOT_GUARD_US, 0);
    CHECK_EQ(p.slot_events, 119);

    /* tick arithmetic */
    CHECK_EQ(tick_of(1000000, 1000000, 9700), 0);
    CHECK_EQ(tick_of(1009699, 1000000, 9700), 0);
    CHECK_EQ(tick_of(1009700, 1000000, 9700), 1);
    CHECK_EQ(tick_phase_us(1009701, 1000000, 9700), 1);
    CHECK_EQ(tick_of(5, 1000000, 9700), 0);              /* before the origin */
    CHECK_EQ(tick_start_us(3, 1000000, 9700), 1029100);
    UNIT_END();
}
