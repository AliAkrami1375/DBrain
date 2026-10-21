#include <string.h>
#include "dbrain/recipe.h"
#include "dbrain/artifact.h"
#include "dbrain/synapse.h"
#include "dbrain/frame.h"
#include "dbrain/rng.h"

void recipe_default(recipe_t *r)
{
    r->seed = 1;
    r->topology = RECIPE_RANDOM;
    r->exc_percent = 80;
    r->w_exc = 64;          /* 0.5 */
    r->w_inh = -128;        /* -1.0 */
    r->local_fan_in = 20;
    r->remote_fan_in = 10;
    r->drive = 12 * 128;    /* up to 12.0 per tick: mean 6, enough to keep RS cells active */
}

void recipe_encode(uint8_t *out, const recipe_t *r)
{
    wr32(out, r->seed);
    out[4] = r->topology;
    out[5] = r->exc_percent;
    wr16(out + 6, (uint16_t)r->w_exc);
    wr16(out + 8, (uint16_t)r->w_inh);
    wr16(out + 10, r->local_fan_in);
    wr16(out + 12, r->remote_fan_in);
    wr16(out + 14, r->drive);
}

bool recipe_decode(const uint8_t *in, recipe_t *r)
{
    r->seed = rd32(in);
    r->topology = in[4];
    r->exc_percent = in[5];
    r->w_exc = (int16_t)rd16(in + 6);
    r->w_inh = (int16_t)rd16(in + 8);
    r->local_fan_in = rd16(in + 10);
    r->remote_fan_in = rd16(in + 12);
    r->drive = rd16(in + 14);
    return r->topology <= RECIPE_SYNFIRE && r->exc_percent <= 100;
}

uint16_t recipe_neurons_for(const recipe_t *r, const cap_t *cap)
{
    uint32_t fan = (uint32_t)r->local_fan_in + r->remote_fan_in;
    if (fan > 65535u) fan = 65535u;
    return cap_neuron_budget(cap, (uint16_t)fan);
}

uint16_t recipe_exc_count(const recipe_t *r, uint16_t n)
{
    return (uint16_t)(((uint32_t)n * r->exc_percent) / 100u);
}

/* Virtual coordinate of a node on a 1024 x 1024 grid, from its id. */
static void node_coord(uint32_t seed, uint16_t id, uint32_t *x, uint32_t *y)
{
    uint32_t h = rng_mix(seed ^ ((uint32_t)id * 0x9E3779B1u));
    *x = h & 1023u;
    *y = (h >> 10) & 1023u;
}

/* Fan-out multiplier in 1/256 for the spatial topology: 1 at distance 0, 0 beyond 768. */
static uint32_t spatial_factor(uint32_t seed, uint16_t a, uint16_t b)
{
    uint32_t ax, ay, bx, by, dx, dy, d2, d;
    node_coord(seed, a, &ax, &ay);
    node_coord(seed, b, &bx, &by);
    dx = ax > bx ? ax - bx : bx - ax;
    dy = ay > by ? ay - by : by - ay;
    d2 = dx * dx + dy * dy;
    /* integer square root */
    d = 0;
    {
        uint32_t bit = 1u << 20;
        while (bit > d2) bit >>= 2;
        while (bit) {
            if (d2 >= d + bit) { d2 -= d + bit; d = (d >> 1) + bit; }
            else d >>= 1;
            bit >>= 2;
        }
    }
    if (d >= 768u) return 0;
    return ((768u - d) * 256u) / 768u;
}

static void sort_u16(uint16_t *v, unsigned n)
{
    unsigned i, j;
    for (i = 1; i < n; i++) {
        uint16_t key = v[i];
        j = i;
        while (j > 0 && v[j - 1] > key) { v[j] = v[j - 1]; j--; }
        v[j] = key;
    }
}

/*
 * Fan-out of one source neuron on `src` toward `dst`: the target node's
 * fan-in from this source, scaled by the size ratio so the expected fan-in
 * per target neuron matches the recipe. Returns synapses per source neuron.
 */
