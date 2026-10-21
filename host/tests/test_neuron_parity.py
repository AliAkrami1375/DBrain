import random

from dbrain import fix, neuron
from dbrain.core import neuron_run


def run_py(p, inputs):
    s = neuron.init_state(p)
    out = []
    for i in inputs:
        f = neuron.step(s, p, i)
        out.append((s[0], s[1], f))
    return out


def test_izh_bit_exact():
    rng = random.Random(3)
    inputs = [rng.randint(-fix.from_int(20), fix.from_int(30)) for _ in range(5000)]
    for p in (neuron.RS, neuron.CH, neuron.FS, neuron.Params.izh(0.02, 0.2, -65, 8, 10.0)):
        assert run_py(p, inputs) == neuron_run(p.pack(), inputs)


def test_lif_bit_exact():
    rng = random.Random(4)
    inputs = [rng.randint(0, fix.from_int(40)) for _ in range(3000)]
    p = neuron.Params.lif(-65, -70, -50, 20.0, 1.0, refrac_ticks=2)
    assert run_py(p, inputs) == neuron_run(p.pack(), inputs)


def test_extreme_inputs_stay_clamped():
    p = neuron.RS
    inputs = [fix.MAX] * 50 + [fix.MIN] * 50
    out = run_py(p, inputs)
    assert out == neuron_run(p.pack(), inputs)
    for v, u, f in out:
        assert neuron.V_MIN <= v <= neuron.V_MAX
