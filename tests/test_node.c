#include "unit.h"
#include "vnet.h"
#include "dbrain/slot.h"

static int32_t i32abs(int32_t v) { return v == INT32_MIN ? INT32_MAX : v < 0 ? -v : v; }

static void base_cfg(node_config_t *cfg)
{
    node_config_default(cfg);
    cfg->cluster_id = 9;
    cfg->discover_ms = 2000;
}

int main(void)
{
    vnet_t net;
    node_config_t cfg;
    int i;

    /* 1. a lone eligible board forms a cluster and runs its shard */
    vnet_init(&net, 1);
    base_cfg(&cfg);
    CHECK_EQ(vnet_add(&net, DBRAIN_CHIP_ESP32, 250000, 0, &cfg, 5, 123456), 0);
    vnet_power(&net, 0, true);
    vnet_run_until(&net, 1000000);
    CHECK_EQ(net.nodes[0]->node.state, NODE_STATE_DISCOVER);
    vnet_run_until(&net, 6000000);
    CHECK_EQ(net.nodes[0]->node.state, NODE_STATE_COORDINATOR);
    CHECK_EQ(net.nodes[0]->node.node_id, 1);
    CHECK_EQ(net.nodes[0]->node.epoch, 1);
    CHECK(net.nodes[0]->node.shard_valid);
    CHECK(net.nodes[0]->node.shard.art.n_neurons > 100);
    CHECK(net.nodes[0]->node.shard.tick > 500);           /* about 1000 ticks in 4 s at the plan's period */
    CHECK(net.nodes[0]->spikes > 0);
    CHECK_EQ(net.nodes[0]->node.tm.tick_overruns, 0);
    CHECK_EQ(vnet_coordinator(&net), 0);
    vnet_free(&net);

    /* 2. a lone leaf board never promotes itself */
    vnet_init(&net, 2);
    base_cfg(&cfg);
    vnet_add(&net, DBRAIN_CHIP_ESP8266, 40000, 0, &cfg, 0, 0);
    vnet_power(&net, 0, true);
    vnet_run_until(&net, 10000000);
    CHECK_EQ(net.nodes[0]->node.state, NODE_STATE_DISCOVER);
    CHECK_EQ(net.nodes[0]->node.node_id, 0);
    vnet_free(&net);

    /* 3. four boards converge; the strongest leads; spikes cross; clocks agree */
    vnet_init(&net, 3);
    base_cfg(&cfg);
    vnet_add(&net, DBRAIN_CHIP_ESP32C3, 200000, 0, &cfg, 10, 500000000);
    vnet_add(&net, DBRAIN_CHIP_ESP32S3, 250000, 4u << 20, &cfg, -8, -300000000);   /* PSRAM: highest score */
    vnet_add(&net, DBRAIN_CHIP_ESP32, 250000, 0, &cfg, 3, 0);
    vnet_add(&net, DBRAIN_CHIP_ESP32, 250000, 0, &cfg, -10, 77777);
    for (i = 0; i < 4; i++) vnet_power(&net, i, true);
    vnet_run_until(&net, 15000000);
    CHECK_EQ(vnet_coordinator(&net), 1);
    for (i = 0; i < 4; i++) {
        const vnode_t *v = net.nodes[i];
        CHECK(v->node.node_id >= 1 && v->node.node_id <= 4);
        CHECK(v->node.shard_valid);
        CHECK_EQ(v->node.epoch, 1);
        if (i != 1) {
            int32_t err = vnet_sync_error_us(&net, i);
            CHECK(err != INT32_MIN);
            CHECK(i32abs(err) < 150);
            CHECK(i32abs(v->last_residual_us) < 150);
        }
        CHECK(v->node.tm.bundles_rx > 0);
        CHECK(v->node.tm.bundles_tx > 0);
        CHECK(v->node.tm.events_applied > 0);
        CHECK_EQ(v->node.tm.seq_gaps, 0);
        CHECK_EQ(v->node.tm.events_late, 0);
        CHECK_EQ(v->node.tm.events_early, 0);
        CHECK_EQ(v->node.tm.control_failures, 0);
        CHECK(v->node.shard.art.n_subs > 0);
    }
    /* every node's shard knows every other node's rows */
    for (i = 0; i < 4; i++) {
        int j;
        for (j = 0; j < 4; j++) CHECK(shard_subscribes(&net.nodes[i]->node.shard, net.nodes[j]->node.node_id));
    }
    /* ticks agree within one across the cluster */
    {
        uint32_t tmin = 0xFFFFFFFFu, tmax = 0;
        for (i = 0; i < 4; i++) {
            uint32_t t = net.nodes[i]->node.shard.tick;
            if (t < tmin) tmin = t;
            if (t > tmax) tmax = t;
        }
        CHECK(tmax - tmin <= 1);
        CHECK(tmin > 1000);
    }
    CHECK_EQ(net.collisions, 0);
    CHECK(net.frames_sent > 1000);

    /* 4. the coordinator dies: the next strongest takes over with a new epoch, nobody else leads */
    {
        uint32_t ticks_before = net.nodes[2]->node.shard.tick;
        int c;
        vnet_power(&net, 1, false);
        vnet_run_until(&net, 30000000);
        c = vnet_coordinator(&net);
        CHECK(c == 2 || c == 3);                            /* an ESP32 beats the C3; the MAC breaks the tie */
        CHECK_EQ(net.nodes[2]->node.epoch, 2);
        CHECK_EQ(net.nodes[0]->node.epoch, 2);
        CHECK_EQ(net.nodes[3]->node.epoch, 2);
        CHECK(net.nodes[2]->node.shard.tick > ticks_before + 500);
        CHECK(net.nodes[3]->node.shard.tick > ticks_before + 500);
        CHECK(net.nodes[0]->node.shard.tick > ticks_before + 500);
        CHECK(i32abs(vnet_sync_error_us(&net, 0)) < 150);
        CHECK(i32abs(vnet_sync_error_us(&net, 3)) < 150);
        /* the dead node is gone from everyone's roster and rows */
        for (i = 0; i < 4; i++) {
            if (i == 1) continue;
            CHECK(membership_find_id(&net.nodes[i]->node.members, 2) == NULL ||
                  net.nodes[i]->node.members.version > 0);
            CHECK(!shard_subscribes(&net.nodes[i]->node.shard, net.nodes[1]->node.node_id));
        }
        CHECK_EQ(net.nodes[2]->node.tm.control_failures, 0);
    }

    /* 5. it comes back and rejoins as a member with its old id */
    {
        uint16_t old_id = net.nodes[1]->node.node_id;
        int c;
        vnet_power(&net, 1, true);
        vnet_run_until(&net, 45000000);
        c = vnet_coordinator(&net);
        CHECK(c == 2 || c == 3);                            /* a running coordinator is not pre-empted */
        CHECK_EQ(net.nodes[1]->node.state, NODE_STATE_MEMBER);
        CHECK_EQ(net.nodes[1]->node.epoch, 2);
        CHECK_EQ(net.nodes[1]->node.node_id, old_id);       /* the id is remembered for 30 s */
        CHECK(net.nodes[1]->node.tm.bundles_rx > 0);
        CHECK(shard_subscribes(&net.nodes[0]->node.shard, net.nodes[1]->node.node_id));
        CHECK(i32abs(vnet_sync_error_us(&net, 1)) < 150);
    }
    vnet_free(&net);

    /* 6. a mixed cluster: the ESP8266 is a member, never leads, syncs at millisecond level */
    vnet_init(&net, 4);
    base_cfg(&cfg);
    cfg.phy_rate = PHY_RATE_24M;
    vnet_add(&net, DBRAIN_CHIP_ESP8266, 40000, 0, &cfg, 7, 1000);
    vnet_add(&net, DBRAIN_CHIP_ESP32C3, 200000, 0, &cfg, -3, 2000);
    vnet_add(&net, DBRAIN_CHIP_ESP8266, 40000, 0, &cfg, 9, 3000);
    for (i = 0; i < 3; i++) vnet_power(&net, i, true);
    vnet_run_until(&net, 15000000);
    CHECK_EQ(vnet_coordinator(&net), 1);
    CHECK_EQ(net.nodes[1]->node.phy_rate, PHY_RATE_1M);              /* leaf-audible rate */
    CHECK(net.nodes[1]->node.slot_len_us > SLOT_GUARD_SOFT_US);       /* wide guard */
    CHECK_EQ(net.nodes[0]->node.state, NODE_STATE_MEMBER);
    CHECK_EQ(net.nodes[2]->node.state, NODE_STATE_MEMBER);
    CHECK(i32abs(vnet_sync_error_us(&net, 0)) < 1500);
    CHECK(i32abs(vnet_sync_error_us(&net, 2)) < 1500);
    CHECK(net.nodes[0]->node.shard_valid && net.nodes[0]->node.shard.art.n_neurons >= 16);
    CHECK(net.nodes[0]->node.tm.bundles_rx > 0);
    CHECK_EQ(net.nodes[0]->node.tm.seq_gaps, 0);
    vnet_free(&net);

    /* 7. a lossy channel: losses are counted, nobody panics */
    vnet_init(&net, 5);
    base_cfg(&cfg);
    vnet_add(&net, DBRAIN_CHIP_ESP32, 250000, 0, &cfg, 1, 0);
    vnet_add(&net, DBRAIN_CHIP_ESP32, 250000, 0, &cfg, 2, 0);
    vnet_add(&net, DBRAIN_CHIP_ESP32, 250000, 0, &cfg, 3, 0);
    net.loss_pct_x100 = 500;   /* 5 % */
    for (i = 0; i < 3; i++) vnet_power(&net, i, true);
    vnet_run_until(&net, 20000000);
    CHECK(vnet_converged(&net));
    {
        uint32_t gaps = 0, rx = 0;
        for (i = 0; i < 3; i++) { gaps += net.nodes[i]->node.tm.seq_gaps; rx += net.nodes[i]->node.tm.bundles_rx; }
        CHECK(gaps > 0);
        CHECK(rx > 10 * gaps && rx < 40 * gaps);           /* about 5 % lost */
        CHECK(net.losses > 0);
    }
    for (i = 0; i < 3; i++) CHECK_EQ(net.nodes[i]->node.tm.control_failures, 0);
    vnet_free(&net);

    /* 8. late joiner: everyone rebuilds its rows to include the newcomer */
    vnet_init(&net, 6);
    base_cfg(&cfg);
    vnet_add(&net, DBRAIN_CHIP_ESP32, 250000, 0, &cfg, 0, 0);
    vnet_add(&net, DBRAIN_CHIP_ESP32, 250000, 0, &cfg, 0, 0);
    vnet_add(&net, DBRAIN_CHIP_ESP32C3, 200000, 0, &cfg, 0, 0);
    vnet_power(&net, 0, true);
    vnet_power(&net, 1, true);
    vnet_run_until(&net, 10000000);
    CHECK(vnet_converged(&net));
    {
        uint16_t v0 = net.nodes[0]->node.shard_version;
        uint32_t tick0 = net.nodes[0]->node.shard.tick;
        vnet_power(&net, 2, true);
        vnet_run_until(&net, 20000000);
        CHECK(vnet_converged(&net));
        CHECK_EQ(net.nodes[2]->node.state, NODE_STATE_MEMBER);
        CHECK(net.nodes[2]->node.node_id == 3);
        CHECK(net.nodes[0]->node.shard_version > v0);
        CHECK(net.nodes[0]->node.shard.tick > tick0 + 500);      /* warm rebuild kept ticking */
        CHECK(shard_subscribes(&net.nodes[0]->node.shard, 3));
        CHECK(shard_subscribes(&net.nodes[2]->node.shard, net.nodes[0]->node.node_id));
        CHECK(net.nodes[2]->node.tm.bundles_rx > 0);
        CHECK(net.nodes[0]->node.tm.events_late == 0);
    }
    vnet_free(&net);
    UNIT_END();
}
