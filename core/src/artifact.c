#include <string.h>
#include "dbrain/artifact.h"
#include "dbrain/synapse.h"
#include "dbrain/frame.h"
#include "dbrain/crc32.h"
#include "dbrain/version.h"

static const uint8_t ARTIFACT_MAGIC[4] = { 'D', 'B', 'R', 'A' };

static uint32_t align4(uint32_t x)
{
    return (x + 3u) & ~3u;
}

static uint32_t image_crc(const uint8_t *image, uint32_t size)
{
    uint8_t zero[4] = { 0, 0, 0, 0 };
    uint32_t crc = crc32_update(0, image, 28);
    crc = crc32_update(crc, zero, 4);
    return crc32_update(crc, image + ARTIFACT_HDR_LEN, size - ARTIFACT_HDR_LEN);
}

const char *artifact_strerror(int err)
{
    switch (err) {
    case ARTIFACT_OK:        return "ok";
    case ARTIFACT_E_SIZE:    return "size";
    case ARTIFACT_E_MAGIC:   return "magic";
    case ARTIFACT_E_VERSION: return "format version";
    case ARTIFACT_E_MODEL:   return "model version";
    case ARTIFACT_E_CRC:     return "crc";
    case ARTIFACT_E_POPS:    return "populations";
    case ARTIFACT_E_SUBS:    return "subscriptions";
    case ARTIFACT_E_ROWS:    return "rows";
    case ARTIFACT_E_ALIGN:   return "alignment";
    case ARTIFACT_E_NOSPACE: return "no space";
    case ARTIFACT_E_ORDER:   return "order";
    case ARTIFACT_E_ARG:     return "argument";
    default:                 return "unknown";
    }
}

/* Validates one row's segments. Returns 0 or ARTIFACT_E_ROWS. */
static int check_row(const uint32_t *w, uint32_t words, uint16_t n_neurons)
{
    uint32_t i = 0;

    if (words == 0) return ARTIFACT_E_ROWS;
    while (i < words) {
        uint32_t h = w[i++];
        uint16_t base = syn_seg_base(h);
        uint8_t count = syn_seg_count(h);
        uint8_t k;

        if (count == 0 || syn_seg_shift(h) > SYN_WEIGHT_SHIFT_MAX) return ARTIFACT_E_ROWS;
        if (i + count > words) return ARTIFACT_E_ROWS;
        for (k = 0; k < count; k++) {
            uint32_t s = w[i + k];
            if (syn_delay(s) < SYN_DELAY_MIN) return ARTIFACT_E_ROWS;
            if ((uint32_t)base + syn_target(s) >= n_neurons) return ARTIFACT_E_ROWS;
        }
        i += count;
    }
    return 0;
}

