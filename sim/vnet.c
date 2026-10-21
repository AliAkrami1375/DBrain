#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vnet.h"
#include "dbrain/frame.h"
#include "dbrain/slot.h"

/* ---- event heap ------------------------------------------------------- */

static void heap_push(vnet_t *net, const vevent_t *ev)
{
    size_t i;

    if (net->heap_len == net->heap_cap) {
        net->heap_cap = net->heap_cap ? net->heap_cap * 2 : 1024;
        net->heap = (vevent_t *)realloc(net->heap, net->heap_cap * sizeof(vevent_t));
        if (!net->heap) { fprintf(stderr, "vnet: out of memory\n"); exit(2); }
    }
    i = net->heap_len++;
    net->heap[i] = *ev;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (net->heap[p].at_us <= net->heap[i].at_us) break;
        { vevent_t t = net->heap[p]; net->heap[p] = net->heap[i]; net->heap[i] = t; }
        i = p;
    }
}

static bool heap_pop(vnet_t *net, vevent_t *out)
{
    size_t i = 0;

    if (net->heap_len == 0) return false;
    *out = net->heap[0];
    net->heap[0] = net->heap[--net->heap_len];
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < net->heap_len && net->heap[l].at_us < net->heap[m].at_us) m = l;
        if (r < net->heap_len && net->heap[r].at_us < net->heap[m].at_us) m = r;
        if (m == i) break;
        { vevent_t t = net->heap[m]; net->heap[m] = net->heap[i]; net->heap[i] = t; }
        i = m;
    }
    return true;
}

/* ---- clocks ----------------------------------------------------------- */

uint64_t vnet_local_time(const vnode_t *v, uint64_t sim_us)
{
    int64_t t = (int64_t)VNET_CLOCK_BASE_US + (int64_t)sim_us + v->clock_offset_us +
                ((int64_t)sim_us * v->clock_ppm) / 1000000;
    return t < 0 ? 0 : (uint64_t)t;
}

/* ---- platform hooks --------------------------------------------------- */

static uint64_t pf_now_us(void *ctx)
{
    vnode_t *v = (vnode_t *)ctx;
    return vnet_local_time(v, v->net->now_us);
}

static void pf_get_mac(void *ctx, uint8_t mac[6])
{
    memcpy(mac, ((vnode_t *)ctx)->mac, 6);
}

static void pf_get_cap(void *ctx, cap_t *cap)
{
    *cap = ((vnode_t *)ctx)->cap;
}

static uint32_t pf_random(void *ctx)
{
    return rng_next(&((vnode_t *)ctx)->net->rng);
}

static void pf_log(void *ctx, int level, const char *msg)
{
    vnode_t *v = (vnode_t *)ctx;
    if (v->net->verbose || level <= NODE_LOG_WARN)
        fprintf(stderr, "[%9.3f] n%d: %s\n", (double)v->net->now_us / 1e6, v->index, msg);
}

static void pf_event(void *ctx, const node_event_t *ev)
{
    vnode_t *v = (vnode_t *)ctx;
    vnet_t *net = v->net;

    switch (ev->type) {
    case NODE_EV_SPIKES:
        v->spikes += ev->count;
        if (net->raster_printed < net->raster_limit) {
            unsigned i;
            net->raster_printed++;
            printf("spikes node=%u tick=%lu n=%u:", (unsigned)ev->node_id, (unsigned long)ev->tick, ev->count);
            for (i = 0; i < ev->count && i < 16; i++) printf(" %u", ev->events[i]);
            printf("%s\n", ev->count > 16 ? " ..." : "");
        }
        break;
    case NODE_EV_SYNC:
        v->last_residual_us = ev->value;
        break;
    case NODE_EV_TICK_RESYNC:
        v->resyncs++;
        if (net->trace)
            fprintf(stderr, "[%9.3f] n%d: tick resync to %lu (%ld)\n", (double)net->now_us / 1e6, v->index,
                    (unsigned long)ev->tick, (long)ev->value);
        break;
    case NODE_EV_JOINED:
        if (v->joined_at_ms == 0) v->joined_at_ms = (uint32_t)(net->now_us / 1000u) + 1u;
        break;
    case NODE_EV_COORDINATOR:
        v->coordinator_changes++;
        break;
    default:
        break;
    }
}

