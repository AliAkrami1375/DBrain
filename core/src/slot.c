#include "dbrain/slot.h"

/*
 * 802.11 timing used for the airtime model. DSSS at 1 Mbps: 192 us long
 * preamble, 50 us DIFS, 20 us slot time with a mean backoff of 15.5 slots.
 * ERP-OFDM: 20 us preamble plus 4 us SIGNAL, 28 us DIFS (short slot), 9 us
 * slot time. The ESP-NOW action frame adds 39 bytes around the payload.
 */
uint32_t slot_airtime_us(uint8_t phy_rate, uint16_t payload_bytes)
{
    uint32_t bits = (39u + (uint32_t)payload_bytes) * 8u;

    switch (phy_rate) {
    case PHY_RATE_1M:  return 192u + bits + 50u + 310u;
    case PHY_RATE_6M:  return 24u + (bits + 5u) / 6u + 28u + 140u;
    case PHY_RATE_24M: return 24u + (bits + 23u) / 24u + 28u + 140u;
    case PHY_RATE_54M: return 24u + (bits + 53u) / 54u + 28u + 140u;
    default:           return 192u + bits + 50u + 310u;
    }
}

void slot_plan_build(slot_plan_t *p, uint16_t members, uint8_t phy_rate, uint16_t slot_events,
                     uint16_t guard_us, uint32_t min_period_us)
{
    uint32_t period;

    if (slot_events == 0) slot_events = SLOT_EVENTS_DEFAULT;
    if (slot_events > 119u) slot_events = 119u;
    p->slot_events = slot_events;
    p->slot_len_us = (uint16_t)(slot_airtime_us(phy_rate, slot_bundle_bytes(slot_events)) + guard_us);
    p->slots = (uint16_t)(members + 1u);
    p->tx_window_us = TICK_INTEGRATE_US;
    period = p->tx_window_us + (uint32_t)p->slots * p->slot_len_us;
    if (period < min_period_us) period = min_period_us;
    if (period < TICK_PERIOD_MIN_US) period = TICK_PERIOD_MIN_US;
    if (period > TICK_PERIOD_MAX_US) period = TICK_PERIOD_MAX_US;
    p->tick_period_us = period;
}
