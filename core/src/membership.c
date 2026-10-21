#include <string.h>
#include "dbrain/membership.h"
#include "dbrain/frame.h"

/* ---- capability ------------------------------------------------------- */

void cap_encode(uint8_t *p, const cap_t *c)
{
    p[0] = c->chip;
    p[1] = c->cores;
    wr16(p + 2, c->mhz);
    wr32(p + 4, c->free_heap);
    wr32(p + 8, c->psram);
    p[12] = c->flags;
    p[13] = c->tier;
    wr16(p + 14, c->reserved);
}

void cap_decode(const uint8_t *p, cap_t *c)
{
    c->chip = p[0];
    c->cores = p[1];
    c->mhz = rd16(p + 2);
    c->free_heap = rd32(p + 4);
    c->psram = rd32(p + 8);
    c->flags = p[12];
    c->tier = p[13];
    c->reserved = rd16(p + 14);
}

static uint32_t cap32(uint32_t v, uint32_t max)
{
    return v > max ? max : v;
}

/*
 * bits 31..28 tier, 27..20 PSRAM in MB (capped), 19..8 MHz (capped),
 * 7..4 cores (capped), 3..0 chip id. free_heap is deliberately excluded so
 * the key does not move while a node runs.
 */
uint32_t cap_score(const cap_t *c)
{
    return (cap32(c->tier, 15u) << 28) | (cap32(c->psram >> 20, 255u) << 20) |
           (cap32(c->mhz, 4095u) << 8) | (cap32(c->cores, 15u) << 4) | cap32(c->chip, 15u);
}

void cap_classify(cap_t *c)
{
    switch (c->chip) {
    case DBRAIN_CHIP_ESP8266:
    case DBRAIN_CHIP_ESP32C2:
        c->tier = TIER_LEAF;
        c->flags &= (uint8_t)~CAP_FLAG_ELIGIBLE;
        break;
    case DBRAIN_CHIP_ESP32S2:
    case DBRAIN_CHIP_ESP32C3:
    case DBRAIN_CHIP_ESP32C5:
    case DBRAIN_CHIP_ESP32C6:
        c->tier = TIER_MID;
        c->flags |= CAP_FLAG_ELIGIBLE;
        break;
    case DBRAIN_CHIP_ESP32:
    case DBRAIN_CHIP_ESP32S3:
    case DBRAIN_CHIP_HOST:
        c->tier = (c->flags & CAP_FLAG_PSRAM) ? TIER_HEAVY : TIER_WORKHORSE;
        c->flags |= CAP_FLAG_ELIGIBLE;
        break;
    default:
        c->tier = TIER_LEAF;
        c->flags &= (uint8_t)~CAP_FLAG_ELIGIBLE;
        break;
    }
    if (c->chip == DBRAIN_CHIP_ESP8266) c->flags &= (uint8_t)~CAP_FLAG_HW_RX_TS;
}

/*
 * Half of the free heap is the simulation budget. A neuron costs 8 bytes of
 * state, 64 bytes of delay buckets, 2 bytes of spike queue, and fan_in
 * synapse words plus a share of row headers (4 bytes per synapse and a 10%
 * allowance). Rows of recipe networks live in RAM; with PSRAM the rows go
 * there and the internal budget only pays for state.
 */
uint16_t cap_neuron_budget(const cap_t *c, uint16_t fan_in)
{
    uint32_t budget = c->free_heap / 2u;
    uint32_t per_neuron = 8u + 64u + 2u;
    uint32_t n;

    if (!(c->flags & CAP_FLAG_PSRAM)) per_neuron += (4u * (uint32_t)fan_in * 11u) / 10u;
    n = budget / per_neuron;
    if (c->flags & CAP_FLAG_PSRAM) {
        uint32_t rows = (c->psram / 2u) / ((4u * (uint32_t)fan_in * 11u) / 10u + 1u);
        if (rows < n) n = rows;
    }
    if (n < 16u) n = 16u;
    if (n > 2000u) n = 2000u;
    return (uint16_t)n;
}

/* ---- heartbeat -------------------------------------------------------- */

size_t heartbeat_encode(uint8_t *out, size_t cap, const heartbeat_t *h)
{
    if (cap < HEARTBEAT_LEN) return 0;
    out[0] = FRAME_MAGIC;
    out[1] = frame_ver_type(FRAME_HEARTBEAT);
    wr16(out + 2, h->cluster_id);
    wr16(out + 4, h->node_id);
    wr_mac(out + 6, h->mac);
    wr16(out + 12, h->epoch);
    wr16(out + 14, h->coord_id);
    wr16(out + 16, h->shard_id);
    wr16(out + 18, h->shard_version);
    wr16(out + 20, h->n_neurons);
    cap_encode(out + 22, &h->cap);
    out[38] = h->state;
    out[39] = h->flags;
    wr16(out + 40, h->hb_seq);
    wr32(out + 42, h->tick);
    wr16(out + 46, h->drops);
    wr16(out + 48, h->overruns);
    wr16(out + 50, (uint16_t)h->sync_residual_us);
    wr16(out + 52, h->reserved);
    wr16(out + 54, 0);
    return HEARTBEAT_LEN;
}

