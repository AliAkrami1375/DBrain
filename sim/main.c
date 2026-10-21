/*
 * dbrain-sim: scenarios on the virtual radio cell.
 *
 *   dbrain-sim [options]
 *     --nodes N            number of boards (default 4)
 *     --chips list         comma-separated chip names cycled over the boards
 *                          (esp32, esp32s2, esp32s3, esp32c2, esp32c3, esp32c5, esp32c6, esp8266)
 *     --seconds S          simulated time (default 30)
 *     --loss P             per-receiver frame loss in percent (default 0)
 *     --rate R             preferred PHY rate: 1, 6, 24, 54 Mbps (default 1)
 *     --kill-coord-at S    power the coordinator off at S seconds
 *     --join-at S          power the last board on at S seconds instead of 0
 *     --topology T         random, spatial, synfire (default random)
 *     --seed X             simulation seed (default 1)
 *     --raster N           print the first N spike events
 *     --verbose            node logs at info level
 *     --trace              report tick lag episodes and resyncs
 *
 * Prints a JSON summary on stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vnet.h"
#include "dbrain/slot.h"

static uint8_t chip_of(const char *name)
{
    if (!strcmp(name, "esp8266")) return DBRAIN_CHIP_ESP8266;
    if (!strcmp(name, "esp32"))   return DBRAIN_CHIP_ESP32;
    if (!strcmp(name, "esp32s2")) return DBRAIN_CHIP_ESP32S2;
    if (!strcmp(name, "esp32s3")) return DBRAIN_CHIP_ESP32S3;
    if (!strcmp(name, "esp32c2")) return DBRAIN_CHIP_ESP32C2;
    if (!strcmp(name, "esp32c3")) return DBRAIN_CHIP_ESP32C3;
    if (!strcmp(name, "esp32c5")) return DBRAIN_CHIP_ESP32C5;
    if (!strcmp(name, "esp32c6")) return DBRAIN_CHIP_ESP32C6;
    return DBRAIN_CHIP_ESP32;
}

static uint32_t heap_of(uint8_t chip)
{
    switch (chip) {
    case DBRAIN_CHIP_ESP8266: return 40000;
    case DBRAIN_CHIP_ESP32C2: return 150000;
    case DBRAIN_CHIP_ESP32C3: return 200000;
    case DBRAIN_CHIP_ESP32S2: return 180000;
    default:           return 250000;
    }
}

int main(int argc, char **argv)
{
    int nodes = 4, i;
    double seconds = 30, kill_at = -1, join_at = -1;
    double loss = 0;
    uint32_t seed = 1, raster = 0;
    int rate = 1;
    const char *chips = "esp32";
    const char *topology = "random";
    bool verbose = false, trace = false;
    vnet_t net;
    node_config_t cfg;
    uint64_t t, end_us, converged_at = 0, reelected_at = 0;
    int coord_before_kill = -1;
    char chipbuf[256];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nodes") && i + 1 < argc) nodes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chips") && i + 1 < argc) chips = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc) loss = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--kill-coord-at") && i + 1 < argc) kill_at = atof(argv[++i]);
        else if (!strcmp(argv[i], "--join-at") && i + 1 < argc) join_at = atof(argv[++i]);
        else if (!strcmp(argv[i], "--topology") && i + 1 < argc) topology = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--raster") && i + 1 < argc) raster = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "--trace")) trace = true;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (nodes < 1 || nodes > VNET_MAX_NODES) { fprintf(stderr, "nodes must be 1..%d\n", VNET_MAX_NODES); return 2; }

    vnet_init(&net, seed);
    net.verbose = verbose;
    net.trace = trace;
    net.raster_limit = raster;
    net.loss_pct_x100 = (uint32_t)(loss * 100.0);
    net.phy_rate = rate >= 54 ? PHY_RATE_54M : rate >= 24 ? PHY_RATE_24M : rate >= 6 ? PHY_RATE_6M : PHY_RATE_1M;

    node_config_default(&cfg);
    cfg.cluster_id = 7;
    cfg.phy_rate = net.phy_rate;
    cfg.discover_ms = 3000;
    if (!strcmp(topology, "spatial")) cfg.recipe.topology = RECIPE_SPATIAL;
    else if (!strcmp(topology, "synfire")) {
        cfg.recipe.topology = RECIPE_SYNFIRE;
        cfg.recipe.local_fan_in = 0;
        cfg.recipe.remote_fan_in = 30;
        cfg.recipe.w_exc = 4 * 128;
        cfg.recipe.drive = 0;
    }

    strncpy(chipbuf, chips, sizeof chipbuf - 1);
    chipbuf[sizeof chipbuf - 1] = 0;
    {
        char *names[64];
        int n_names = 0;
        char *tok = strtok(chipbuf, ",");
        while (tok && n_names < 64) { names[n_names++] = tok; tok = strtok(NULL, ","); }
        if (n_names == 0) names[n_names++] = (char *)"esp32";
        for (i = 0; i < nodes; i++) {
            uint8_t chip = chip_of(names[i % n_names]);
            int32_t ppm = (int32_t)(rng_below(&net.rng, 21) ) - 10;
            int64_t off = (int64_t)rng_below(&net.rng, 2000000000u) - 1000000000;
            vnet_add(&net, chip, heap_of(chip), chip == DBRAIN_CHIP_ESP32S3 ? (4u << 20) : 0, &cfg, ppm, off);
        }
    }
    for (i = 0; i < nodes; i++)
        if (!(join_at >= 0 && i == nodes - 1)) vnet_power(&net, i, true);

    end_us = (uint64_t)(seconds * 1e6);
    for (t = 0; t < end_us; t += 100000) {
        vnet_run_until(&net, t);
        if (!converged_at && vnet_converged(&net)) converged_at = t;
        if (kill_at >= 0 && t >= (uint64_t)(kill_at * 1e6) && coord_before_kill < 0) {
            coord_before_kill = vnet_coordinator(&net);
            if (coord_before_kill >= 0) {
                vnet_power(&net, coord_before_kill, false);
                fprintf(stderr, "[%9.3f] killed coordinator n%d\n", (double)t / 1e6, coord_before_kill);
            }
        }
        if (coord_before_kill >= 0 && !reelected_at && vnet_converged(&net)) reelected_at = t;
        if (join_at >= 0 && t >= (uint64_t)(join_at * 1e6) && !net.nodes[nodes - 1]->powered) {
            vnet_power(&net, nodes - 1, true);
            fprintf(stderr, "[%9.3f] powered n%d\n", (double)t / 1e6, nodes - 1);
        }
    }

    printf("{\"nodes\":%d,\"seconds\":%.1f,\"converged_at\":%.1f,\"coordinator\":%d,"
           "\"reelected_at\":%.1f,\"frames_sent\":%u,\"collisions\":%u,\"losses\":%u,\"per_node\":[\n",
           nodes, seconds, converged_at / 1e6, vnet_coordinator(&net), reelected_at / 1e6,
           net.frames_sent, net.collisions, net.losses);
    for (i = 0; i < nodes; i++) {
        const vnode_t *v = net.nodes[i];
        const telemetry_t *tm = &v->node.tm;
        int32_t err = vnet_sync_error_us(&net, i);
        printf("  {\"index\":%d,\"chip\":%u,\"powered\":%s,\"state\":\"%s\",\"node_id\":%u,\"epoch\":%u,"
               "\"coordinator\":%u,\"shard_neurons\":%u,\"shard_version\":%u,\"tick\":%lu,"
               "\"sync_error_us\":%ld,\"residual_us\":%ld,\"spikes\":%u,\"bundles_tx\":%u,\"bundles_rx\":%u,"
               "\"events_applied\":%u,\"events_late\":%u,\"events_early\":%u,\"seq_gaps\":%u,"
               "\"overruns\":%u,\"resyncs\":%u,\"ctrl_retries\":%u,\"ctrl_failures\":%u,"
               "\"frames_rx\":%u,\"frames_lost\":%u,\"joined_at_ms\":%u}%s\n",
               i, v->cap.chip, v->powered ? "true" : "false", node_state_name(v->node.state),
               v->node.node_id, v->node.epoch, v->node.coord_id,
               v->node.shard_valid ? v->node.shard.art.n_neurons : 0, v->node.shard_version,
               (unsigned long)(v->node.shard_valid ? v->node.shard.tick : 0),
               err == INT32_MIN ? 0L : (long)err, (long)v->last_residual_us, v->spikes,
               tm->bundles_tx, tm->bundles_rx, tm->events_applied, tm->events_late, tm->events_early,
               tm->seq_gaps, tm->tick_overruns, tm->tick_resyncs, tm->control_retries,
               tm->control_failures, v->frames_rx, v->frames_lost, v->joined_at_ms,
               i + 1 < nodes ? "," : "");
    }
    printf("]}\n");
    vnet_free(&net);
    return 0;
}
