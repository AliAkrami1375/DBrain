"""Host-side tools for DBrain.

fix, rng, neuron, artifact and recipe mirror the C core bit for bit and are
checked against it by the test suite. network and compiler define and place
networks; reference runs them through the C core; gateway talks to a board.
"""

__version__ = "0.1.0"
PROTO_VERSION = 1
ARTIFACT_VERSION = 1
MODEL_VERSION = 1
