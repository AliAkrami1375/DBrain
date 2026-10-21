/*
 * Tick timing and time-division transmission slots.
 *
 * Each tick is laid out as
 *
 *   |-- integration budget --|-- slot 0 --|-- slot 1 --| ... |-- slot S --|
 *
 * Slot 0 is a contention window for control traffic (beacons, heartbeats,
 * control frames, overflow bundles); slots 1..S belong to the members in
 * ascending node-id order. A member transmits its tick's spike bundle at the
 * start of its slot. CSMA remains underneath as a safety net, so a node
 * that misses its slot still sends, it just risks a collision.
 *
 * Slot length is the airtime of a bundle carrying slot_events events plus a
 * guard. The tick period is the integration budget plus S+1 slots, but never
 * shorter than the configured minimum. Every node computes the same plan
 * from the values the coordinator publishes in its beacon.
 */
#ifndef DBRAIN_SLOT_H
#define DBRAIN_SLOT_H

#include <stdint.h>

enum { PHY_RATE_1M = 0, PHY_RATE_6M = 1, PHY_RATE_24M = 2, PHY_RATE_54M = 3 };

#define SLOT_GUARD_US          100u    /* nodes with hardware receive timestamps */
#define SLOT_GUARD_SOFT_US     1500u   /* clusters containing a software-timestamp node */
#define SLOT_EVENTS_DEFAULT    32u
#define TICK_INTEGRATE_US      1000u   /* integration budget per tick */
#define TICK_PERIOD_MIN_US     1000u   /* one biological millisecond */
#define TICK_PERIOD_MAX_US     1000000u

typedef struct {
    uint32_t tick_period_us;
    uint32_t tx_window_us;   /* offset of slot 0 from the tick start */
    uint16_t slot_len_us;
    uint16_t slot_events;
    uint16_t slots;          /* S + 1 */
} slot_plan_t;

/* Airtime in microseconds of an ESP-NOW frame carrying payload bytes, including
 * preamble, MAC overhead, DIFS and the mean backoff. */
uint32_t slot_airtime_us(uint8_t phy_rate, uint16_t payload_bytes);

/* Bytes per microsecond would be lossy; this returns the bundle size for n events. */
static inline uint16_t slot_bundle_bytes(uint16_t events)
{
    return (uint16_t)(12u + 2u * events);
}

/* Builds the plan from cluster parameters. members excludes nobody: S = members. */
void slot_plan_build(slot_plan_t *p, uint16_t members, uint8_t phy_rate, uint16_t slot_events,
                     uint16_t guard_us, uint32_t min_period_us);

/* Offset of slot i within the tick. */
static inline uint32_t slot_offset_us(const slot_plan_t *p, uint16_t slot)
{
    return p->tx_window_us + (uint32_t)slot * p->slot_len_us;
}

/* Tick number for a cluster time, and the phase within it. */
static inline uint32_t tick_of(uint64_t cluster_us, uint64_t origin_us, uint32_t period_us)
{
    return cluster_us < origin_us ? 0u : (uint32_t)((cluster_us - origin_us) / period_us);
}

static inline uint32_t tick_phase_us(uint64_t cluster_us, uint64_t origin_us, uint32_t period_us)
{
    return cluster_us < origin_us ? 0u : (uint32_t)((cluster_us - origin_us) % period_us);
}

static inline uint64_t tick_start_us(uint32_t tick, uint64_t origin_us, uint32_t period_us)
{
    return origin_us + (uint64_t)tick * period_us;
}

#endif /* DBRAIN_SLOT_H */
