"""Two excitatory pools driving each other, with a stimulus population.

    dbrain compile host/examples/two_pools.py -n host/examples/nodes.json -o build-net
"""

from dbrain.network import Network, FixedFanIn, AllToAll
from dbrain.neuron import RS, FS, Params

network = Network("two_pools")
stim = network.population("stim", 20, Params.izh(0.02, 0.2, -65, 8), rate_hz=20.0, stimulus=True)
a = network.population("pool_a", 200, RS, rate_hz=5.0)
b = network.population("pool_b", 200, RS, rate_hz=5.0)
inh = network.population("inh", 50, FS, rate_hz=10.0)

network.connect(stim, a, AllToAll(), weight=2.0, delay=1, seed=1)
network.connect(a, b, FixedFanIn(20), weight=1.5, delay=(2, 5), seed=2)
network.connect(b, a, FixedFanIn(20), weight=1.5, delay=(2, 5), seed=3)
network.connect(a, inh, FixedFanIn(10), weight=2.0, delay=(1, 3), seed=4)
network.connect(inh, a, FixedFanIn(10), weight=-4.0, delay=(1, 3), seed=5)
network.connect(inh, b, FixedFanIn(10), weight=-4.0, delay=(1, 3), seed=6)
