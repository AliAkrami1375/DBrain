#include "unit.h"
#include "dbrain/membership.h"
#include "dbrain/frame.h"

static void mk_hb(heartbeat_t *h, uint8_t last_mac, uint16_t id, uint8_t chip, uint8_t flags)
{
    memset(h, 0, sizeof(*h));
    h->cluster_id = 42;
    h->node_id = id;
    h->mac[0] = 0x24; h->mac[1] = 0x6F; h->mac[2] = 0x28; h->mac[5] = last_mac;
    h->epoch = 3;
    h->coord_id = 1;
    h->shard_id = id;
    h->n_neurons = 100;
    h->cap.chip = chip;
    h->cap.cores = chip == DBRAIN_CHIP_ESP32 ? 2 : 1;
    h->cap.mhz = chip == DBRAIN_CHIP_ESP8266 ? 80 : 240;
    h->cap.free_heap = 200000;
    cap_classify(&h->cap);
    h->state = NODE_STATE_MEMBER;
    h->flags = flags;
}

/* Re-announces every active member except `except` so only it can time out. */
static void refresh_others(membership_t *t, uint16_t except, uint32_t now)
{
    unsigned i;
    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        member_t *m = &t->m[i];
        if (m->liveness == MEMBER_EMPTY || m->liveness == MEMBER_DEAD || m->node_id == except) continue;
        m->last_heard_ms = now;
        if (m->liveness == MEMBER_SUSPECT) m->liveness = MEMBER_ALIVE;
    }
}

