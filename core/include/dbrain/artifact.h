/*
 * Compiled shard image ("artifact").
 *
 * An artifact is a self-contained, position-independent binary that holds
 * everything a node needs to simulate one shard: population parameters,
 * synaptic rows and the subscription index. The host compiler (dbrainc)
 * writes it; the recipe generator on a node writes the same format in RAM.
 * The loader validates every field before the simulation touches it, so the
 * hot path can run without bounds checks.
 *
 * Layout (little-endian, every section 4-byte aligned, offsets in bytes):
 *
 *   0    4   magic "DBRA"
 *   4    2   artifact format version   (DBRAIN_ARTIFACT_VERSION)
 *   6    2   model version             (DBRAIN_MODEL_VERSION)
 *   8    2   shard id
 *   10   2   shard version
 *   12   2   neuron count n
 *   14   2   population count p
 *   16   4   subscription entry count s
 *   20   4   row stream length in words r
 *   24   4   total image size in bytes
 *   28   4   CRC-32 of the image with this field as zero
 *   32       populations: p × neuron_params_t (48 bytes each)
 *            pop_first: (p + 1) × u16, padded to 4 bytes; neuron i belongs
 *            to population k iff pop_first[k] <= i < pop_first[k+1]
 *            rows: r words; a sequence of
 *               u32 source gid, u32 row length in words, then the segments
 *               described in synapse.h
 *            subs: s × sub_entry_t, ascending by gid; one entry per row
 *
 * The image must be 4-byte aligned in memory. On ESP32-class chips it is
 * normally memory-mapped straight from a flash partition.
 */
#ifndef DBRAIN_ARTIFACT_H
#define DBRAIN_ARTIFACT_H

#include <stdint.h>
#include <stddef.h>
#include "dbrain/neuron.h"

#define ARTIFACT_HDR_LEN       32u
#define ARTIFACT_MAX_NEURONS   65535u
#define ARTIFACT_MAX_POPS      4096u
#define ARTIFACT_MAX_SIZE      (16u * 1024u * 1024u)

enum {
    ARTIFACT_OK = 0,
    ARTIFACT_E_SIZE = -1,
    ARTIFACT_E_MAGIC = -2,
    ARTIFACT_E_VERSION = -3,
    ARTIFACT_E_MODEL = -4,
    ARTIFACT_E_CRC = -5,
    ARTIFACT_E_POPS = -6,
    ARTIFACT_E_SUBS = -7,
    ARTIFACT_E_ROWS = -8,
    ARTIFACT_E_ALIGN = -9,
    ARTIFACT_E_NOSPACE = -10,
    ARTIFACT_E_ORDER = -11,
    ARTIFACT_E_ARG = -12
};

typedef struct {
    uint32_t gid;        /* source neuron */
    uint32_t row_offset; /* first segment word, index into rows */
    uint16_t row_words;  /* segment words in the row */
    uint16_t reserved;
} sub_entry_t;

typedef struct {
    const uint8_t *image;
    uint32_t size;
    uint16_t shard_id;
    uint16_t shard_version;
    uint16_t n_neurons;
    uint16_t n_populations;
    uint32_t n_subs;
    uint32_t n_row_words;
    const neuron_params_t *pops;
    const uint16_t *pop_first;
    const uint32_t *rows;
    const sub_entry_t *subs;
} artifact_view_t;

/* Validates the whole image. Returns ARTIFACT_OK or an ARTIFACT_E_* code. */
int artifact_parse(const void *image, uint32_t size, artifact_view_t *v);

const char *artifact_strerror(int err);

/* Binary search; returns the entry for gid or NULL. */
const sub_entry_t *artifact_find_row(const artifact_view_t *v, uint32_t gid);

/* True when at least one row has a source on the given shard. */
int artifact_subscribes(const artifact_view_t *v, uint16_t shard_id);

/*
 * Builder. Populations first, then rows in ascending gid order with the
 * synapses of each row in non-decreasing target order, then finish().
 */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t pos;            /* bytes used */
    uint16_t shard_id;
    uint16_t shard_version;
    uint16_t n_neurons;
    uint16_t n_populations;
    uint32_t rows_off;       /* byte offset of the row stream, 0 until rows start */
    uint32_t n_rows;
    uint32_t last_gid;
    int      in_row;
    uint32_t row_hdr_off;    /* byte offset of the current row header */
    uint32_t seg_hdr_off;    /* byte offset of the current segment header, 0 if none */
    uint16_t seg_base;
    uint8_t  seg_count;
    uint8_t  weight_shift;
    uint16_t last_target;
} artifact_builder_t;

int artifact_build_begin(artifact_builder_t *b, void *buf, uint32_t cap, uint16_t shard_id,
                         uint16_t shard_version);
/* Returns the first local index of the new population, or an error code. */
int artifact_build_population(artifact_builder_t *b, const neuron_params_t *p, uint16_t count);
int artifact_build_row_begin(artifact_builder_t *b, uint32_t gid, uint8_t weight_shift);
int artifact_build_synapse(artifact_builder_t *b, uint16_t target, int16_t weight, uint8_t delay,
                           uint8_t flags);
int artifact_build_row_end(artifact_builder_t *b);
/* Writes the subscription index, header and CRC. Returns the image size or an error code. */
int32_t artifact_build_finish(artifact_builder_t *b);

#endif /* DBRAIN_ARTIFACT_H */
