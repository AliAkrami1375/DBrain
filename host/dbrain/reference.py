"""Host reference simulator: every shard of a build, run through the C core.

Deliveries are perfect and on time, so the result is what the cluster
computes when nothing is lost. Spikes are recorded as (tick, node, index).
"""

import csv
import json
import pathlib
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Tuple

from .core import Shard, recipe_drive_c

Stimulus = Callable[[int], Iterable[Tuple[int, int, int]]]   # tick -> (node_id, neuron, current)


class ReferenceRun:
    def __init__(self, artifacts: Dict[int, bytes]):
        self.shards: Dict[int, Shard] = {nid: Shard(img) for nid, img in artifacts.items()}
        self.subs: Dict[int, List[int]] = {}
        for nid in self.shards:
            self.subs[nid] = [d for d, other in self.shards.items() if d != nid and other.subscribes(nid)]
        self.raster: List[Tuple[int, int, int]] = []

    @classmethod
    def from_dir(cls, outdir: pathlib.Path) -> "ReferenceRun":
        outdir = pathlib.Path(outdir)
        manifest = json.loads((outdir / "manifest.json").read_text())
        arts = {int(nid): (outdir / entry["artifact"]).read_bytes() for nid, entry in manifest["nodes"].items()}
        return cls(arts)

    def run(self, ticks: int, stimulus: Optional[Stimulus] = None, record: bool = True,
            recipe: Optional[bytes] = None) -> int:
        total = 0
        for _ in range(ticks):
            fired: Dict[int, List[int]] = {}
            if stimulus is not None:
                t = next(iter(self.shards.values())).tick
                for nid, neuron, current in stimulus(t):
                    self.shards[nid].inject(neuron, current, t)
            for nid, sh in self.shards.items():
                if recipe is not None:
                    recipe_drive_c(recipe, sh)
                t = sh.tick
                ev = sh.step()
                fired[nid] = ev
                total += len(ev)
                if record:
                    self.raster.extend((t, nid, i) for i in ev)
            for nid, ev in fired.items():
                if not ev:
                    continue
                src_tick = self.shards[nid].tick - 1
                for dst in self.subs[nid]:
                    self.shards[dst].deliver(nid, src_tick, ev)
        return total

    def telemetry(self) -> Dict[int, dict]:
        return {nid: sh.telemetry() for nid, sh in self.shards.items()}

    def write_raster(self, path: pathlib.Path) -> None:
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["tick", "node", "neuron"])
            w.writerows(self.raster)


def read_gateway_raster(path: pathlib.Path) -> List[Tuple[int, int, int]]:
    """Spike events from a gateway log (JSON lines with ev == spikes and idx)."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("ev") == "spikes" and "idx" in rec:
                out.extend((int(rec["tick"]), int(rec["node"]), int(i)) for i in rec["idx"])
    return out


def compare_rasters(reference: Sequence[Tuple[int, int, int]], observed: Sequence[Tuple[int, int, int]],
                    tick_offset: int = 0) -> dict:
    ref = set(reference)
    obs = {(t - tick_offset, n, i) for t, n, i in observed}
    return {"reference": len(ref), "observed": len(obs), "matched": len(ref & obs),
            "missing": len(ref - obs), "extra": len(obs - ref)}
