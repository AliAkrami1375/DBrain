import pytest

from dbrain import artifact, recipe
from dbrain.core import parse_check, recipe_build_c, Shard
from dbrain.neuron import RS, FS


def test_builder_matches_c_recipe():
    members = [(3, 60), (7, 40), (12, 50)]
    for topo in (recipe.RANDOM, recipe.SPATIAL, recipe.SYNFIRE):
        r = recipe.Recipe(seed=5, topology=topo)
        for nid, _ in members:
            py = recipe.build(r, nid, 1, members)
            c = recipe_build_c(r.pack(), nid, 1, members)
            assert py == c, (topo, nid)
            parse_check(py)


def test_parse_roundtrip():
    b = artifact.ArtifactBuilder(7, 3)
    b.add_population(RS, 300)
    b.add_population(FS, 20)
    b.row_begin(artifact.gid(7, 0))
    b.synapse(1, 128, 1)
    b.synapse(255, -64, 2)
    b.synapse(256, 32, 3)
    b.synapse(319, 16, 15)
    b.row_end()
    b.row_begin(artifact.gid(9, 5))
    for i in range(300):
        b.synapse(i, 10, 2)
    b.row_end()
    img = b.finish()
    parse_check(img)
    a = artifact.parse(img)
    assert a.shard_id == 7 and a.n_neurons == 320 and len(a.rows) == 2
    assert a.rows[artifact.gid(7, 0)].synapses == [(1, 128, 1, 0), (255, -64, 2, 0), (256, 32, 3, 0), (319, 16, 15, 0)]
    assert a.subscribes(9) and not a.subscribes(8)
    assert a.n_synapses == 304
    bad = bytearray(img)
    bad[100] ^= 1
    with pytest.raises(artifact.ArtifactError):
        artifact.parse(bytes(bad))


def test_builder_rules():
    b = artifact.ArtifactBuilder(1)
    with pytest.raises(artifact.ArtifactError):
        b.row_begin(1)
    b.add_population(RS, 10)
    b.row_begin(5)
    b.synapse(3, 1, 1)
    with pytest.raises(artifact.ArtifactError):
        b.synapse(2, 1, 1)
    with pytest.raises(artifact.ArtifactError):
        b.synapse(3, 1, 0)
    b.row_end()
    with pytest.raises(artifact.ArtifactError):
        b.row_begin(5)


def test_shard_runs_python_built_image():
    members = [(1, 80), (2, 60)]
    r = recipe.Recipe(seed=9, topology=recipe.SYNFIRE, local_fan_in=0, remote_fan_in=30, w_exc=4 * 128, drive=0)
    shards = {nid: Shard(recipe.build(r, nid, 1, members)) for nid, _ in members}
    assert shards[2].subscribes(1) and shards[1].subscribes(2)
    for t in range(10):
        for i in range(80):
            shards[1].inject(i, 30 << 15, t)
    first = {}
    for t in range(150):
        fired = {nid: sh.step() for nid, sh in shards.items()}
        for nid, ev in fired.items():
            if ev and nid not in first:
                first[nid] = t
            for dst, sh in shards.items():
                if dst != nid and ev:
                    sh.deliver(nid, t, ev)
    assert first[1] < first[2]
    assert shards[2].telemetry()["events_late"] == 0