bool heartbeat_decode(const uint8_t *in, size_t len, heartbeat_t *h)
{
    if (frame_type(in, len) != FRAME_HEARTBEAT || len != HEARTBEAT_LEN) return false;
    h->cluster_id = rd16(in + 2);
    h->node_id = rd16(in + 4);
    rd_mac(in + 6, h->mac);
    h->epoch = rd16(in + 12);
    h->coord_id = rd16(in + 14);
    h->shard_id = rd16(in + 16);
    h->shard_version = rd16(in + 18);
    h->n_neurons = rd16(in + 20);
    cap_decode(in + 22, &h->cap);
    h->state = in[38];
    h->flags = in[39];
    h->hb_seq = rd16(in + 40);
    h->tick = rd32(in + 42);
    h->drops = rd16(in + 46);
    h->overruns = rd16(in + 48);
    h->sync_residual_us = (int16_t)rd16(in + 50);
    h->reserved = rd16(in + 52);
    if (h->node_id > NODE_ID_MAX) return false;
    if (h->state > NODE_STATE_COORDINATOR) return false;
    return true;
}

/* ---- table ------------------------------------------------------------ */

static bool active(const member_t *m)
{
    return (m->liveness == MEMBER_ALIVE || m->liveness == MEMBER_SUSPECT) && m->node_id != NODE_ID_NONE;
}


void membership_init(membership_t *t)
{
    memset(t, 0, sizeof(*t));
}

member_t *membership_find_mac(membership_t *t, const uint8_t mac[6])
{
    unsigned i;
    for (i = 0; i < MEMBERSHIP_MAX; i++)
        if (t->m[i].liveness != MEMBER_EMPTY && memcmp(t->m[i].mac, mac, 6) == 0) return &t->m[i];
    return NULL;
}

member_t *membership_find_id(membership_t *t, uint16_t node_id)
{
    unsigned i;
    if (node_id == NODE_ID_NONE) return NULL;
    for (i = 0; i < MEMBERSHIP_MAX; i++)
        if (t->m[i].liveness != MEMBER_EMPTY && t->m[i].liveness != MEMBER_DEAD &&
            t->m[i].node_id == node_id)
            return &t->m[i];
    return NULL;
}

static member_t *alloc_entry(membership_t *t)
{
    unsigned i;
    for (i = 0; i < MEMBERSHIP_MAX; i++)
        if (t->m[i].liveness == MEMBER_EMPTY) { t->count++; return &t->m[i]; }
    return NULL;
}

member_t *membership_update(membership_t *t, const heartbeat_t *h, uint32_t now_ms)
{
    member_t *m = membership_find_mac(t, h->mac);
    bool changed = false;

    if (m == NULL) {
        m = alloc_entry(t);
        if (m == NULL) return NULL;
        memset(m, 0, sizeof(*m));
        memcpy(m->mac, h->mac, 6);
        changed = true;
    }
    if (m->liveness != MEMBER_ALIVE) changed = true;
    if (m->node_id != h->node_id) changed = true;
    if (m->node_state != h->state || m->flags != h->flags) changed = true;
    if (m->shard_id != h->shard_id || m->shard_version != h->shard_version ||
        m->n_neurons != h->n_neurons)
        changed = true;

    m->liveness = MEMBER_ALIVE;
    m->node_state = h->state;
    m->node_id = h->node_id;
    m->epoch = h->epoch;
    m->coord_id = h->coord_id;
    m->shard_id = h->shard_id;
    m->shard_version = h->shard_version;
    m->n_neurons = h->n_neurons;
    m->cap = h->cap;
    m->flags = h->flags;
    m->probes = 0;
    m->hb_seq = h->hb_seq;
    m->tick = h->tick;
    m->last_heard_ms = now_ms;
    m->drops = h->drops;
    m->overruns = h->overruns;
    m->sync_residual_us = h->sync_residual_us;
    if (changed) t->version++;
    return m;
}

