#include "unit.h"
#include "dbrain/timesync.h"
#include "dbrain/frame.h"
#include "dbrain/rng.h"

/* Coordinator clock: offset + local * (1 + ppm/1e6). */
static uint64_t coord_clock(uint64_t local_us, int64_t offset_us, int32_t ppm)
{
    return (uint64_t)((int64_t)local_us + offset_us + ((int64_t)local_us * ppm) / 1000000);
}

static int64_t i64abs(int64_t v) { return v < 0 ? -v : v; }

/* Runs beacons every 1 s with symmetric RX jitter and returns the worst error after warm-up. */
static int64_t run(int32_t ppm, int64_t offset, uint32_t jitter_us, int beacons, int64_t *worst_mid)
{
    timesync_t s;
    rng_t r;
    int64_t worst = 0, est;
    int i;
    uint64_t local = 5000000;

    timesync_init(&s);
    rng_seed(&r, 1, 2, (uint32_t)ppm);
    *worst_mid = 0;
    for (i = 0; i < beacons; i++) {
        uint64_t true_coord = coord_clock(local, offset, ppm);
        uint64_t rx = local + rng_below(&r, 2 * jitter_us + 1) - jitter_us;   /* observed arrival */
        timesync_add(&s, rx, true_coord);
        if (i >= 8) {
            /* evaluate halfway to the next beacon, where extrapolation matters */
            uint64_t mid = local + 500000;
            timesync_estimate(&s, mid, &est);
            {
                int64_t e = i64abs(est - ((int64_t)coord_clock(mid, offset, ppm) - (int64_t)mid));
                if (e > worst) worst = e;
                if (e > *worst_mid) *worst_mid = e;
            }
        }
        local += 1000000;
    }
    return worst;
}

int main(void)
{
    timesync_t s;
    int64_t est, worst_mid;
    beacon_t b, b2;
    uint8_t buf[80];
    size_t len;
    uint64_t ct;

    /* no samples: nothing to estimate */
    timesync_init(&s);
    CHECK(!timesync_estimate(&s, 123, &est));
    CHECK(!timesync_cluster_time(&s, 123, &ct));

    /* a single sample is a constant offset */
    CHECK_EQ(timesync_add(&s, 1000000, 1500000), 0);
    CHECK(timesync_estimate(&s, 2000000, &est));
    CHECK_EQ(est, 500000);

    /* exact clocks: regression recovers offset and skew perfectly */
    timesync_init(&s);
    {
        int i;
        for (i = 0; i < 8; i++) {
            uint64_t l = 1000000u + (uint64_t)i * 1000000u;
            timesync_add(&s, l, coord_clock(l, 250000, 10));
        }
        CHECK(timesync_estimate(&s, 9500000, &est));
        CHECK(i64abs(est - ((int64_t)coord_clock(9500000, 250000, 10) - 9500000)) <= 1);
        CHECK(i64abs(s.last_residual_us) <= 1);
    }

    /* 10 ppm skew, 80 us receiver spread: error stays well inside 100 us */
    CHECK(run(10, 123456789, 40, 60, &worst_mid) < 100);
    CHECK(run(-20, -987654321, 40, 60, &worst_mid) < 100);
    /* software timestamps (ESP8266 class): millisecond jitter gives millisecond error */
    CHECK(run(10, 1000, 1000, 60, &worst_mid) < 1500);
    CHECK(run(0, 0, 0, 20, &worst_mid) <= 1);

    /* a coordinator restart (offset jump) resets the filter instead of poisoning it */
    timesync_init(&s);
    timesync_add(&s, 1000000, 2000000);
    timesync_add(&s, 2000000, 3000000);
    timesync_add(&s, 3000000, 4000000);
    CHECK_EQ(s.n, 3);
    timesync_add(&s, 4000000, 9000000);      /* +4 s jump */
    CHECK_EQ(s.n, 1);
    CHECK(timesync_estimate(&s, 4500000, &est));
    CHECK_EQ(est, 5000000);

    /* samples older than the span are dropped; time going backwards resets */
    timesync_init(&s);
    timesync_add(&s, 1000000, 1000000);
    timesync_add(&s, 2000000, 2000000);
    timesync_add(&s, 62000000, 62000000);   /* 61 s after the first: only it is dropped */
    CHECK_EQ(s.n, 2);
    timesync_add(&s, 60000000, 60000000);
    CHECK_EQ(s.n, 1);

    /* slew: first application steps, later ones move by at most max_step */
    timesync_init(&s);
    timesync_add(&s, 1000000, 1100000);
    CHECK(timesync_slew(&s, 1000000, 10, 5000));
    CHECK_EQ(s.applied_offset, 100000);
    timesync_add(&s, 2000000, 2100050);
    CHECK(!timesync_slew(&s, 2000000, 10, 5000));
    CHECK_EQ(s.applied_offset, 100010);
    CHECK(timesync_cluster_time(&s, 2000000, &ct));
    CHECK_EQ(ct, 2100010);
    timesync_add(&s, 3000000, 3200000);      /* 100 ms jump: beyond the step threshold */
    CHECK(timesync_slew(&s, 3000000, 10, 5000));
    CHECK(i64abs(s.applied_offset - 200000) < 60000);

    /* beacon round trip */
    memset(&b, 0, sizeof b);
    b.cluster_id = 42; b.epoch = 7; b.coord_id = 1; b.coord_mac[5] = 9; b.seq = 300;
    b.tx_latency_us = 120; b.member_count = 5; b.prev_tx_time_us = 0x123456789ABCDEF0ull;
    b.tick_origin_us = 99999999999ull; b.tick_period_us = 9700; b.tick_now = 4242;
    b.slot_version = 17; b.slot_len_us = 2900; b.slot_events = 32; b.phy_rate = 2; b.flags = 2;
    b.coord_score = 0xAABBCCDDu; b.prev_tx_seq = 299;
    len = beacon_encode(buf, sizeof buf, &b);
    CHECK_EQ(len, BEACON_LEN);
    CHECK_EQ(frame_type(buf, len), FRAME_BEACON);
    CHECK(beacon_decode(buf, len, &b2));
    CHECK_EQ(memcmp(&b, &b2, sizeof b), 0);
    CHECK(!beacon_decode(buf, len - 1, &b2));
    wr32(buf + 36, 0);
    CHECK(!beacon_decode(buf, len, &b2));
    CHECK_EQ(b2.coord_score, 0xAABBCCDDu);
    UNIT_END();
}
