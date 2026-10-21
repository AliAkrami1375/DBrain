#include <string.h>
#include "dbrain/neuron.h"

void neuron_params_izh(neuron_params_t *p, accum_t a, accum_t b, accum_t c, accum_t d, accum_t i_bias)
{
    memset(p, 0, sizeof(*p));
    p->model = NEURON_MODEL_IZH;
    p->a = a;
    p->b = b;
    p->c = c;
    p->d = d;
    p->i_bias = i_bias;
}

void neuron_params_lif(neuron_params_t *p, accum_t v_rest, accum_t v_reset, accum_t v_thresh,
                       accum_t alpha, accum_t beta, uint16_t refrac_ticks, accum_t i_bias)
{
    memset(p, 0, sizeof(*p));
    p->model = NEURON_MODEL_LIF;
    p->v_rest = v_rest;
    p->v_reset = v_reset;
    p->v_thresh = v_thresh;
    p->alpha = alpha;
    p->beta = beta;
    p->refrac_ticks = refrac_ticks;
    p->i_bias = i_bias;
}

void neuron_init(neuron_state_t *s, const neuron_params_t *p)
{
    if (p->model == NEURON_MODEL_LIF) {
        s->v = p->v_rest;
        s->u = 0;
    } else {
        s->v = p->c;
        s->u = fix_mul(p->b, p->c);
    }
}

/*
 * Izhikevich, dt = 1 ms, integrated as two 0.5 ms Euler half steps for v and
 * one full step for u, the scheme of the original 2003 reference code.
 * v is clamped to [IZH_V_MIN, IZH_V_MAX] so that 0.04 v^2 stays inside the
 * s16.15 range; the clamp only engages far outside the physiological range.
 */
static bool step_izh(neuron_state_t *s, const neuron_params_t *p, accum_t in)
{
    accum_t v = s->v;
    accum_t u = s->u;
    int k;

    for (k = 0; k < 2; k++) {
        accum_t vc = fix_clamp(v, IZH_V_MIN, IZH_V_MAX);
        accum_t sq = fix_mul(fix_mul(IZH_K_SQ, vc), vc);
        accum_t dv = fix_add_sat(sq, fix_mul_sat(IZH_K_LIN, vc));
        dv = fix_add_sat(dv, IZH_K_CONST);
        dv = fix_sub_sat(dv, u);
        dv = fix_add_sat(dv, in);
        v = fix_add_sat(vc, fix_mul_sat(FIX_HALF, dv));
        v = fix_clamp(v, IZH_V_MIN, IZH_V_MAX);
    }
    u = fix_add_sat(u, fix_mul_sat(p->a, fix_sub_sat(fix_mul_sat(p->b, v), u)));

    if (v >= IZH_V_PEAK) {
        s->v = p->c;
        s->u = fix_add_sat(u, p->d);
        return true;
    }
    s->v = v;
    s->u = u;
    return false;
}

static bool step_lif(neuron_state_t *s, const neuron_params_t *p, accum_t in)
{
    accum_t v;

    if (s->u > 0) {
        s->u -= 1;
        s->v = p->v_reset;
        return false;
    }
    v = fix_add_sat(s->v, fix_mul_sat(p->alpha, fix_sub_sat(p->v_rest, s->v)));
    v = fix_add_sat(v, fix_mul_sat(p->beta, in));
    if (v >= p->v_thresh) {
        s->v = p->v_reset;
        s->u = (accum_t)p->refrac_ticks;
        return true;
    }
    s->v = v;
    return false;
}

bool neuron_step(neuron_state_t *s, const neuron_params_t *p, accum_t input)
{
    accum_t in = fix_clamp(fix_add_sat(input, p->i_bias), -NEURON_I_MAX, NEURON_I_MAX);

    if (p->model == NEURON_MODEL_LIF)
        return step_lif(s, p, in);
    return step_izh(s, p, in);
}
