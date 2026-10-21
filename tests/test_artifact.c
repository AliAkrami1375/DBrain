#include "unit.h"
#include "dbrain/artifact.h"
#include "dbrain/synapse.h"
#include "dbrain/frame.h"

static uint8_t img[8192] __attribute__((aligned(4)));

static int32_t build_sample(void)
{
    artifact_builder_t b;
    neuron_params_t p;
    int i;

    CHECK_EQ(artifact_build_begin(&b, img, sizeof img, 7, 3), 0);
    neuron_params_izh(&p, FIX_RATIO(2, 100), FIX_RATIO(2, 10), FIX_INT(-65), FIX_INT(8), 0);
    CHECK_EQ(artifact_build_population(&b, &p, 300), 0);
    neuron_params_lif(&p, FIX_INT(-65), FIX_INT(-70), FIX_INT(-50), FIX_RATIO(5, 100),
                      FIX_RATIO(5, 100), 2, 0);
    CHECK_EQ(artifact_build_population(&b, &p, 20), 300);

    /* local source 0 -> targets spread over two 256-blocks */
    CHECK_EQ(artifact_build_row_begin(&b, gid_make(7, 0), SYN_WEIGHT_SHIFT_DEFAULT), 0);
    CHECK_EQ(artifact_build_synapse(&b, 1, 128, 1, 0), 0);
    CHECK_EQ(artifact_build_synapse(&b, 255, -64, 2, 0), 0);
    CHECK_EQ(artifact_build_synapse(&b, 256, 32, 3, 0), 0);
    CHECK_EQ(artifact_build_synapse(&b, 319, 16, 15, 0), 0);
    CHECK_EQ(artifact_build_row_end(&b), 0);
    /* remote source (shard 9, neuron 5) -> many targets, forces a count split */
    CHECK_EQ(artifact_build_row_begin(&b, gid_make(9, 5), 8), 0);
    for (i = 0; i < 300; i++) CHECK_EQ(artifact_build_synapse(&b, (uint16_t)i, 10, 2, 0), 0);
    CHECK_EQ(artifact_build_row_end(&b), 0);
    return artifact_build_finish(&b);
}

