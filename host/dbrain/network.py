"""Network description: populations and projections, PyNN-flavoured.

A Network is pure description. Connectivity is materialised deterministically
from the projection seeds by synapses(), so the same script always compiles
to the same artifacts.
"""

import random
from dataclasses import dataclass, field
from typing import Callable, Iterator, List, Optional, Sequence, Tuple, Union

from .neuron import Params

Weight = Union[float, Callable[[random.Random], float]]
Delay = Union[int, Tuple[int, int], Callable[[random.Random], int]]


@dataclass
class Population:
    name: str
    size: int
    params: Params
    rate_hz: float = 3.0            # expected mean firing rate, used by the placer
    stimulus: bool = False           # input population driven from the host

    def __post_init__(self):
        if self.size <= 0 or self.size > 65535:
            raise ValueError("population size must be 1..65535")


class Connector:
    def pairs(self, pre: Population, post: Population, rng: random.Random) -> Iterator[Tuple[int, int]]:
        raise NotImplementedError


class AllToAll(Connector):
    def __init__(self, allow_self: bool = False):
        self.allow_self = allow_self

    def pairs(self, pre, post, rng):
        for i in range(pre.size):
            for j in range(post.size):
                if pre is post and i == j and not self.allow_self:
                    continue
                yield i, j


class OneToOne(Connector):
    def pairs(self, pre, post, rng):
        for i in range(min(pre.size, post.size)):
            yield i, i


class FixedProbability(Connector):
    def __init__(self, p: float, allow_self: bool = False):
        self.p = p
        self.allow_self = allow_self

    def pairs(self, pre, post, rng):
        for i in range(pre.size):
            for j in range(post.size):
                if pre is post and i == j and not self.allow_self:
                    continue
                if rng.random() < self.p:
                    yield i, j


class FixedFanIn(Connector):
    """Every target receives exactly n sources drawn without replacement."""

    def __init__(self, n: int, allow_self: bool = False):
        self.n = n
        self.allow_self = allow_self

    def pairs(self, pre, post, rng):
        for j in range(post.size):
            pool = [i for i in range(pre.size) if self.allow_self or pre is not post or i != j]
            for i in sorted(rng.sample(pool, min(self.n, len(pool)))):
                yield i, j


class FromList(Connector):
    """Explicit (pre, post) pairs, optionally with (pre, post, weight, delay)."""

    def __init__(self, items: Sequence[Tuple]):
        self.items = list(items)

    def pairs(self, pre, post, rng):
        for it in self.items:
            yield it[0], it[1]


@dataclass
class Projection:
    pre: Population
    post: Population
    connector: Connector
    weight: Weight = 1.0
    delay: Delay = 1
    seed: int = 0
    name: str = ""

    def materialise(self) -> List[Tuple[int, int, float, int]]:
        rng = random.Random((self.seed * 1000003 + hash((self.pre.name, self.post.name))) & 0xFFFFFFFF)
        out = []
        explicit = isinstance(self.connector, FromList)
        for k, (i, j) in enumerate(self.connector.pairs(self.pre, self.post, rng)):
            if explicit and len(self.connector.items[k]) >= 4:
                w, d = self.connector.items[k][2], self.connector.items[k][3]
            else:
                w = self.weight(rng) if callable(self.weight) else self.weight
                if callable(self.delay):
                    d = self.delay(rng)
                elif isinstance(self.delay, tuple):
                    d = rng.randint(self.delay[0], self.delay[1])
                else:
                    d = self.delay
            out.append((i, j, float(w), int(d)))
        return out


@dataclass
class Network:
    name: str = "network"
    populations: List[Population] = field(default_factory=list)
    projections: List[Projection] = field(default_factory=list)

    def population(self, name: str, size: int, params: Params, rate_hz: float = 3.0,
                   stimulus: bool = False) -> Population:
        if any(p.name == name for p in self.populations):
            raise ValueError("duplicate population %s" % name)
        p = Population(name, size, params, rate_hz, stimulus)
        self.populations.append(p)
        return p

    def connect(self, pre: Population, post: Population, connector: Connector, weight: Weight = 1.0,
                delay: Delay = 1, seed: int = 0, name: str = "") -> Projection:
        pr = Projection(pre, post, connector, weight, delay, seed, name or "%s->%s" % (pre.name, post.name))
        self.projections.append(pr)
        return pr

    def synapses(self) -> Iterator[Tuple[Population, int, Population, int, float, int]]:
        for pr in self.projections:
            for i, j, w, d in pr.materialise():
                yield pr.pre, i, pr.post, j, w, d

    @property
    def n_neurons(self) -> int:
        return sum(p.size for p in self.populations)
