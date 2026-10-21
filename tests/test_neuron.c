#include "unit.h"
#include "dbrain/neuron.h"

/* Regular-spiking Izhikevich parameters: a=0.02 b=0.2 c=-65 d=8. */
static void rs_params(neuron_params_t *p, accum_t bias)
{
    neuron_params_izh(p, FIX_RATIO(2, 100), FIX_RATIO(2, 10), FIX_INT(-65), FIX_INT(8), bias);
}

static int run(const neuron_params_t *p, accum_t input, int ticks, int *first_spike)
{
    neuron_state_t s;
    int t, n = 0;

    *first_spike = -1;
    neuron_init(&s, p);
    for (t = 0; t < ticks; t++) {
        if (neuron_step(&s, p, input)) {
            if (*first_spike < 0) *first_spike = t;
            n++;
        }
    }
    return n;
}

int main(void)
{
    neuron_params_t p;
    neuron_state_t a, b;
    int first, n, t;

    CHECK_EQ(sizeof(neuron_params_t), 48);
    CHECK_EQ(sizeof(neuron_state_t), 8);

    /* resting neuron stays silent */
    rs_params(&p, 0);
    n = run(&p, 0, 1000, &first);
    CHECK_EQ(n, 0);

    /* I = 10 drives a regular-spiking cell at a few tens of hertz */
    n = run(&p, FIX_INT(10), 1000, &first);
    CHECK(n >= 15 && n <= 60);
    CHECK(first >= 0 && first < 100);

    /* the same drive through the bias field gives the identical trajectory */
    rs_params(&p, FIX_INT(10));
    CHECK_EQ(run(&p, 0, 1000, &first), n);

    /* bursting cell (chattering: c=-50, d=2) fires more */
    neuron_params_izh(&p, FIX_RATIO(2, 100), FIX_RATIO(2, 10), FIX_INT(-50), FIX_INT(2), 0);
    CHECK(run(&p, FIX_INT(10), 1000, &first) > n);

    /* initial state: v = c, u = b*c */
    rs_params(&p, 0);
    neuron_init(&a, &p);
    CHECK_EQ(a.v, FIX_INT(-65));
    CHECK_EQ(a.u, fix_mul(FIX_RATIO(2, 10), FIX_INT(-65)));

    /* determinism: two instances stepped identically stay bit-identical */
    neuron_init(&b, &p);
    for (t = 0; t < 2000; t++) {
        int fa = neuron_step(&a, &p, FIX_RATIO(t % 17, 2));
        int fb = neuron_step(&b, &p, FIX_RATIO(t % 17, 2));
        if (fa != fb || a.v != b.v || a.u != b.u) break;
    }
    CHECK_EQ(t, 2000);

    /* absurd input neither traps nor wraps: state stays clamped */
    neuron_init(&a, &p);
    for (t = 0; t < 100; t++) neuron_step(&a, &p, FIX_MAX);
    CHECK(a.v >= IZH_V_MIN && a.v <= IZH_V_MAX);
    for (t = 0; t < 100; t++) neuron_step(&a, &p, FIX_MIN);
    CHECK(a.v >= IZH_V_MIN && a.v <= IZH_V_MAX);

    /* LIF: tau_m = 20 ms (alpha = 0.05), R = 1, threshold -50 from rest -65, refractory 2 */
    neuron_params_lif(&p, FIX_INT(-65), FIX_INT(-70), FIX_INT(-50), FIX_RATIO(5, 100),
                      FIX_RATIO(5, 100), 2, 0);
    CHECK_EQ(run(&p, 0, 500, &first), 0);
    n = run(&p, FIX_INT(20), 1000, &first);     /* steady state -45 > threshold */
    CHECK(n > 10);
    CHECK(first > 0);
    neuron_init(&a, &p);
    for (t = 0; t < 1000 && !neuron_step(&a, &p, FIX_INT(20)); t++) {}
    CHECK_EQ(a.v, FIX_INT(-70));
    CHECK_EQ(a.u, 2);
    CHECK(!neuron_step(&a, &p, FIX_INT(20)));
    CHECK_EQ(a.u, 1);
    CHECK(!neuron_step(&a, &p, FIX_INT(20)));
    CHECK_EQ(a.u, 0);
    UNIT_END();
}