int main(void)
{
    artifact_view_t v;
    artifact_builder_t b;
    neuron_params_t p;
    int32_t size;
    const sub_entry_t *e;
    uint32_t crc_saved;

    size = build_sample();
    CHECK(size > 0);
    CHECK_EQ(size % 4, 0);
    CHECK_EQ(artifact_parse(img, (uint32_t)size, &v), ARTIFACT_OK);
    CHECK_EQ(v.shard_id, 7);
    CHECK_EQ(v.shard_version, 3);
    CHECK_EQ(v.n_neurons, 320);
    CHECK_EQ(v.n_populations, 2);
    CHECK_EQ(v.pop_first[0], 0);
    CHECK_EQ(v.pop_first[1], 300);
    CHECK_EQ(v.pop_first[2], 320);
    CHECK_EQ(v.pops[1].model, NEURON_MODEL_LIF);
    CHECK_EQ(v.pops[1].reserved1, 0);
    CHECK_EQ(v.n_subs, 2);

    e = artifact_find_row(&v, gid_make(7, 0));
    CHECK(e != NULL);
    if (e) {
        /* segments are cut 256 above their first target: base 1 holds targets
         * 1, 255 and 256 (offset 255); 319 opens a second segment: 6 words */
        CHECK_EQ(e->row_words, 6);
        CHECK_EQ(syn_seg_base(v.rows[e->row_offset]), 1);
        CHECK_EQ(syn_seg_count(v.rows[e->row_offset]), 3);
        CHECK_EQ(syn_seg_shift(v.rows[e->row_offset]), 8);
        CHECK_EQ(syn_target(v.rows[e->row_offset + 2]), 254);
        CHECK_EQ(syn_weight(v.rows[e->row_offset + 2]), -64);
        CHECK_EQ(syn_delay(v.rows[e->row_offset + 2]), 2);
        CHECK_EQ(syn_target(v.rows[e->row_offset + 3]), 255);
        CHECK_EQ(syn_delay(v.rows[e->row_offset + 3]), 3);
        CHECK_EQ(syn_seg_base(v.rows[e->row_offset + 4]), 319);
        CHECK_EQ(syn_seg_count(v.rows[e->row_offset + 4]), 1);
        CHECK_EQ(syn_delay(v.rows[e->row_offset + 5]), 15);
    }
    e = artifact_find_row(&v, gid_make(9, 5));
    CHECK(e != NULL);
    if (e) CHECK_EQ(e->row_words, 300 + 2);   /* 255 + 45 synapses, two headers */
    CHECK(artifact_find_row(&v, gid_make(9, 4)) == NULL);
    CHECK(artifact_find_row(&v, gid_make(8, 0)) == NULL);
    CHECK(artifact_subscribes(&v, 7));
    CHECK(artifact_subscribes(&v, 9));
    CHECK(!artifact_subscribes(&v, 8));
    CHECK(!artifact_subscribes(&v, 10));

    /* corruption is caught by the CRC; a bad CRC is caught too */
    crc_saved = rd32(img + 28);
    img[100] ^= 1;
    CHECK_EQ(artifact_parse(img, (uint32_t)size, &v), ARTIFACT_E_CRC);
    img[100] ^= 1;
    wr32(img + 28, crc_saved ^ 1);
    CHECK_EQ(artifact_parse(img, (uint32_t)size, &v), ARTIFACT_E_CRC);
    wr32(img + 28, crc_saved);
    CHECK_EQ(artifact_parse(img, (uint32_t)size, &v), ARTIFACT_OK);
    CHECK_EQ(artifact_parse(img, (uint32_t)size - 4, &v), ARTIFACT_E_SIZE);
    CHECK_EQ(artifact_parse(img + 1, (uint32_t)size, &v), ARTIFACT_E_ALIGN);
    img[0] = 'X';
    CHECK_EQ(artifact_parse(img, (uint32_t)size, &v), ARTIFACT_E_MAGIC);
    img[0] = 'D';

    /* builder ordering rules */
    CHECK_EQ(artifact_build_begin(&b, img, sizeof img, 1, 1), 0);
    CHECK_EQ(artifact_build_row_begin(&b, 5, 8), ARTIFACT_E_POPS);   /* no populations */
    neuron_params_izh(&p, 1, 1, 1, 1, 0);
    CHECK_EQ(artifact_build_population(&b, &p, 10), 0);
    CHECK_EQ(artifact_build_row_begin(&b, 5, 8), 0);
    CHECK_EQ(artifact_build_population(&b, &p, 10), ARTIFACT_E_ORDER); /* rows started */
    CHECK_EQ(artifact_build_synapse(&b, 3, 1, 1, 0), 0);
    CHECK_EQ(artifact_build_synapse(&b, 2, 1, 1, 0), ARTIFACT_E_ORDER); /* targets descend */
    CHECK_EQ(artifact_build_synapse(&b, 3, 1, 0, 0), ARTIFACT_E_ARG);   /* delay 0 */
    CHECK_EQ(artifact_build_synapse(&b, 10, 1, 1, 0), ARTIFACT_E_ARG);  /* target out of range */
    CHECK_EQ(artifact_build_row_end(&b), 0);
    CHECK_EQ(artifact_build_row_begin(&b, 5, 8), ARTIFACT_E_ORDER);     /* gid must ascend */
    CHECK_EQ(artifact_build_row_begin(&b, 6, 8), 0);
    CHECK_EQ(artifact_build_row_end(&b), ARTIFACT_E_ARG);               /* empty row */
    CHECK_EQ(artifact_build_synapse(&b, 0, 1, 1, 0), 0);
    CHECK_EQ(artifact_build_row_end(&b), 0);
    size = artifact_build_finish(&b);
    CHECK(size > 0);
    CHECK_EQ(artifact_parse(img, (uint32_t)size, &v), ARTIFACT_OK);
    CHECK_EQ(v.n_subs, 2);

    /* an image with populations only is valid */
    CHECK_EQ(artifact_build_begin(&b, img, sizeof img, 1, 1), 0);
    CHECK_EQ(artifact_build_population(&b, &p, 10), 0);
    size = artifact_build_finish(&b);
    CHECK_EQ(size, 32 + 48 + 4);
    CHECK_EQ(artifact_parse(img, (uint32_t)size, &v), ARTIFACT_OK);
    CHECK_EQ(v.n_subs, 0);
    CHECK(!artifact_subscribes(&v, 1));

    /* no space */
    CHECK_EQ(artifact_build_begin(&b, img, 40, 1, 1), 0);
    CHECK_EQ(artifact_build_population(&b, &p, 10), ARTIFACT_E_NOSPACE);
    UNIT_END();
}