bool membership_tick(membership_t *t, uint32_t now_ms)
{
    unsigned i;
    bool changed = false;

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        member_t *m = &t->m[i];
        uint32_t silent = now_ms - m->last_heard_ms;

        switch (m->liveness) {
        case MEMBER_ALIVE:
            if (silent >= HB_SUSPECT_MS) {
                m->liveness = MEMBER_SUSPECT;
                m->suspect_since_ms = now_ms;
                m->probes = 0;
                changed = true;
            }
            break;
        case MEMBER_SUSPECT:
            if (now_ms - m->suspect_since_ms >= HB_PROBE_TIMEOUT_MS) {
                m->liveness = MEMBER_DEAD;
                m->last_heard_ms = now_ms;
                changed = true;
            }
            break;
        case MEMBER_DEAD:
            if (silent >= HB_DEAD_RETAIN_MS) {
                memset(m, 0, sizeof(*m));
                t->count--;
                changed = true;
            }
            break;
        default:
            break;
        }
    }
    if (changed) t->version++;
    return changed;
}

void membership_confirm(membership_t *t, member_t *m, uint32_t now_ms)
{
    if (m->liveness != MEMBER_ALIVE) t->version++;
    m->liveness = MEMBER_ALIVE;
    m->last_heard_ms = now_ms;
    m->probes = 0;
}

void membership_declare_dead(membership_t *t, member_t *m, uint32_t now_ms)
{
    if (m->liveness == MEMBER_EMPTY || m->liveness == MEMBER_DEAD) return;
    m->liveness = MEMBER_DEAD;
    m->last_heard_ms = now_ms;
    t->version++;
}

uint16_t membership_active(const membership_t *t)
{
    unsigned i;
    uint16_t n = 0;
    for (i = 0; i < MEMBERSHIP_MAX; i++)
        if (active(&t->m[i])) n++;
    return n;
}

int membership_rank(const membership_t *t, uint16_t node_id)
{
    unsigned i;
    int rank = 0;
    bool found = false;

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        const member_t *m = &t->m[i];
        if (!active(m)) continue;
        if (m->node_id == node_id) found = true;
        else if (m->node_id < node_id) rank++;
    }
    return found ? rank : -1;
}

uint16_t membership_free_id(const membership_t *t)
{
    uint16_t id;
    for (id = 1; id <= NODE_ID_MAX; id++) {
        unsigned i;
        bool used = false;
        for (i = 0; i < MEMBERSHIP_MAX; i++)
            if (t->m[i].liveness != MEMBER_EMPTY && t->m[i].node_id == id) { used = true; break; }
        if (!used) return id;
    }
    return NODE_ID_NONE;
}

uint16_t membership_max_epoch(const membership_t *t)
{
    unsigned i;
    uint16_t e = 0;
    for (i = 0; i < MEMBERSHIP_MAX; i++)
        if (t->m[i].liveness != MEMBER_EMPTY && t->m[i].epoch > e) e = t->m[i].epoch;
    return e;
}

/* Order-independent hash: entries are combined by addition, so slot order is irrelevant. */
static uint32_t mix32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

uint32_t membership_roster_signature(const membership_t *t)
{
    uint32_t h = 0x811C9DC5u;
    unsigned i;

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        const member_t *m = &t->m[i];
        if (!active(m) || !(m->flags & HB_FLAG_RECIPE) || m->n_neurons == 0) continue;
        h += mix32(((uint32_t)m->node_id << 16) | m->n_neurons);
    }
    return h;
}

uint32_t membership_plan_signature(const membership_t *t)
{
    uint32_t h = 0x811C9DC5u;
    unsigned i;

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        const member_t *m = &t->m[i];
        if (!active(m)) continue;
        h += mix32(((uint32_t)m->node_id << 16) | ((uint32_t)m->cap.chip << 8) |
                   (m->cap.flags & CAP_FLAG_HW_RX_TS));
    }
    return h;
}

int election_compare(uint16_t epoch_a, uint32_t score_a, const uint8_t mac_a[6],
                     uint16_t epoch_b, uint32_t score_b, const uint8_t mac_b[6])
{
    if (epoch_a != epoch_b) return epoch_a > epoch_b ? 1 : -1;
    if (score_a != score_b) return score_a > score_b ? 1 : -1;
    return memcmp(mac_a, mac_b, 6);
}

member_t *membership_best_coordinator(membership_t *t)
{
    unsigned i;
    member_t *best = NULL;

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        member_t *m = &t->m[i];
        if (m->liveness != MEMBER_ALIVE && m->liveness != MEMBER_SUSPECT) continue;
        if (!(m->cap.flags & CAP_FLAG_ELIGIBLE)) continue;
        if (!(m->flags & HB_FLAG_COORDINATOR)) continue;
        if (best == NULL ||
            election_compare(m->epoch, cap_score(&m->cap), m->mac,
                             best->epoch, cap_score(&best->cap), best->mac) > 0)
            best = m;
    }
    return best;
}
