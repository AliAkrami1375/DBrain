"""dbrainc: places a Network onto a cluster and writes one artifact per node.

Placement objective (docs/host-tools.md): minimise expected radio traffic,
counted once per (source population, subscribing node) pair and weighted by
the population's firing rate, while keeping every node inside its neuron and
synapse budgets and balancing integration load against capability. A greedy
pass assigns populations largest first; a refinement pass moves single
populations while that lowers traffic.
"""

import json
import pathlib
from dataclasses import dataclass, field
from typing import Dict, List, Sequence, Tuple

from . import ARTIFACT_VERSION, MODEL_VERSION, __version__
from .artifact import ArtifactBuilder, WEIGHT_SHIFT_DEFAULT, XNODE_DELAY_MIN, DELAY_MAX, gid
from .network import Network, Population

CHIP_IDS = {"esp8266": 1, "esp32": 2, "esp32s2": 3, "esp32s3": 4, "esp32c2": 5, "esp32c3": 6,
            "esp32c5": 7, "esp32c6": 8, "host": 15}
LEAF_CHIPS = {"esp8266", "esp32c2"}


@dataclass
class NodeSpec:
    node_id: int
    chip: str = "esp32"
    free_heap: int = 250000
    psram: int = 0
    max_neurons: int = 2000         # CONFIG_DBRAIN_MAX_NEURONS of the board
    image_bytes: int = 1 << 20      # artifact partition size

    @property
    def mhz(self) -> int:
        return {"esp8266": 80, "esp32c2": 120, "esp32c3": 160, "esp32c6": 160}.get(self.chip, 240)

    @property
    def cores(self) -> int:
        return 2 if self.chip in ("esp32", "esp32s3") else 1

    def neuron_budget(self) -> int:
        """Neurons whose state and buckets fit half the internal heap, capped by the arena."""
        return max(16, min(self.max_neurons, (self.free_heap // 2) // 74))

    def compute_capacity(self) -> float:
        """Relative integration throughput: clock scaled by core count."""
        return self.mhz * (1.5 if self.cores > 1 else 1.0)

    @classmethod
    def from_dict(cls, d: dict) -> "NodeSpec":
        return cls(int(d["node_id"]), d.get("chip", "esp32"), int(d.get("free_heap", 250000)),
                   int(d.get("psram", 0)), int(d.get("max_neurons", 2000)), int(d.get("image_bytes", 1 << 20)))


@dataclass
class Placement:
    pop_node: Dict[str, int]
    pop_first: Dict[str, int]       # local index of the population's first neuron on its node
    node_pops: Dict[int, List[str]]
    traffic: float                  # expected frames per second across the air
    moves: int = 0


@dataclass
class Build:
    manifest: dict
    artifacts: Dict[int, bytes]
    placement: Placement
    warnings: List[str] = field(default_factory=list)

    def write(self, outdir: pathlib.Path) -> None:
        outdir = pathlib.Path(outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        for nid, img in self.artifacts.items():
            (outdir / self.manifest["nodes"][str(nid)]["artifact"]).write_bytes(img)
        (outdir / "manifest.json").write_text(json.dumps(self.manifest, indent=2))


class CompileError(ValueError):
    pass


def _pop(net: Network, name: str) -> Population:
    for p in net.populations:
        if p.name == name:
            return p
    raise KeyError(name)


def _rate_traffic(pop: Population) -> float:
    """Frames per second a population costs each subscribing node: at most one bundle per tick."""
    return min(pop.size * pop.rate_hz, 1000.0) / 119.0


def _traffic_total(net: Network, assign: Dict[str, int], pop_sources: Dict[str, Dict[str, int]]) -> float:
    pairs = set()
    for post_name, srcs in pop_sources.items():
        if post_name not in assign:
            continue
        for pre_name in srcs:
            if pre_name in assign and assign[pre_name] != assign[post_name]:
                pairs.add((pre_name, assign[post_name]))
    return sum(_rate_traffic(_pop(net, pre_name)) for pre_name, _ in pairs)


def _place(net: Network, nodes: Sequence[NodeSpec], pop_sources: Dict[str, Dict[str, int]]) -> Placement:
    pops = sorted(net.populations, key=lambda p: -p.size)
    budgets = {n.node_id: n.neuron_budget() for n in nodes}
    used = {n.node_id: 0 for n in nodes}
    cap = {n.node_id: n.compute_capacity() for n in nodes}
    mean_cap = sum(cap.values()) / len(cap)
    pop_node: Dict[str, int] = {}
    node_pops: Dict[int, List[str]] = {n.node_id: [] for n in nodes}
    leaf = {n.node_id for n in nodes if n.chip in LEAF_CHIPS}

    def fits(p: Population, nid: int) -> bool:
        if used[nid] + p.size > budgets[nid]:
            return False
        if nid in leaf and len(nodes) > 1 and p.size > budgets[nid] // 2:
            return False
        return True

    for p in pops:
        best, best_cost = None, None
        for n in nodes:
            nid = n.node_id
            if not fits(p, nid):
                continue
            trial = dict(pop_node)
            trial[p.name] = nid
            traffic = _traffic_total(net, trial, pop_sources)
            balance = (used[nid] + p.size) / cap[nid] * mean_cap / 1000.0
            cost = traffic + balance
            if best_cost is None or cost < best_cost:
                best, best_cost = nid, cost
        if best is None:
            raise CompileError("population %s (%d neurons) fits no node" % (p.name, p.size))
        pop_node[p.name] = best
        node_pops[best].append(p.name)
        used[best] += p.size

    # refinement: move one population at a time while cross-node traffic drops
    moves = 0
    improved = True
    while improved and moves < 100:
        improved = False
        base = _traffic_total(net, pop_node, pop_sources)
        for p in pops:
            cur = pop_node[p.name]
            for n in nodes:
                nid = n.node_id
                if nid == cur or not fits(p, nid):
                    continue
                trial = dict(pop_node)
                trial[p.name] = nid
                t = _traffic_total(net, trial, pop_sources)
                if t < base - 1e-9:
                    pop_node[p.name] = nid
                    node_pops[cur].remove(p.name)
                    node_pops[nid].append(p.name)
                    used[cur] -= p.size
                    used[nid] += p.size
                    base = t
                    moves += 1
                    improved = True
                    break
            if improved:
                break

    # populations keep the order of the network description within a node
    order = {p.name: i for i, p in enumerate(net.populations)}
    pop_first: Dict[str, int] = {}
    for nid in node_pops:
        node_pops[nid].sort(key=lambda name: order[name])
        first = 0
        for name in node_pops[nid]:
            pop_first[name] = first
            first += _pop(net, name).size
    return Placement(pop_node, pop_first, node_pops, _traffic_total(net, pop_node, pop_sources), moves)


def compile_network(net: Network, nodes: Sequence[NodeSpec], cluster_id: int = 1,
                    weight_shift: int = WEIGHT_SHIFT_DEFAULT, shard_version: int = 1,
                    min_period_us: int = 1000) -> Build:
    if not nodes:
        raise CompileError("no nodes")
    if len({n.node_id for n in nodes}) != len(nodes):
        raise CompileError("duplicate node ids")
    if not net.populations:
        raise CompileError("empty network")
    warnings: List[str] = []

    synapses = list(net.synapses())
    pop_sources: Dict[str, Dict[str, int]] = {p.name: {} for p in net.populations}
    for pre, i, post, j, w, d in synapses:
        pop_sources[post.name][pre.name] = pop_sources[post.name].get(pre.name, 0) + 1

    placement = _place(net, nodes, pop_sources)
    scale = 1 << (15 - weight_shift)

    rows: Dict[int, Dict[int, List[Tuple[int, int, int]]]] = {n.node_id: {} for n in nodes}
    adjusted = 0
    clipped = 0
    for pre, i, post, j, w, d in synapses:
        src_node = placement.pop_node[pre.name]
        dst_node = placement.pop_node[post.name]
        src_gid = gid(src_node, placement.pop_first[pre.name] + i)
        target = placement.pop_first[post.name] + j
        if src_node != dst_node and d < XNODE_DELAY_MIN:
            d = XNODE_DELAY_MIN
            adjusted += 1
        if d < 1:
            d = 1
            adjusted += 1
        if d > DELAY_MAX:
            d = DELAY_MAX
            adjusted += 1
        wi = int(round(w * scale))
        if wi > 32767 or wi < -32768:
            wi = max(-32768, min(32767, wi))
            clipped += 1
        rows[dst_node].setdefault(src_gid, []).append((target, wi, d))
    if adjusted:
        warnings.append("%d synaptic delays adjusted to the legal range (cross-node >= %d)" % (adjusted, XNODE_DELAY_MIN))
    if clipped:
        warnings.append("%d weights clipped to int16 at shift %d" % (clipped, weight_shift))

    artifacts: Dict[int, bytes] = {}
    manifest_nodes = {}
    for n in nodes:
        nid = n.node_id
        names = placement.node_pops[nid]
        if not names:
            continue
        b = ArtifactBuilder(nid, shard_version)
        pops_entry = {}
        for name in names:
            p = _pop(net, name)
            first = b.add_population(p.params, p.size)
            assert first == placement.pop_first[name]
            pops_entry[name] = {"first": first, "size": p.size}
        n_syn = 0
        for g in sorted(rows[nid]):
            syns = sorted(rows[nid][g])
            b.row_begin(g, weight_shift)
            for target, wi, d in syns:
                b.synapse(target, wi, d)
            b.row_end()
            n_syn += len(syns)
        img = b.finish()
        if len(img) > n.image_bytes:
            raise CompileError("node %d: image of %d bytes exceeds its %d byte partition" % (nid, len(img), n.image_bytes))
        artifacts[nid] = img
        manifest_nodes[str(nid)] = {
            "artifact": "shard_%d.bin" % nid, "chip": n.chip, "neurons": sum(_pop(net, x).size for x in names),
            "synapses": n_syn, "rows": len(rows[nid]), "image_bytes": len(img), "populations": pops_entry,
        }

    manifest = {
        "dbrain": __version__, "artifact_version": ARTIFACT_VERSION, "model_version": MODEL_VERSION,
        "network": net.name, "cluster_id": cluster_id, "shard_version": shard_version,
        "min_period_us": min_period_us, "weight_shift": weight_shift,
        "nodes": manifest_nodes,
        "populations": {p.name: {"node": placement.pop_node[p.name], "first": placement.pop_first[p.name],
                                 "size": p.size, "rate_hz": p.rate_hz, "stimulus": p.stimulus}
                        for p in net.populations},
        "traffic_frames_per_s": round(placement.traffic, 2),
        "placement_moves": placement.moves,
        "warnings": warnings,
    }
    return Build(manifest, artifacts, placement, warnings)


def load_nodes(path: pathlib.Path) -> List[NodeSpec]:
    data = json.loads(pathlib.Path(path).read_text())
    if isinstance(data, dict):
        data = data["nodes"]
    return [NodeSpec.from_dict(d) for d in data]


def load_network(path: pathlib.Path) -> Network:
    """Executes a network script; it must define `network`."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("dbrain_network_script", str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    net = getattr(mod, "network", None)
    if not isinstance(net, Network):
        raise CompileError("%s must define `network = Network(...)`" % path)
    return net