int artifact_parse(const void *image_v, uint32_t size, artifact_view_t *v)
{
    const uint8_t *image = (const uint8_t *)image_v;
    uint32_t off, pops_len, popfirst_len, rows_len, subs_len, total;
    uint32_t i, pos, n_found;
    const uint32_t *rows;
    const sub_entry_t *subs;

    memset(v, 0, sizeof(*v));
    if (((uintptr_t)image & 3u) != 0) return ARTIFACT_E_ALIGN;
    if (size < ARTIFACT_HDR_LEN || size > ARTIFACT_MAX_SIZE) return ARTIFACT_E_SIZE;
    if (memcmp(image, ARTIFACT_MAGIC, 4) != 0) return ARTIFACT_E_MAGIC;
    if (rd16(image + 4) != DBRAIN_ARTIFACT_VERSION) return ARTIFACT_E_VERSION;
    if (rd16(image + 6) != DBRAIN_MODEL_VERSION) return ARTIFACT_E_MODEL;

    v->image = image;
    v->shard_id = rd16(image + 8);
    v->shard_version = rd16(image + 10);
    v->n_neurons = rd16(image + 12);
    v->n_populations = rd16(image + 14);
    v->n_subs = rd32(image + 16);
    v->n_row_words = rd32(image + 20);
    total = rd32(image + 24);
    if (total != size) return ARTIFACT_E_SIZE;
    v->size = size;

    if (v->n_neurons == 0 || v->n_populations == 0 || v->n_populations > ARTIFACT_MAX_POPS)
        return ARTIFACT_E_POPS;

    pops_len = (uint32_t)v->n_populations * (uint32_t)sizeof(neuron_params_t);
    popfirst_len = align4(((uint32_t)v->n_populations + 1u) * 2u);
    rows_len = v->n_row_words * 4u;
    subs_len = v->n_subs * (uint32_t)sizeof(sub_entry_t);
    if (v->n_row_words > ARTIFACT_MAX_SIZE / 4u || v->n_subs > ARTIFACT_MAX_SIZE / sizeof(sub_entry_t))
        return ARTIFACT_E_SIZE;
    off = ARTIFACT_HDR_LEN;
    if ((uint64_t)off + pops_len + popfirst_len + rows_len + subs_len != size) return ARTIFACT_E_SIZE;

    if (rd32(image + 28) != image_crc(image, size)) return ARTIFACT_E_CRC;

    v->pops = (const neuron_params_t *)(const void *)(image + off);
    off += pops_len;
    v->pop_first = (const uint16_t *)(const void *)(image + off);
    off += popfirst_len;
    v->rows = (const uint32_t *)(const void *)(image + off);
    off += rows_len;
    v->subs = (const sub_entry_t *)(const void *)(image + off);

    /* populations */
    if (v->pop_first[0] != 0) return ARTIFACT_E_POPS;
    for (i = 0; i < v->n_populations; i++) {
        if (v->pop_first[i + 1] <= v->pop_first[i]) return ARTIFACT_E_POPS;
        if (v->pops[i].model != NEURON_MODEL_IZH && v->pops[i].model != NEURON_MODEL_LIF)
            return ARTIFACT_E_POPS;
    }
    if (v->pop_first[v->n_populations] != v->n_neurons) return ARTIFACT_E_POPS;

    /* row stream: [gid][len][segments...] repeated; must match subs exactly */
    rows = v->rows;
    subs = v->subs;
    pos = 0;
    n_found = 0;
    while (pos < v->n_row_words) {
        uint32_t gid, len;
        int err;

        if (pos + 2u > v->n_row_words) return ARTIFACT_E_ROWS;
        gid = rows[pos];
        len = rows[pos + 1];
        if (len == 0 || len > 0xFFFFu || pos + 2u + len > v->n_row_words) return ARTIFACT_E_ROWS;
        err = check_row(rows + pos + 2u, len, v->n_neurons);
        if (err) return err;
        if (n_found >= v->n_subs) return ARTIFACT_E_SUBS;
        if (subs[n_found].gid != gid || subs[n_found].row_offset != pos + 2u ||
            subs[n_found].row_words != len)
            return ARTIFACT_E_SUBS;
        if (n_found > 0 && subs[n_found - 1].gid >= gid) return ARTIFACT_E_SUBS;
        n_found++;
        pos += 2u + len;
    }
    if (n_found != v->n_subs) return ARTIFACT_E_SUBS;
    return ARTIFACT_OK;
}

const sub_entry_t *artifact_find_row(const artifact_view_t *v, uint32_t gid)
{
    uint32_t lo = 0, hi = v->n_subs;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t g = v->subs[mid].gid;
        if (g == gid) return &v->subs[mid];
        if (g < gid) lo = mid + 1u;
        else hi = mid;
    }
    return NULL;
}

int artifact_subscribes(const artifact_view_t *v, uint16_t shard_id)
{
    uint32_t key = gid_make(shard_id, 0);
    uint32_t lo = 0, hi = v->n_subs;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (v->subs[mid].gid < key) lo = mid + 1u;
        else hi = mid;
    }
    return lo < v->n_subs && gid_shard(v->subs[lo].gid) == shard_id;
}

