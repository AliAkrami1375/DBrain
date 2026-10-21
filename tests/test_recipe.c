#include "unit.h"
#include "dbrain/recipe.h"
#include "dbrain/synapse.h"

#define NODES 3
static uint8_t img[NODES][65536] __attribute__((aligned(4)));
static uint8_t img2[65536] __attribute__((aligned(4)));
static uint8_t arena[NODES][65536] __attribute__((aligned(4)));

int main(void)
{
    recipe_t r, r2;
    uint8_t wire[RECIPE_LEN];
    recipe_member_t members[NODES] = { {3, 60}, {7, 40}, {12, 50} };
    int32_t size[NODES], s2;
    shard_t sh[NODES];
    telemetry_t tm[NODES];
    cap_t cap;
    unsigned k, t;
    uint32_t fired[NODES] = { 0, 0, 0 };
    uint32_t first_fire[NODES] = { 0, 0, 0 };

    /* wire round trip and validation */
    recipe_default(&r);
    recipe_encode(wire, &r);
    CHECK(recipe_decode(wire, &r2));
    CHECK_EQ(memcmp(&r, &r2, sizeof r), 0);
    wire[4] = 9;
    CHECK(!recipe_decode(wire, &r2));
    wire[4] = 0; wire[5] = 101;
    CHECK(!recipe_decode(wire, &r2));

    memset(&cap, 0, sizeof cap);
    cap.chip = DBRAIN_CHIP_ESP32; cap.cores = 2; cap.mhz = 240; cap.free_heap = 200000;
    cap_classify(&cap);
    CHECK_EQ(recipe_neurons_for(&r, &cap), cap_neuron_budget(&cap, 30));
    CHECK_EQ(recipe_exc_count(&r, 100), 80);

    /* random topology: builds for every node, parses, deterministic */
    for (k = 0; k < NODES; k++) {
        size[k] = recipe_build(&r, members[k].node_id, 1, members, NODES, img[k], sizeof img[k]);
        CHECK(size[k] > 0);
        CHECK_EQ(shard_bind(&sh[k], img[k], (uint32_t)size[k], arena[k], sizeof arena[k], &tm[k]), ARTIFACT_OK);
        CHECK_EQ(sh[k].art.shard_id, members[k].node_id);
        CHECK_EQ(sh[k].art.n_neurons, members[k].n_neurons);
        CHECK_EQ(sh[k].art.n_populations, 2);
        CHECK_EQ(sh[k].art.pop_first[1], recipe_exc_count(&r, members[k].n_neurons));
    }
    s2 = recipe_build(&r, 7, 1, members, NODES, img2, sizeof img2);
    CHECK_EQ(s2, size[1]);
    CHECK_EQ(memcmp(img2, img[1], (size_t)s2), 0);
    /* every node subscribes to every other node and to itself */
    for (k = 0; k < NODES; k++) {
        unsigned j;
        for (j = 0; j < NODES; j++) CHECK(shard_subscribes(&sh[k], members[j].node_id));
    }
    /* cross-node delays are at least 2, local at least 1; row of a remote source exists */
    {
        const sub_entry_t *e = artifact_find_row(&sh[0].art, gid_make(7, 0));
        CHECK(e != NULL);
        if (e) {
            uint32_t i = 0;
            while (i < e->row_words) {
                uint32_t h = sh[0].art.rows[e->row_offset + i++];
                uint8_t c = syn_seg_count(h), q;
                for (q = 0; q < c; q++) CHECK(syn_delay(sh[0].art.rows[e->row_offset + i + q]) >= 2);
                i += c;
            }
        }
    }
    /* fan-in: node 3 (60 cells) receives about 10 remote synapses per neuron in total */
    {
        uint32_t remote = 0, local = 0;
        uint32_t i;
        for (i = 0; i < sh[0].art.n_subs; i++) {
            const sub_entry_t *e = &sh[0].art.subs[i];
            uint32_t w = 0, p = 0;
            while (p < e->row_words) { uint8_t c = syn_seg_count(sh[0].art.rows[e->row_offset + p]); w += c; p += 1u + c; }
            if (gid_shard(e->gid) == 3) local += w; else remote += w;
        }
        CHECK(local >= 60 * 18 && local <= 60 * 22);
        CHECK(remote >= 60 * 8 && remote <= 60 * 12);
    }
    /* a roster that is not sorted or does not contain self is rejected */
    {
        recipe_member_t bad[2] = { {7, 40}, {3, 60} };
        CHECK_EQ(recipe_build(&r, 7, 1, bad, 2, img2, sizeof img2), ARTIFACT_E_ORDER);
        CHECK_EQ(recipe_build(&r, 99, 1, members, NODES, img2, sizeof img2), ARTIFACT_E_ARG);
        CHECK_EQ(recipe_build(&r, 7, 1, members, NODES, img2, 100), ARTIFACT_E_NOSPACE);
    }
    /* drive is reproducible and bounded */
    for (k = 0; k < NODES; k++) shard_reset(&sh[k]);
    recipe_drive(&r, &sh[0]);
    {
        accum_t a = sh[0].buckets[0], b;
        shard_reset(&sh[0]);
        recipe_drive(&r, &sh[0]);
        b = sh[0].buckets[0];
        CHECK_EQ(a, b);
        CHECK(a >= 0 && a < FIX_INT(12));
    }

    /* the random network runs and keeps firing under drive, bit-identical on a second run */
    {
        uint32_t spikes = 0, spikes2 = 0;
        for (k = 0; k < NODES; k++) shard_reset(&sh[k]);
        for (t = 0; t < 500; t++) {
            for (k = 0; k < NODES; k++) { recipe_drive(&r, &sh[k]); spikes += shard_tick(&sh[k]); }
            for (k = 0; k < NODES; k++) {
                unsigned j;
                for (j = 0; j < NODES; j++)
                    if (j != k) shard_deliver(&sh[j], sh[k].art.shard_id, sh[k].tick - 1, sh[k].fired, sh[k].n_fired);
            }
        }
        CHECK(spikes > 100);
        CHECK_EQ(tm[0].events_late + tm[1].events_late + tm[2].events_late, 0);
        for (k = 0; k < NODES; k++) shard_reset(&sh[k]);
        for (t = 0; t < 500; t++) {
            for (k = 0; k < NODES; k++) { recipe_drive(&r, &sh[k]); spikes2 += shard_tick(&sh[k]); }
            for (k = 0; k < NODES; k++) {
                unsigned j;
                for (j = 0; j < NODES; j++)
                    if (j != k) shard_deliver(&sh[j], sh[k].art.shard_id, sh[k].tick - 1, sh[k].fired, sh[k].n_fired);
            }
        }
        CHECK_EQ(spikes, spikes2);
    }

    /* synfire ring: 3 -> 7 -> 12 -> 3; kick node 3 and watch activity travel */
    r.topology = RECIPE_SYNFIRE;
    r.local_fan_in = 0;
    r.remote_fan_in = 30;
    r.w_exc = 4 * 128;
    r.drive = 0;
    for (k = 0; k < NODES; k++) {
        size[k] = recipe_build(&r, members[k].node_id, 2, members, NODES, img[k], sizeof img[k]);
        CHECK(size[k] > 0);
        CHECK_EQ(shard_bind(&sh[k], img[k], (uint32_t)size[k], arena[k], sizeof arena[k], &tm[k]), ARTIFACT_OK);
    }
    CHECK(shard_subscribes(&sh[1], 3));
    CHECK(!shard_subscribes(&sh[1], 12));
    CHECK(shard_subscribes(&sh[2], 7));
    CHECK(shard_subscribes(&sh[0], 12));
    CHECK(!shard_subscribes(&sh[0], 3));             /* no local synapses in this recipe */
    for (t = 0; t < 10; t++) {
        unsigned i;
        for (i = 0; i < 60; i++) shard_inject(&sh[0], (uint16_t)i, FIX_INT(30), t);
    }
    for (t = 0; t < 200; t++) {
        for (k = 0; k < NODES; k++) {
            uint16_t n = shard_tick(&sh[k]);
            if (n && !fired[k]) first_fire[k] = t;
            fired[k] += n;
        }
        for (k = 0; k < NODES; k++) {
            unsigned j;
            for (j = 0; j < NODES; j++)
                if (j != k) shard_deliver(&sh[j], sh[k].art.shard_id, sh[k].tick - 1, sh[k].fired, sh[k].n_fired);
        }
    }
    CHECK(fired[0] > 0 && fired[1] > 0 && fired[2] > 0);
    CHECK(first_fire[0] < first_fire[1] && first_fire[1] < first_fire[2]);
    CHECK(first_fire[1] - first_fire[0] >= 2);        /* at least the minimum cross-node delay */
    UNIT_END();
}
