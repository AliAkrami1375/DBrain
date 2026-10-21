#include "unit.h"
#include "dbrain/shard.h"
#include "dbrain/synapse.h"

static uint8_t img[16384] __attribute__((aligned(4)));
static uint8_t arena[65536] __attribute__((aligned(4)));

/* shard 1: 4 RS neurons; 0 -> 1 (delay 3, w 20.0), 0 -> 2 (delay 1, w -5.0);
 * remote shard 2 neuron 9 -> 3 (delay 2, w 7.5) */
static int32_t build(void)
{
    artifact_builder_t b;
    neuron_params_t p;

    CHECK_EQ(artifact_build_begin(&b, img, sizeof img, 1, 1), 0);
    neuron_params_izh(&p, FIX_RATIO(2, 100), FIX_RATIO(2, 10), FIX_INT(-65), FIX_INT(8), 0);
    CHECK_EQ(artifact_build_population(&b, &p, 4), 0);
    CHECK_EQ(artifact_build_row_begin(&b, gid_make(1, 0), 8), 0);
    CHECK_EQ(artifact_build_synapse(&b, 1, 20 * 128, 3, 0), 0);
    CHECK_EQ(artifact_build_synapse(&b, 2, -5 * 128, 1, 0), 0);
    CHECK_EQ(artifact_build_row_end(&b), 0);
    CHECK_EQ(artifact_build_row_begin(&b, gid_make(2, 9), 8), 0);
    CHECK_EQ(artifact_build_synapse(&b, 3, 15 * 64, 2, 0), 0);    /* 7.5 */
    CHECK_EQ(artifact_build_row_end(&b), 0);
    return artifact_build_finish(&b);
}

static accum_t bucket(const shard_t *sh, uint32_t tick, uint16_t neuron)
{
    return sh->buckets[(tick & (SHARD_DELAY_SLOTS - 1u)) * sh->art.n_neurons + neuron];
}

int main(void)
{
    shard_t sh;
    telemetry_t tm;
    int32_t size = build();
    uint16_t ev;
    uint32_t t, fire_tick = 0;

    CHECK(size > 0);
    CHECK_EQ(shard_arena_size(4), 32 + 256 + 8);
    telemetry_reset(&tm);
    CHECK_EQ(shard_bind(&sh, img, (uint32_t)size, arena, 10, &tm), ARTIFACT_E_NOSPACE);
    CHECK_EQ(shard_bind(&sh, img, (uint32_t)size, arena + 1, sizeof arena - 1, &tm), ARTIFACT_E_ALIGN);
    CHECK_EQ(shard_bind(&sh, img, (uint32_t)size, arena, sizeof arena, &tm), ARTIFACT_OK);
    CHECK_EQ(sh.tick, 0);
    CHECK(shard_subscribes(&sh, 1));
    CHECK(shard_subscribes(&sh, 2));
    CHECK(!shard_subscribes(&sh, 3));

    /* nothing fires at rest */
    for (t = 0; t < 50; t++) CHECK_EQ(shard_tick(&sh), 0);
    CHECK_EQ(sh.tick, 50);

    /* a strong injected current makes neuron 0 fire; its local synapses land at +delay */
    CHECK_EQ(shard_inject(&sh, 0, FIX_INT(200), 50), 0);
    CHECK_EQ(shard_inject(&sh, 0, FIX_INT(200), 49), -2);       /* already integrated */
    CHECK_EQ(shard_inject(&sh, 0, FIX_INT(200), 66), -2);       /* beyond the ring */
    CHECK_EQ(shard_inject(&sh, 4, FIX_INT(200), 50), -1);
    for (t = 50; t < 60; t++) {
        uint16_t n = shard_tick(&sh);
        if (n) { fire_tick = t; CHECK_EQ(sh.fired[0], 0); break; }
    }
    CHECK(fire_tick >= 50 && fire_tick < 55);
    CHECK_EQ(bucket(&sh, fire_tick + 3, 1), FIX_INT(20));
    CHECK_EQ(bucket(&sh, fire_tick + 1, 2), FIX_INT(-5));
    CHECK_EQ(bucket(&sh, fire_tick + 2, 1), 0);
    CHECK_EQ(tm.events_applied, 2);

    /* the bucket is consumed exactly at its tick: neuron 1 sees 20.0 once, then 0 */
    for (t = sh.tick; t < fire_tick + 3; t++) shard_tick(&sh);
    CHECK_EQ(sh.tick, fire_tick + 3);
    CHECK_EQ(bucket(&sh, fire_tick + 3, 1), FIX_INT(20));
    shard_tick(&sh);
    CHECK_EQ(bucket(&sh, fire_tick + 3, 1), 0);

    /* remote delivery: shard 2 neuron 9 fired in tick S; due at S + 2 */
    ev = 9;
    t = sh.tick;
    shard_deliver(&sh, 2, t, &ev, 1);                 /* sender one tick behind us */
    CHECK_EQ(bucket(&sh, t + 2, 3), FIX_RATIO(15, 2));
    shard_deliver(&sh, 2, t - 1, &ev, 1);             /* sender in step */
    CHECK_EQ(bucket(&sh, t + 1, 3), FIX_RATIO(15, 2));
    shard_deliver(&sh, 2, t - 2, &ev, 1);             /* sender two behind: due now, fine */
    CHECK_EQ(bucket(&sh, t, 3), FIX_RATIO(15, 2));
    CHECK_EQ(tm.events_late, 0);
    shard_deliver(&sh, 2, t - 3, &ev, 1);             /* due tick already integrated */
    CHECK_EQ(tm.events_late, 1);
    shard_deliver(&sh, 2, t + 14, &ev, 1);            /* due t+16: would alias the live bucket */
    CHECK_EQ(tm.events_early, 1);
    shard_deliver(&sh, 2, t + 13, &ev, 1);            /* due t+15: last slot of the ring */
    CHECK_EQ(bucket(&sh, t + 15, 3), FIX_RATIO(15, 2));
    ev = 8;
    shard_deliver(&sh, 2, t, &ev, 1);
    CHECK_EQ(tm.events_norow, 1);
    CHECK_EQ(tm.events_applied, 2 + 4);

    /* bucket accumulation saturates instead of wrapping */
    for (t = 0; t < 100000; t++) shard_inject(&sh, 3, FIX_MAX, sh.tick + 5);
    CHECK_EQ(bucket(&sh, sh.tick + 5, 3), FIX_MAX);

    /* reset returns to the initial state */
    shard_reset(&sh);
    CHECK_EQ(sh.tick, 0);
    CHECK_EQ(sh.state[0].v, FIX_INT(-65));
    CHECK_EQ(bucket(&sh, 5, 3), 0);
    UNIT_END();
}