/* Frames enter the channel here. */
static int pf_send(void *ctx, const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag)
{
    vnode_t *v = (vnode_t *)ctx;
    vnet_t *net = v->net;
    uint64_t start, end;
    unsigned i;
    vevent_t ev;
    uint32_t id = net->next_tx_id++;
    bool collided = false;

    if (len == 0 || len > 250) return -1;
    v->frames_tx++;
    net->frames_sent++;

    /* CSMA: wait for the channel, then back off */
    start = net->now_us;
    if (start < net->busy_until_us) start = net->busy_until_us;
    start += rng_below(&net->rng, VNET_BACKOFF_US);
    end = start + slot_airtime_us(net->phy_rate, (uint16_t)len) - VNET_BACKOFF_US - 50u;
    net->busy_until_us = end;

    /* two transmissions on the air at once destroy each other */
    for (i = 0; i < net->n_air; i++) {
        vair_t *a = &net->air[i];
        if (a->end_us <= net->now_us - (net->now_us > 100000 ? 100000 : net->now_us)) continue;
        if (a->start_us < end && start < a->end_us) { a->collided = true; collided = true; }
    }
    if (collided) net->collisions++;
    /* keep the recent transmissions; the list is small */
    if (net->n_air == 16) { memmove(net->air, net->air + 1, 15 * sizeof(vair_t)); net->n_air = 15; }
    net->air[net->n_air].start_us = start;
    net->air[net->n_air].end_us = end;
    net->air[net->n_air].id = id;
    net->air[net->n_air].collided = collided;
    net->n_air++;

    memset(&ev, 0, sizeof ev);
    ev.len = len;
    memcpy(ev.frame, frame, len);
    ev.from = (int16_t)v->index;
    ev.tag = tag;
    ev.tx_id = id;

    for (i = 0; i < (unsigned)net->n_nodes; i++) {
        vnode_t *r = net->nodes[i];
        if (r == v || !r->powered) continue;
        if (mac != NULL && memcmp(mac, r->mac, 6) != 0) continue;
        ev.kind = 1;
        ev.node = (int16_t)i;
        ev.at_us = end + rng_below(&net->rng, VNET_SPREAD_US + 1u);
        heap_push(net, &ev);
    }
    ev.kind = 2;
    ev.node = (int16_t)v->index;
    ev.at_us = end + VNET_CB_LATENCY_US + rng_below(&net->rng, 20u);
    heap_push(net, &ev);
    return 0;
}

/* ---- construction ----------------------------------------------------- */

void vnet_init(vnet_t *net, uint32_t seed)
{
    memset(net, 0, sizeof(*net));
    rng_seed(&net->rng, seed, 0xC1A55u, 7u);
    net->phy_rate = PHY_RATE_1M;
    net->raster_limit = 0;
}

void vnet_free(vnet_t *net)
{
    int i;
    for (i = 0; i < net->n_nodes; i++) {
        free(net->nodes[i]->arena);
        free(net->nodes[i]->image);
        free(net->nodes[i]);
    }
    free(net->heap);
    memset(net, 0, sizeof(*net));
}

int vnet_add(vnet_t *net, uint8_t chip, uint32_t free_heap, uint32_t psram, const node_config_t *cfg,
             int32_t clock_ppm, int64_t clock_offset_us)
{
    vnode_t *v;

    if (net->n_nodes >= VNET_MAX_NODES) return -1;
    v = (vnode_t *)calloc(1, sizeof(vnode_t));
    if (!v) return -1;
    v->net = net;
    v->index = net->n_nodes;
    v->mac[0] = 0x24; v->mac[1] = 0x6F; v->mac[2] = 0x28;
    v->mac[3] = (uint8_t)(rng_next(&net->rng));
    v->mac[4] = (uint8_t)(v->index >> 8);
    v->mac[5] = (uint8_t)v->index;
    memset(&v->cap, 0, sizeof v->cap);
    v->cap.chip = chip;
    v->cap.cores = (chip == DBRAIN_CHIP_ESP32 || chip == DBRAIN_CHIP_ESP32S3) ? 2 : 1;
    v->cap.mhz = chip == DBRAIN_CHIP_ESP8266 ? 80 : (chip == DBRAIN_CHIP_ESP32C2 ? 120 : (chip == DBRAIN_CHIP_ESP32C3 || chip == DBRAIN_CHIP_ESP32C6) ? 160 : 240);
    v->cap.free_heap = free_heap;
    v->cap.psram = psram;
    v->cap.flags = CAP_FLAG_HW_RX_TS;
    if (psram) v->cap.flags |= CAP_FLAG_PSRAM;
    if (chip == DBRAIN_CHIP_ESP32 || chip == DBRAIN_CHIP_ESP32S3) v->cap.flags |= CAP_FLAG_FPU;
    cap_classify(&v->cap);
    v->hw_ts = (v->cap.flags & CAP_FLAG_HW_RX_TS) != 0;
    v->clock_ppm = clock_ppm;
    v->clock_offset_us = clock_offset_us;
    v->cfg = *cfg;
    v->arena_size = shard_arena_size(2000);
    v->arena = calloc(1, v->arena_size);
    v->image_cap = 1u << 20;
    v->image = (uint8_t *)calloc(1, v->image_cap);
    v->cfg.arena = v->arena;
    v->cfg.arena_size = v->arena_size;
    v->cfg.image_buf = v->image;
    v->cfg.image_cap = v->image_cap;
    net->nodes[net->n_nodes++] = v;
    return v->index;
}

