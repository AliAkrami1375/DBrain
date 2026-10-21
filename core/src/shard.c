#include <string.h>
#include "dbrain/shard.h"
#include "dbrain/synapse.h"

static size_t round4(size_t x)
{
    return (x + 3u) & ~(size_t)3u;
}

size_t shard_arena_size(uint16_t n_neurons)
{
    size_t n = n_neurons;
    return round4(n * sizeof(neuron_state_t)) +
           round4(SHARD_DELAY_SLOTS * n * sizeof(accum_t)) +
           round4(n * sizeof(uint16_t));
}

int shard_bind(shard_t *sh, const void *image, uint32_t size, void *arena, size_t arena_size,
               telemetry_t *tm)
{
    int err;
    uint8_t *p = (uint8_t *)arena;
    size_t n;

    memset(sh, 0, sizeof(*sh));
    err = artifact_parse(image, size, &sh->art);
    if (err) return err;
    n = sh->art.n_neurons;
    if (arena == NULL || (((uintptr_t)arena) & 3u) != 0) return ARTIFACT_E_ALIGN;
    if (arena_size < shard_arena_size(sh->art.n_neurons)) return ARTIFACT_E_NOSPACE;

    sh->state = (neuron_state_t *)(void *)p;
    p += round4(n * sizeof(neuron_state_t));
    sh->buckets = (accum_t *)(void *)p;
    p += round4(SHARD_DELAY_SLOTS * n * sizeof(accum_t));
    sh->fired = (uint16_t *)(void *)p;
    sh->tm = tm ? tm : &sh->tm_own;
    if (!tm) telemetry_reset(&sh->tm_own);
    shard_reset(sh);
    return ARTIFACT_OK;
}

int shard_rebind(shard_t *sh, const void *image, uint32_t size)
{
    artifact_view_t nv;
    int err = artifact_parse(image, size, &nv);
    bool same;
    uint16_t k;

    if (err) { sh->art.n_neurons = 0; return err; }
    same = nv.n_neurons == sh->art.n_neurons && nv.n_populations == sh->art.n_populations;
    for (k = 0; same && k <= nv.n_populations; k++)
        if (nv.pop_first[k] != sh->art.pop_first[k]) same = false;
    for (k = 0; same && k < nv.n_populations; k++)
        if (memcmp(&nv.pops[k], &sh->art.pops[k], sizeof(neuron_params_t)) != 0) same = false;
    sh->art = nv;
    if (!same) {
        size_t n = nv.n_neurons;
        uint8_t *p = (uint8_t *)sh->state;
        sh->state = (neuron_state_t *)(void *)p;
        p += round4(n * sizeof(neuron_state_t));
        sh->buckets = (accum_t *)(void *)p;
        p += round4(SHARD_DELAY_SLOTS * n * sizeof(accum_t));
        sh->fired = (uint16_t *)(void *)p;
        shard_reset(sh);
    }
    return ARTIFACT_OK;
}

void shard_reset(shard_t *sh)
{
    uint16_t k, i;
    const artifact_view_t *a = &sh->art;

    for (k = 0; k < a->n_populations; k++)
        for (i = a->pop_first[k]; i < a->pop_first[k + 1]; i++)
            neuron_init(&sh->state[i], &a->pops[k]);
    memset(sh->buckets, 0, SHARD_DELAY_SLOTS * (size_t)a->n_neurons * sizeof(accum_t));
    sh->n_fired = 0;
    sh->tick = 0;
}

/* Walks one row and adds each synapse into the bucket of its delivery tick. */
static void apply_row(shard_t *sh, const uint32_t *w, uint32_t words, uint32_t src_tick)
{
    uint32_t i = 0;
    uint32_t t = sh->tick;
    uint16_t n = sh->art.n_neurons;
    telemetry_t *tm = sh->tm;

    while (i < words) {
        uint32_t h = w[i++];
        uint16_t base = syn_seg_base(h);
        uint8_t count = syn_seg_count(h);
        uint8_t shift = syn_seg_shift(h);
        uint8_t k;

        for (k = 0; k < count; k++) {
            uint32_t s = w[i + k];
            uint32_t due = src_tick + syn_delay(s);
            accum_t *slot;

            if (due < t) { tm->events_late++; continue; }
            if (due - t >= SHARD_DELAY_SLOTS) { tm->events_early++; continue; }
            slot = sh->buckets + (due & (SHARD_DELAY_SLOTS - 1u)) * n;
            slot[base + syn_target(s)] = fix_add_sat(slot[base + syn_target(s)],
                                                     syn_weight_accum(syn_weight(s), shift));
            tm->events_applied++;
        }
        i += count;
    }
}

static void deliver_gid(shard_t *sh, uint32_t gid, uint32_t src_tick)
{
    const sub_entry_t *e = artifact_find_row(&sh->art, gid);

    if (e == NULL) return;
    apply_row(sh, sh->art.rows + e->row_offset, e->row_words, src_tick);
}

uint16_t shard_tick(shard_t *sh)
{
    const artifact_view_t *a = &sh->art;
    uint32_t t = sh->tick;
    accum_t *bucket = sh->buckets + (t & (SHARD_DELAY_SLOTS - 1u)) * a->n_neurons;
    uint16_t k, i, nf = 0;

    for (k = 0; k < a->n_populations; k++) {
        const neuron_params_t *p = &a->pops[k];
        for (i = a->pop_first[k]; i < a->pop_first[k + 1]; i++) {
            if (neuron_step(&sh->state[i], p, bucket[i])) sh->fired[nf++] = i;
            bucket[i] = 0;
        }
    }
    sh->n_fired = nf;
    sh->tick = t + 1u;

    for (i = 0; i < nf; i++)
        deliver_gid(sh, gid_make(a->shard_id, sh->fired[i]), t);
    return nf;
}

int shard_subscribes(const shard_t *sh, uint16_t src_shard)
{
    return artifact_subscribes(&sh->art, src_shard);
}

void shard_deliver(shard_t *sh, uint16_t src_shard, uint32_t src_tick, const uint16_t *events,
                   uint16_t n)
{
    uint16_t i;

    for (i = 0; i < n; i++) {
        const sub_entry_t *e = artifact_find_row(&sh->art, gid_make(src_shard, events[i]));
        if (e == NULL) { sh->tm->events_norow++; continue; }
        apply_row(sh, sh->art.rows + e->row_offset, e->row_words, src_tick);
    }
}

int shard_inject(shard_t *sh, uint16_t neuron, accum_t current, uint32_t at_tick)
{
    uint32_t t = sh->tick;
    accum_t *slot;

    if (neuron >= sh->art.n_neurons) return -1;
    if (at_tick < t || at_tick - t >= SHARD_DELAY_SLOTS) return -2;
    slot = sh->buckets + (at_tick & (SHARD_DELAY_SLOTS - 1u)) * sh->art.n_neurons;
    slot[neuron] = fix_add_sat(slot[neuron], current);
    return 0;
}

/* ---- opaque accessors ------------------------------------------------- */

size_t shard_sizeof(void)                               { return sizeof(shard_t); }
uint32_t shard_tick_get(const shard_t *sh)              { return sh->tick; }
const uint16_t *shard_fired_get(const shard_t *sh, uint16_t *n) { *n = sh->n_fired; return sh->fired; }
telemetry_t *shard_telemetry(shard_t *sh)               { return sh->tm; }
const neuron_state_t *shard_state_get(const shard_t *sh) { return sh->state; }
uint16_t shard_neurons(const shard_t *sh)               { return sh->art.n_neurons; }
uint16_t shard_id(const shard_t *sh)                    { return sh->art.shard_id; }