static uint32_t fan_out(const recipe_t *r, uint16_t src, uint16_t n_src, uint16_t dst, uint16_t n_dst,
                        uint16_t n_members, uint16_t src_rank, uint16_t dst_rank)
{
    uint32_t fan_in;

    if (src == dst) {
        fan_in = r->local_fan_in;
    } else if (r->topology == RECIPE_SYNFIRE) {
        /* pools form a ring in id order; only the predecessor projects */
        if (n_members < 2 || (uint16_t)((src_rank + 1u) % n_members) != dst_rank) return 0;
        fan_in = r->remote_fan_in;
    } else {
        if (n_members < 2) return 0;
        fan_in = ((uint32_t)r->remote_fan_in + (n_members - 2u)) / (n_members - 1u);
        if (r->topology == RECIPE_SPATIAL)
            fan_in = (fan_in * spatial_factor(r->seed, src, dst) + 128u) / 256u;
    }
    if (fan_in == 0 || n_src == 0) return 0;
    return (fan_in * n_dst + n_src / 2u) / n_src;
}

int32_t recipe_build(const recipe_t *r, uint16_t self_id, uint16_t self_version,
                     const recipe_member_t *members, uint16_t n_members, void *buf, uint32_t cap)
{
    artifact_builder_t b;
    neuron_params_t p;
    uint16_t n_self = 0, exc_self, i, k, self_rank = 0;
    int err;

    for (i = 0; i < n_members; i++) {
        if (i > 0 && members[i].node_id <= members[i - 1].node_id) return ARTIFACT_E_ORDER;
        if (members[i].node_id == self_id) { n_self = members[i].n_neurons; self_rank = i; }
    }
    if (n_self == 0) return ARTIFACT_E_ARG;
    exc_self = recipe_exc_count(r, n_self);

    err = artifact_build_begin(&b, buf, cap, self_id, self_version);
    if (err) return err;
    /* regular spiking excitatory cells, fast spiking inhibitory cells */
    if (exc_self > 0) {
        neuron_params_izh(&p, FIX_RATIO(2, 100), FIX_RATIO(2, 10), FIX_INT(-65), FIX_INT(8), 0);
        err = artifact_build_population(&b, &p, exc_self);
        if (err < 0) return err;
    }
    if (n_self > exc_self) {
        neuron_params_izh(&p, FIX_RATIO(1, 10), FIX_RATIO(2, 10), FIX_INT(-65), FIX_INT(2), 0);
        err = artifact_build_population(&b, &p, (uint16_t)(n_self - exc_self));
        if (err < 0) return err;
    }

    for (k = 0; k < n_members; k++) {
        uint16_t src = members[k].node_id, n_src = members[k].n_neurons;
        uint16_t exc_src = recipe_exc_count(r, n_src);
        uint32_t fo = fan_out(r, src, n_src, self_id, n_self, n_members, k, self_rank);
        uint8_t delay_min = (src == self_id) ? 1u : 2u;
        uint8_t delay_span = (uint8_t)(9u - delay_min);   /* delays delay_min .. 8 */

        if (fo == 0) continue;
        if (fo > SYN_SEG_MAX_COUNT) fo = SYN_SEG_MAX_COUNT;
        for (i = 0; i < n_src; i++) {
            uint16_t targets[SYN_SEG_MAX_COUNT];
            rng_t g;
            uint32_t j;
            int16_t w = i < exc_src ? r->w_exc : r->w_inh;

            rng_seed(&g, r->seed, ((uint32_t)src << 16) | i, self_id);
            for (j = 0; j < fo; j++) targets[j] = (uint16_t)rng_below(&g, n_self);
            sort_u16(targets, fo);
            err = artifact_build_row_begin(&b, gid_make(src, i), SYN_WEIGHT_SHIFT_DEFAULT);
            if (err) return err;
            for (j = 0; j < fo; j++) {
                uint8_t d = (uint8_t)(delay_min + rng_below(&g, delay_span));
                err = artifact_build_synapse(&b, targets[j], w, d, 0);
                if (err) return err;
            }
            err = artifact_build_row_end(&b);
            if (err) return err;
        }
    }
    return artifact_build_finish(&b);
}

void recipe_drive(const recipe_t *r, shard_t *sh)
{
    rng_t g;
    uint16_t i, n = sh->art.n_neurons;
    uint32_t span = (uint32_t)r->drive << 8;   /* shift 8 -> s16.15 */

    if (r->drive == 0) return;
    rng_seed(&g, r->seed ^ 0xD71Fu, sh->art.shard_id, sh->tick);
    for (i = 0; i < n; i++)
        shard_inject(sh, i, (accum_t)rng_below(&g, span), sh->tick);
}
