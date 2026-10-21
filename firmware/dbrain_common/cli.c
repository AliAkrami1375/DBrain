#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"
#include "gateway.h"
#include "bench.h"
#include "dbrain/version.h"
#include "dbrain/crc32.h"

static cli_hooks_t h;
static char linebuf[128];
static size_t linelen;

void cli_init(const cli_hooks_t *hooks)
{
    h = *hooks;
    linelen = 0;
}

static void lock(void)   { if (h.lock) h.lock(); }
static void unlock(void) { if (h.unlock) h.unlock(); }

static void cmd_help(void)
{
    puts("commands:");
    puts("  status                   node, cluster and shard summary");
    puts("  members                  membership table");
    puts("  telemetry                counters as JSON");
    puts("  stream off|summary|full  event stream on the console");
    puts("  inject N CURRENT [TICK]  add CURRENT (s16.15) to neuron N");
    puts("  pause | resume           stop and restart the tick loop");
    puts("  set KEY VALUE            persist a setting (see docs/firmware.md)");
    puts("  erase                    forget all persisted settings");
    puts("  artifact info|save       inspect or persist the shard image");
    puts("  bench tx N LEN US | bench rx | bench stop | bench report");
    puts("  reboot | version | help");
}

static void cmd_status(void)
{
    node_t *n = h.node;
    uint32_t tick = 0;
    uint64_t ct = 0;

    lock();
    node_current_tick(n, &tick);
    node_cluster_time(n, h.now_us(), &ct);
    printf("DBrain %s  node %u  state %s  epoch %u  coordinator %u%s\n", DBRAIN_VERSION, (unsigned)n->node_id,
           node_state_name(n->state), (unsigned)n->epoch, (unsigned)n->coord_id,
           n->have_coord ? "" : " (none)");
    printf("mac %02x:%02x:%02x:%02x:%02x:%02x  chip %u tier %u score %08lx  free heap %lu\n", n->mac[0], n->mac[1],
           n->mac[2], n->mac[3], n->mac[4], n->mac[5], (unsigned)n->cap.chip, (unsigned)n->cap.tier,
           (unsigned long)cap_score(&n->cap), (unsigned long)n->cap.free_heap);
    if (n->cluster_valid)
        printf("cluster %u: %u members, tick %lu, period %lu us, slot %u us, rate %u, cluster time %llu\n",
               (unsigned)n->cfg.cluster_id, (unsigned)n->cluster_members, (unsigned long)tick,
               (unsigned long)n->tick_period_us, (unsigned)n->slot_len_us, (unsigned)n->phy_rate,
               (unsigned long long)ct);
    else
        printf("cluster %u: not synchronised\n", (unsigned)n->cfg.cluster_id);
    if (n->shard_valid)
        printf("shard %u v%u: %u neurons, %lu sources, %lu row words, tick %lu, %s\n",
               (unsigned)n->shard.art.shard_id, (unsigned)n->shard_version, (unsigned)n->shard.art.n_neurons,
               (unsigned long)n->shard.art.n_subs, (unsigned long)n->shard.art.n_row_words,
               (unsigned long)n->shard.tick, n->paused ? "paused" : "running");
    else
        printf("shard: none (%s mode)\n", n->cfg.mode == NODE_MODE_RECIPE ? "recipe" : "artifact");
    printf("sync residual %ld us, samples %lu, resyncs %lu, overruns %lu, rx queue drops %lu\n",
           (long)n->tm.sync_residual_us, (unsigned long)n->tm.sync_samples, (unsigned long)n->tm.tick_resyncs,
           (unsigned long)n->tm.tick_overruns, (unsigned long)(h.rx_dropped ? h.rx_dropped() : 0));
    unlock();
}

static void cmd_members(void)
{
    unsigned i;
    node_t *n = h.node;

    lock();
    printf("%-5s %-17s %-5s %-4s %-6s %-9s %-6s %-7s %-10s %s\n", "id", "mac", "chip", "tier", "live", "state",
           "shard", "neurons", "tick", "drops");
    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        const member_t *m = &n->members.m[i];
        if (m->liveness == MEMBER_EMPTY) continue;
        printf("%-5u %02x:%02x:%02x:%02x:%02x:%02x %-5u %-4u %-6s %-9s %-6u %-7u %-10lu %u%s\n",
               (unsigned)m->node_id, m->mac[0], m->mac[1], m->mac[2], m->mac[3], m->mac[4], m->mac[5],
               (unsigned)m->cap.chip, (unsigned)m->cap.tier,
               m->liveness == MEMBER_ALIVE ? "alive" : m->liveness == MEMBER_SUSPECT ? "suspect" : "dead",
               node_state_name(m->node_state), (unsigned)m->shard_id, (unsigned)m->n_neurons,
               (unsigned long)m->tick, (unsigned)m->drops, (m->flags & HB_FLAG_COORDINATOR) ? " *" : "");
    }
    unlock();
}