/* ---- builder ---------------------------------------------------------- */

static int reserve(artifact_builder_t *b, uint32_t bytes)
{
    if (b->cap - b->pos < bytes) return ARTIFACT_E_NOSPACE;
    return 0;
}

static void put32(artifact_builder_t *b, uint32_t off, uint32_t v)
{
    wr32(b->buf + off, v);
}

int artifact_build_begin(artifact_builder_t *b, void *buf, uint32_t cap, uint16_t shard_id,
                         uint16_t shard_version)
{
    memset(b, 0, sizeof(*b));
    if (buf == NULL || (((uintptr_t)buf) & 3u) != 0) return ARTIFACT_E_ALIGN;
    if (cap < ARTIFACT_HDR_LEN) return ARTIFACT_E_NOSPACE;
    b->buf = (uint8_t *)buf;
    b->cap = cap;
    b->pos = ARTIFACT_HDR_LEN;
    b->shard_id = shard_id;
    b->shard_version = shard_version;
    return 0;
}

int artifact_build_population(artifact_builder_t *b, const neuron_params_t *p, uint16_t count)
{
    int first = b->n_neurons;

    if (b->rows_off != 0) return ARTIFACT_E_ORDER;
    if (count == 0 || (uint32_t)b->n_neurons + count > ARTIFACT_MAX_NEURONS) return ARTIFACT_E_ARG;
    if (b->n_populations >= ARTIFACT_MAX_POPS) return ARTIFACT_E_ARG;
    if (p->model != NEURON_MODEL_IZH && p->model != NEURON_MODEL_LIF) return ARTIFACT_E_ARG;
    if (reserve(b, (uint32_t)sizeof(*p))) return ARTIFACT_E_NOSPACE;
    memcpy(b->buf + b->pos, p, sizeof(*p));
    /* The population size is parked in reserved1 until pop_first is written. */
    wr32(b->buf + b->pos + 44, count);
    b->pos += (uint32_t)sizeof(*p);
    b->n_populations++;
    b->n_neurons = (uint16_t)(b->n_neurons + count);
    return first;
}

/* Lays down pop_first after the populations; called when rows start. */
static int close_populations(artifact_builder_t *b)
{
    uint32_t len = align4(((uint32_t)b->n_populations + 1u) * 2u);
    uint32_t i, off = b->pos;
    uint16_t first = 0;

    if (b->n_populations == 0) return ARTIFACT_E_POPS;
    if (reserve(b, len)) return ARTIFACT_E_NOSPACE;
    memset(b->buf + off, 0, len);
    wr16(b->buf + off, 0);
    for (i = 0; i < b->n_populations; i++) {
        uint8_t *blk = b->buf + ARTIFACT_HDR_LEN + i * (uint32_t)sizeof(neuron_params_t);
        first = (uint16_t)(first + rd32(blk + 44));
        wr32(blk + 44, 0);
        wr16(b->buf + off + 2u * (i + 1u), first);
    }
    b->pos += len;
    b->rows_off = b->pos;
    return 0;
}

int artifact_build_row_begin(artifact_builder_t *b, uint32_t gid, uint8_t weight_shift)
{
    if (b->in_row) return ARTIFACT_E_ORDER;
    if (weight_shift > SYN_WEIGHT_SHIFT_MAX) return ARTIFACT_E_ARG;
    if (b->rows_off == 0) {
        int err = close_populations(b);
        if (err) return err;
    } else if (gid <= b->last_gid) {
        return ARTIFACT_E_ORDER;
    }
    if (reserve(b, 8)) return ARTIFACT_E_NOSPACE;
    b->row_hdr_off = b->pos;
    put32(b, b->pos, gid);
    put32(b, b->pos + 4, 0);
    b->pos += 8;
    b->in_row = 1;
    b->seg_hdr_off = 0;
    b->seg_count = 0;
    b->weight_shift = weight_shift;
    b->last_gid = gid;
    return 0;
}

