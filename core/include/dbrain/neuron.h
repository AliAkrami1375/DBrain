/*
 * Point-neuron models, integrated on a fixed 1 ms tick in s16.15 arithmetic.
 *
 * Two models are available per population:
 *
 *   NEURON_MODEL_IZH  Izhikevich (2003). Two state variables (v, u), four
 *                     parameters (a, b, c, d), reproduces the full catalogue
 *                     of cortical firing patterns at 13 operations per tick.
 *   NEURON_MODEL_LIF  Leaky integrate-and-fire with an absolute refractory
 *                     period. Cheaper; a near-subset of the above.
 *
 * The integration scheme is fixed by this header and documented in
 * docs/simulation-core.md. Any change to it changes the bit pattern every
 * node and the host reference produce, so it is versioned through
 * DBRAIN_MODEL_VERSION in version.h.
 */
#ifndef DBRAIN_NEURON_H
#define DBRAIN_NEURON_H

#include <stdint.h>
#include <stdbool.h>
#include "dbrain/fix.h"

enum {
    NEURON_MODEL_IZH = 0,
    NEURON_MODEL_LIF = 1
};

/* 8 bytes per neuron. For LIF, u holds the remaining refractory ticks. */
typedef struct {
    accum_t v;
    accum_t u;
} neuron_state_t;

/*
 * Shared per-population parameter block. 48 bytes, all fields little-endian
 * in the artifact image; the struct below has no padding on every target.
 */
typedef struct {
    uint8_t  model;        /* NEURON_MODEL_* */
    uint8_t  reserved0;
    uint16_t refrac_ticks; /* LIF: absolute refractory period in ticks */
    accum_t  a;            /* IZH: recovery time scale        */
    accum_t  b;            /* IZH: recovery sensitivity       */
    accum_t  c;            /* IZH: after-spike reset of v     */
    accum_t  d;            /* IZH: after-spike increment of u */
    accum_t  i_bias;       /* constant input added every tick (both models) */
    accum_t  v_rest;       /* LIF: resting potential          */
    accum_t  v_reset;      /* LIF: reset potential            */
    accum_t  v_thresh;     /* LIF: firing threshold           */
    accum_t  alpha;        /* LIF: dt / tau_m                 */
    accum_t  beta;         /* LIF: alpha * R_m                */
    uint32_t reserved1;
} neuron_params_t;

/* Izhikevich model constants (s16.15). Exposed for the host reference. */
#define IZH_K_SQ     FIX_RATIO(4, 100)   /* 0.04 -> 1311 (0.040008...) */
#define IZH_K_LIN    FIX_INT(5)
#define IZH_K_CONST  FIX_INT(140)
#define IZH_V_PEAK   FIX_INT(30)
#define IZH_V_MIN    FIX_INT(-200)       /* clamp before squaring */
#define IZH_V_MAX    FIX_INT(1000)       /* clamp after each half step */
#define NEURON_I_MAX FIX_INT(10000)      /* input clamp, both models */

void neuron_init(neuron_state_t *s, const neuron_params_t *p);

/* Advance one tick with the given synaptic input. Returns true on a spike. */
bool neuron_step(neuron_state_t *s, const neuron_params_t *p, accum_t input);

/* Convenience constructors used by tests and the recipe generator. */
void neuron_params_izh(neuron_params_t *p, accum_t a, accum_t b, accum_t c, accum_t d, accum_t i_bias);
void neuron_params_lif(neuron_params_t *p, accum_t v_rest, accum_t v_reset, accum_t v_thresh,
                       accum_t alpha, accum_t beta, uint16_t refrac_ticks, accum_t i_bias);

#endif /* DBRAIN_NEURON_H */