static void cmd_telemetry(void)
{
    const telemetry_t *t = &h.node->tm;
    lock();
    printf("{\"bundles_tx\":%lu,\"bundles_rx\":%lu,\"bundles_ignored\":%lu,\"bundles_bad\":%lu,"
           "\"events_tx\":%lu,\"events_rx\":%lu,\"events_applied\":%lu,\"events_late\":%lu,"
           "\"events_early\":%lu,\"events_norow\":%lu,\"seq_gaps\":%lu,\"tick_overruns\":%lu,"
           "\"tick_resyncs\":%lu,\"sync_residual_us\":%ld,\"sync_samples\":%lu,\"heartbeats_rx\":%lu,"
           "\"control_retries\":%lu,\"control_failures\":%lu,\"elections\":%lu}\n",
           (unsigned long)t->bundles_tx, (unsigned long)t->bundles_rx, (unsigned long)t->bundles_ignored,
           (unsigned long)t->bundles_bad, (unsigned long)t->events_tx, (unsigned long)t->events_rx,
           (unsigned long)t->events_applied, (unsigned long)t->events_late, (unsigned long)t->events_early,
           (unsigned long)t->events_norow, (unsigned long)t->seq_gaps, (unsigned long)t->tick_overruns,
           (unsigned long)t->tick_resyncs, (long)t->sync_residual_us, (unsigned long)t->sync_samples,
           (unsigned long)t->heartbeats_rx, (unsigned long)t->control_retries,
           (unsigned long)t->control_failures, (unsigned long)t->elections);
    unlock();
}

static void cmd_artifact(const char *sub)
{
    node_t *n = h.node;

    if (sub && strcmp(sub, "save") == 0) {
        int r = -1;
        lock();
        if (n->shard_valid && h.save_artifact)
            r = h.save_artifact(n->shard.art.image, n->shard.art.size);
        unlock();
        printf(r == 0 ? "saved\n" : "nothing to save or write failed\n");
        return;
    }
    lock();
    if (!n->shard_valid) {
        printf("no shard bound\n");
    } else {
        const artifact_view_t *a = &n->shard.art;
        printf("shard %u version %u: %u neurons in %u populations, %lu rows, %lu row words, %lu bytes, crc %08lx\n",
               (unsigned)a->shard_id, (unsigned)a->shard_version, (unsigned)a->n_neurons,
               (unsigned)a->n_populations, (unsigned long)a->n_subs, (unsigned long)a->n_row_words,
               (unsigned long)a->size, (unsigned long)crc32_of(a->image, a->size));
    }
    unlock();
}

void cli_execute(char *line)
{
    char *argv[6];
    int argc = 0;
    char *tok = strtok(line, " \t\r\n");

    while (tok && argc < 6) { argv[argc++] = tok; tok = strtok(NULL, " \t\r\n"); }
    if (argc == 0) return;

    if (!strcmp(argv[0], "help")) cmd_help();
    else if (!strcmp(argv[0], "version")) printf("DBrain %s protocol %u artifact %u model %u\n", DBRAIN_VERSION,
                                                 DBRAIN_PROTO_VERSION, DBRAIN_ARTIFACT_VERSION, DBRAIN_MODEL_VERSION);
    else if (!strcmp(argv[0], "status")) cmd_status();
    else if (!strcmp(argv[0], "members")) cmd_members();
    else if (!strcmp(argv[0], "telemetry")) cmd_telemetry();
    else if (!strcmp(argv[0], "stream") && argc >= 2) {
        if (!strcmp(argv[1], "off")) gateway_set_mode(0);
        else if (!strcmp(argv[1], "summary")) gateway_set_mode(1);
        else if (!strcmp(argv[1], "full")) gateway_set_mode(2);
        else printf("stream off|summary|full\n");
    }
    else if (!strcmp(argv[0], "inject") && argc >= 3) {
        int r;
        lock();
        r = node_inject(h.node, (uint16_t)strtoul(argv[1], NULL, 0), (accum_t)strtol(argv[2], NULL, 0),
                        argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0);
        unlock();
        printf(r == 0 ? "ok\n" : "rejected (%d)\n", r);
    }
    else if (!strcmp(argv[0], "pause")) { lock(); h.node->paused = true; unlock(); puts("paused"); }
    else if (!strcmp(argv[0], "resume")) { lock(); h.node->paused = false; h.node->tick_started = false; unlock(); puts("running"); }
    else if (!strcmp(argv[0], "set") && argc >= 3) printf(h.set && h.set(argv[1], argv[2]) == 0 ? "stored; takes effect after reboot\n" : "unknown key or bad value\n");
    else if (!strcmp(argv[0], "erase")) { if (h.erase) h.erase(); puts("settings erased"); }
    else if (!strcmp(argv[0], "artifact")) cmd_artifact(argc >= 2 ? argv[1] : NULL);
    else if (!strcmp(argv[0], "bench") && argc >= 2) {
        if (!strcmp(argv[1], "tx") && argc >= 5)
            printf(bench_start_tx((uint32_t)strtoul(argv[2], NULL, 0), (uint16_t)strtoul(argv[3], NULL, 0),
                                  (uint32_t)strtoul(argv[4], NULL, 0), h.now_us()) == 0 ? "bench tx started\n" : "bad arguments\n");
        else if (!strcmp(argv[1], "rx")) { bench_start_rx(); puts("bench rx counting"); }
        else if (!strcmp(argv[1], "stop")) { bench_report(h.now_us()); bench_stop(); }
        else if (!strcmp(argv[1], "report")) bench_report(h.now_us());
        else puts("bench tx N LEN INTERVAL_US | bench rx | bench stop | bench report");
    }
    else if (!strcmp(argv[0], "reboot")) { if (h.reboot) h.reboot(); }
    else printf("unknown command; try help\n");
}

void cli_feed_char(char c)
{
    if (c == '\n' || c == '\r') {
        if (linelen) {
            linebuf[linelen] = 0;
            cli_execute(linebuf);
            linelen = 0;
        }
        return;
    }
    if (c == 8 || c == 127) { if (linelen) linelen--; return; }
    if (linelen + 1 < sizeof linebuf) linebuf[linelen++] = c;
}