void vnet_power(vnet_t *net, int idx, bool on)
{
    vnode_t *v = net->nodes[idx];
    node_platform_t pf;
    vevent_t ev;

    if (on == v->powered) return;
    v->powered = on;
    if (!on) return;
    memset(&pf, 0, sizeof pf);
    pf.ctx = v;
    pf.now_us = pf_now_us;
    pf.send = pf_send;
    pf.get_mac = pf_get_mac;
    pf.get_cap = pf_get_cap;
    pf.random = pf_random;
    pf.log = pf_log;
    pf.event = pf_event;
    v->joined_at_ms = 0;
    node_init(&v->node, &pf, &v->cfg);
    node_start(&v->node);
    memset(&ev, 0, sizeof ev);
    ev.kind = 0;
    ev.node = (int16_t)idx;
    ev.at_us = net->now_us + VNET_POLL_US;
    heap_push(net, &ev);
}

/* ---- run -------------------------------------------------------------- */

void vnet_run_until(vnet_t *net, uint64_t until_us)
{
    vevent_t ev;

    while (net->heap_len > 0 && net->heap[0].at_us <= until_us) {
        vnode_t *v;
        heap_pop(net, &ev);
        net->now_us = ev.at_us;
        v = net->nodes[ev.node];
        if (!v->powered) continue;
        switch (ev.kind) {
        case 0:
            node_poll(&v->node);
            if (net->trace && v->node.shard_valid) {
                uint32_t tk;
                if (node_current_tick(&v->node, &tk)) {
                    int32_t lag = (int32_t)(tk - v->node.shard.tick);
                    if ((lag > 2 || lag < -1) && !v->lagging) {
                        v->lagging = true;
                        fprintf(stderr, "[%9.3f] n%d: tick lag %ld (shard %lu, cluster %lu) state %s valid %d\n",
                                (double)net->now_us / 1e6, v->index, (long)lag,
                                (unsigned long)v->node.shard.tick, (unsigned long)tk,
                                node_state_name(v->node.state), v->node.cluster_valid);
                    } else if (lag <= 2 && lag >= -1 && v->lagging) {
                        v->lagging = false;
                        fprintf(stderr, "[%9.3f] n%d: tick lag cleared (late %u early %u)\n",
                                (double)net->now_us / 1e6, v->index, v->node.tm.events_late,
                                v->node.tm.events_early);
                    }
                }
            }
            ev.at_us += VNET_POLL_US;
            heap_push(net, &ev);
            break;
        case 1: {
            unsigned i;
            bool collided = false;
            uint64_t rx;
            for (i = 0; i < net->n_air; i++)
                if (net->air[i].id == ev.tx_id) collided = net->air[i].collided;
            if (collided || rng_below(&net->rng, 10000u) < net->loss_pct_x100) {
                v->frames_lost++;
                net->losses++;
                break;
            }
            v->frames_rx++;
            rx = vnet_local_time(v, ev.at_us);
            if (!v->hw_ts) rx = vnet_local_time(v, ev.at_us + rng_below(&net->rng, VNET_SOFT_TS_US));
            node_on_frame(&v->node, net->nodes[ev.from]->mac, ev.frame, ev.len, rx, v->hw_ts);
            break;
        }
        case 2:
            node_on_sent(&v->node, ev.tag, vnet_local_time(v, ev.at_us));
            break;
        default:
            break;
        }
    }
    if (net->now_us < until_us) net->now_us = until_us;
}

/* ---- observations ----------------------------------------------------- */

int vnet_coordinator(const vnet_t *net)
{
    int i, coord = -1;
    uint16_t coord_id = 0;

    for (i = 0; i < net->n_nodes; i++) {
        const vnode_t *v = net->nodes[i];
        if (!v->powered) continue;
        if (v->node.state == NODE_STATE_COORDINATOR) {
            if (coord >= 0) return -1;
            coord = i;
            coord_id = v->node.node_id;
        }
    }
    if (coord < 0) return -1;
    for (i = 0; i < net->n_nodes; i++) {
        const vnode_t *v = net->nodes[i];
        if (!v->powered || i == coord) continue;
        if (v->node.state != NODE_STATE_MEMBER || !v->node.have_coord || v->node.coord_id != coord_id)
            return -1;
    }
    return coord;
}

bool vnet_converged(const vnet_t *net)
{
    return vnet_coordinator(net) >= 0;
}

int32_t vnet_sync_error_us(const vnet_t *net, int idx)
{
    int c = vnet_coordinator(net);
    const vnode_t *v = net->nodes[idx];
    uint64_t est, truth;

    if (c < 0 || !v->powered) return INT32_MIN;
    if (!node_cluster_time(&v->node, vnet_local_time(v, net->now_us), &est)) return INT32_MIN;
    truth = vnet_local_time(net->nodes[c], net->now_us);
    return (int32_t)((int64_t)est - (int64_t)truth);
}
