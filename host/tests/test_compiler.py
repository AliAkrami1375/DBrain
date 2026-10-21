import pathlib

from dbrain import artifact
from dbrain.compiler import NodeSpec, compile_network, load_network, load_nodes
from dbrain.core import parse_check
from dbrain.reference import ReferenceRun

HERE = pathlib.Path(__file__).resolve().parent
EXAMPLES = HERE.parent / "examples"


def test_compile_example_and_run_reference(tmp_path):
    net = load_network(EXAMPLES / "two_pools.py")
    nodes = load_nodes(EXAMPLES / "nodes.json")
    build = compile_network(net, nodes)
    assert set(build.artifacts) <= {1, 2, 3}
    total_syn = sum(e["synapses"] for e in build.manifest["nodes"].values())
    assert total_syn == sum(1 for _ in net.synapses())
    for nid, img in build.artifacts.items():
        parse_check(img)
        a = artifact.parse(img)
        for g, row in a.rows.items():
            for target, w, d, f in row.synapses:
                if artifact.gid_shard(g) != nid:
                    assert d >= 2
    build.write(tmp_path)
    assert (tmp_path / "manifest.json").exists()

    run = ReferenceRun.from_dir(tmp_path)
    stim_node = build.manifest["populations"]["stim"]["node"]
    first = build.manifest["populations"]["stim"]["first"]

    def stim(t):
        if t < 50:
            return [(stim_node, first + i, 15 << 15) for i in range(20)]
        return []

    total = run.run(400, stim)
    assert total > 50
    ticks_a = [t for t, n, i in run.raster if n == build.manifest["populations"]["pool_a"]["node"]]
    assert ticks_a
    for tm in run.telemetry().values():
        assert tm["events_late"] == 0 and tm["events_early"] == 0

    build2 = compile_network(net, nodes)
    assert build2.artifacts == build.artifacts
    run2 = ReferenceRun(build2.artifacts)
    run2.run(400, stim)
    assert run2.raster == run.raster


def test_budget_and_placement():
    net = load_network(EXAMPLES / "two_pools.py")
    tiny = [NodeSpec(1, "esp8266", 40000, max_neurons=200), NodeSpec(2, "esp32", 250000)]
    build = compile_network(net, tiny)
    assert build.manifest["populations"]["pool_a"]["node"] == 2
    assert build.manifest["populations"]["pool_b"]["node"] == 2
    one = [NodeSpec(9, "esp32s3", 250000, psram=4 << 20)]
    build1 = compile_network(net, one)
    assert list(build1.artifacts) == [9]
    assert build1.manifest["traffic_frames_per_s"] == 0