int artifact_build_synapse(artifact_builder_t *b, uint16_t target, int16_t weight, uint8_t delay,
                           uint8_t flags)
{
    if (!b->in_row) return ARTIFACT_E_ORDER;
    if (delay < SYN_DELAY_MIN || delay > SYN_DELAY_MAX || target >= b->n_neurons) return ARTIFACT_E_ARG;
    if (b->seg_hdr_off != 0 && target < b->last_target) return ARTIFACT_E_ORDER;
    if (b->seg_hdr_off == 0 || b->seg_count == SYN_SEG_MAX_COUNT || (uint32_t)target - b->seg_base > 0xFFu) {
        if (reserve(b, 4)) return ARTIFACT_E_NOSPACE;
        b->seg_hdr_off = b->pos;
        b->seg_base = target;
        b->seg_count = 0;
        b->pos += 4;
    }
    if (reserve(b, 4)) return ARTIFACT_E_NOSPACE;
    put32(b, b->pos, syn_pack(weight, delay, flags, (uint8_t)(target - b->seg_base)));
    b->pos += 4;
    b->seg_count++;
    put32(b, b->seg_hdr_off, syn_seg_pack(b->seg_base, b->seg_count, b->weight_shift));
    b->last_target = target;
    return 0;
}

int artifact_build_row_end(artifact_builder_t *b)
{
    uint32_t words;

    if (!b->in_row) return ARTIFACT_E_ORDER;
    if (b->seg_hdr_off == 0) return ARTIFACT_E_ARG;  /* empty row */
    words = (b->pos - b->row_hdr_off - 8u) / 4u;
    if (words > 0xFFFFu) return ARTIFACT_E_ARG;
    put32(b, b->row_hdr_off + 4, words);
    b->in_row = 0;
    b->n_rows++;
    return 0;
}

int32_t artifact_build_finish(artifact_builder_t *b)
{
    uint32_t pos, rows_end, n_row_words, i;
    uint8_t *hdr;

    if (b->in_row) return ARTIFACT_E_ORDER;
    if (b->rows_off == 0) {
        int err = close_populations(b);
        if (err) return err;
    }
    rows_end = b->pos;
    n_row_words = (rows_end - b->rows_off) / 4u;

    /* subscription index straight from the row stream */
    if (reserve(b, b->n_rows * (uint32_t)sizeof(sub_entry_t))) return ARTIFACT_E_NOSPACE;
    pos = b->rows_off;
    for (i = 0; i < b->n_rows; i++) {
        uint32_t gid = rd32(b->buf + pos);
        uint32_t len = rd32(b->buf + pos + 4);
        uint8_t *e = b->buf + b->pos + i * sizeof(sub_entry_t);
        wr32(e, gid);
        wr32(e + 4, (pos - b->rows_off) / 4u + 2u);
        wr16(e + 8, (uint16_t)len);
        wr16(e + 10, 0);
        pos += 8u + len * 4u;
    }
    b->pos += b->n_rows * (uint32_t)sizeof(sub_entry_t);

    hdr = b->buf;
    memcpy(hdr, ARTIFACT_MAGIC, 4);
    wr16(hdr + 4, DBRAIN_ARTIFACT_VERSION);
    wr16(hdr + 6, DBRAIN_MODEL_VERSION);
    wr16(hdr + 8, b->shard_id);
    wr16(hdr + 10, b->shard_version);
    wr16(hdr + 12, b->n_neurons);
    wr16(hdr + 14, b->n_populations);
    wr32(hdr + 16, b->n_rows);
    wr32(hdr + 20, n_row_words);
    wr32(hdr + 24, b->pos);
    wr32(hdr + 28, 0);
    wr32(hdr + 28, image_crc(hdr, b->pos));
    return (int32_t)b->pos;
}