int main(void)
{
    uint8_t buf[128];
    heartbeat_t h, h2;
    membership_t t;
    member_t *m;
    cap_t c;
    uint32_t now = 10000;
    size_t len;

    /* capability scoring is static and orders chips sensibly */
    memset(&c, 0, sizeof c);
    c.chip = DBRAIN_CHIP_ESP8266; c.cores = 1; c.mhz = 80; c.free_heap = 40000;
    cap_classify(&c);
    CHECK_EQ(c.tier, TIER_LEAF);
    CHECK(!(c.flags & CAP_FLAG_ELIGIBLE));
    CHECK(!(c.flags & CAP_FLAG_HW_RX_TS));
    {
        uint32_t leaf = cap_score(&c), mid, work, heavy;
        c.chip = DBRAIN_CHIP_ESP32C3; c.mhz = 160; cap_classify(&c); mid = cap_score(&c);
        CHECK_EQ(c.tier, TIER_MID);
        CHECK(c.flags & CAP_FLAG_ELIGIBLE);
        c.chip = DBRAIN_CHIP_ESP32; c.cores = 2; c.mhz = 240; cap_classify(&c); work = cap_score(&c);
        CHECK_EQ(c.tier, TIER_WORKHORSE);
        c.flags |= CAP_FLAG_PSRAM; c.psram = 4u << 20; cap_classify(&c); heavy = cap_score(&c);
        CHECK_EQ(c.tier, TIER_HEAVY);
        CHECK(leaf < mid && mid < work && work < heavy);
        c.free_heap = 1;
        CHECK_EQ(cap_score(&c), heavy);
    }
    c.chip = DBRAIN_CHIP_ESP8266; c.flags = 0; c.psram = 0; c.free_heap = 40000; cap_classify(&c);
    CHECK(cap_neuron_budget(&c, 50) >= 16 && cap_neuron_budget(&c, 50) < 200);
    c.chip = DBRAIN_CHIP_ESP32; c.free_heap = 250000; cap_classify(&c);
    CHECK(cap_neuron_budget(&c, 50) >= 400);
    CHECK(cap_neuron_budget(&c, 10) > cap_neuron_budget(&c, 50));
    c.flags |= CAP_FLAG_PSRAM; c.psram = 4u << 20; cap_classify(&c);
    CHECK_EQ(cap_neuron_budget(&c, 1000), 476);         /* 4.4 KB of rows per neuron in 2 MB of PSRAM */

    /* heartbeat round trip */
    mk_hb(&h, 0x11, 7, DBRAIN_CHIP_ESP32, HB_FLAG_HAS_SHARD);
    h.hb_seq = 99; h.tick = 123456; h.drops = 3; h.overruns = 1; h.sync_residual_us = -77;
    len = heartbeat_encode(buf, sizeof buf, &h);
    CHECK_EQ(len, HEARTBEAT_LEN);
    CHECK_EQ(frame_type(buf, len), FRAME_HEARTBEAT);
    CHECK(heartbeat_decode(buf, len, &h2));
    CHECK_EQ(memcmp(&h, &h2, sizeof h), 0);
    CHECK(!heartbeat_decode(buf, len - 1, &h2));
    wr16(buf + 4, 5000);
    CHECK(!heartbeat_decode(buf, len, &h2));             /* node id out of range */

    /* table: insert, update, find */
    membership_init(&t);
    mk_hb(&h, 0x01, 1, DBRAIN_CHIP_ESP32, HB_FLAG_COORDINATOR);
    h.state = NODE_STATE_COORDINATOR;
    m = membership_update(&t, &h, now);
    CHECK(m != NULL);
    CHECK_EQ(t.count, 1);
    CHECK_EQ(t.version, 1);
    mk_hb(&h, 0x02, 2, DBRAIN_CHIP_ESP8266, 0);
    CHECK(membership_update(&t, &h, now) != NULL);
    mk_hb(&h, 0x05, 5, DBRAIN_CHIP_ESP32C3, 0);
    CHECK(membership_update(&t, &h, now) != NULL);
    CHECK_EQ(t.count, 3);
    CHECK_EQ(membership_active(&t), 3);
    CHECK(membership_find_id(&t, 5) != NULL);
    CHECK(membership_find_id(&t, 3) == NULL);
    CHECK(membership_find_mac(&t, h.mac) == membership_find_id(&t, 5));
    CHECK_EQ(membership_rank(&t, 1), 0);
    CHECK_EQ(membership_rank(&t, 2), 1);
    CHECK_EQ(membership_rank(&t, 5), 2);
    CHECK_EQ(membership_rank(&t, 4), -1);
    CHECK_EQ(membership_free_id(&t), 3);
    CHECK_EQ(membership_max_epoch(&t), 3);
    {
        uint32_t v = t.version;
        h.hb_seq++;
        membership_update(&t, &h, now + 500);              /* nothing changed */
        CHECK_EQ(t.version, v);
        h.shard_version = 9;
        membership_update(&t, &h, now + 600);
        CHECK_EQ(t.version, v + 1);
    }

    /* best coordinator: only entries claiming the role and eligible */
    m = membership_best_coordinator(&t);
    CHECK(m != NULL && m->node_id == 1);
    mk_hb(&h, 0x09, 9, DBRAIN_CHIP_ESP32S3, HB_FLAG_COORDINATOR);
    h.epoch = 2;                                             /* older epoch loses */
    membership_update(&t, &h, now);
    m = membership_best_coordinator(&t);
    CHECK(m != NULL && m->node_id == 1);
    h.epoch = 3;                                             /* same epoch: S3 with more MHz? equal; MAC decides */
    h.cap.flags |= CAP_FLAG_PSRAM; h.cap.psram = 8u << 20; cap_classify(&h.cap);
    membership_update(&t, &h, now);
    m = membership_best_coordinator(&t);
    CHECK(m != NULL && m->node_id == 9);
    mk_hb(&h, 0x0A, 10, DBRAIN_CHIP_ESP8266, HB_FLAG_COORDINATOR);  /* leaf cannot lead */
    h.epoch = 50;
    membership_update(&t, &h, now);
    m = membership_best_coordinator(&t);
    CHECK(m != NULL && m->node_id == 9);

    /* election compare */
    {
        uint8_t a[6] = {1,2,3,4,5,6}, b[6] = {1,2,3,4,5,7};
        CHECK(election_compare(2, 1, a, 1, 9, b) > 0);
        CHECK(election_compare(1, 9, a, 2, 1, b) < 0);
        CHECK(election_compare(1, 5, a, 1, 9, b) < 0);
        CHECK(election_compare(1, 5, b, 1, 5, a) > 0);
        CHECK_EQ(election_compare(1, 5, a, 1, 5, a), 0);
    }

    /* failure detection: node 1 falls silent while the others keep heartbeating.
     * silent 3 s -> suspect, 2 s more -> dead, 30 s -> forgotten */
    refresh_others(&t, 1, now + 2999);
    CHECK(!membership_tick(&t, now + 2999));
    CHECK(membership_tick(&t, now + 3000));
    m = membership_find_id(&t, 1);
    CHECK(m != NULL && m->liveness == MEMBER_SUSPECT && m->probes == 0);
    CHECK_EQ(membership_active(&t), 5);                      /* suspects still count */
    membership_confirm(&t, m, now + 3500);
    CHECK_EQ(m->liveness, MEMBER_ALIVE);
    refresh_others(&t, 1, now + 6500);
    CHECK(membership_tick(&t, now + 6500));                  /* suspect again at +3 s */
    CHECK_EQ(m->liveness, MEMBER_SUSPECT);
    refresh_others(&t, 1, now + 8499);
    CHECK(!membership_tick(&t, now + 8499));
    CHECK(membership_tick(&t, now + 8500));                  /* probe timeout */
    CHECK_EQ(m->liveness, MEMBER_DEAD);
    CHECK(membership_find_id(&t, 1) == NULL);
    CHECK_EQ(membership_free_id(&t), 3);                     /* dead ids are not reused yet */
    CHECK_EQ(membership_rank(&t, 2), 0);
    CHECK_EQ(t.count, 5);
    refresh_others(&t, 1, now + 38500);
    membership_tick(&t, now + 38500);
    CHECK_EQ(t.count, 4);
    CHECK_EQ(membership_free_id(&t), 1);
    mk_hb(&h, 0x01, 1, DBRAIN_CHIP_ESP32, HB_FLAG_COORDINATOR);     /* it comes back */
    CHECK(membership_update(&t, &h, now + 40000) != NULL);
    CHECK_EQ(t.count, 5);
    refresh_others(&t, 0, now + 40000);

    /* declare dead directly (failed probe cycle) */
    m = membership_find_id(&t, 2);
    membership_declare_dead(&t, m, now + 40000);
    CHECK_EQ(m->liveness, MEMBER_DEAD);
    CHECK_EQ(membership_active(&t), 4);

    /* table full */
    {
        unsigned i;
        membership_init(&t);
        for (i = 0; i < MEMBERSHIP_MAX; i++) {
            mk_hb(&h, (uint8_t)i, (uint16_t)(i + 1), DBRAIN_CHIP_ESP32, 0);
            h.mac[4] = (uint8_t)(i >> 8);
            CHECK(membership_update(&t, &h, now) != NULL);
        }
        mk_hb(&h, 0xFF, 1000, DBRAIN_CHIP_ESP32, 0);
        h.mac[3] = 1;
        CHECK(membership_update(&t, &h, now) == NULL);
    }
    UNIT_END();
}
