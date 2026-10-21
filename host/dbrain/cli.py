"""The `dbrain` command."""

import argparse
import json
import pathlib
import sys


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="dbrain", description="DBrain host tools")
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("compile", help="place a network and write shard artifacts")
    c.add_argument("network", help="Python file defining `network`")
    c.add_argument("-n", "--nodes", required=True, help="JSON list of nodes")
    c.add_argument("-o", "--out", default="build-net", help="output directory")
    c.add_argument("--cluster", type=int, default=1)
    c.add_argument("--weight-shift", type=int, default=8)

    r = sub.add_parser("reference", help="run a build on the host and dump the raster")
    r.add_argument("build", help="directory written by compile")
    r.add_argument("--ticks", type=int, default=1000)
    r.add_argument("--raster", default="raster.csv")
    r.add_argument("--stimulus", help='JSON: {"node": id, "neurons": [..], "current": fix, "ticks": [a,b]}')

    cmp_ = sub.add_parser("compare", help="compare a reference raster with a gateway log")
    cmp_.add_argument("raster")
    cmp_.add_argument("log")
    cmp_.add_argument("--tick-offset", type=int, default=0)

    g = sub.add_parser("gateway", help="stream a board's events, optionally over WebSocket")
    g.add_argument("--port", required=True)
    g.add_argument("--baud", type=int, default=115200)
    g.add_argument("--log")
    g.add_argument("--ws", type=int)
    g.add_argument("--quiet", action="store_true")

    s = sub.add_parser("send", help="send one console command to a board")
    s.add_argument("--port", required=True)
    s.add_argument("--baud", type=int, default=115200)
    s.add_argument("command")

    f = sub.add_parser("flash-artifact", help="write a shard image to a board's artifact partition")
    f.add_argument("--port", required=True)
    f.add_argument("--chip", required=True)
    f.add_argument("image")
    f.add_argument("--baud", type=int, default=460800)
    f.add_argument("--dry-run", action="store_true")

    i = sub.add_parser("inspect", help="describe a shard image")
    i.add_argument("image")

    args = ap.parse_args(argv)

    if args.cmd == "compile":
        from .compiler import compile_network, load_network, load_nodes
        net = load_network(pathlib.Path(args.network))
        nodes = load_nodes(pathlib.Path(args.nodes))
        build = compile_network(net, nodes, cluster_id=args.cluster, weight_shift=args.weight_shift)
        build.write(pathlib.Path(args.out))
        for w in build.warnings:
            print("warning:", w, file=sys.stderr)
        for nid, entry in build.manifest["nodes"].items():
            print("node %s: %d neurons, %d synapses, %d bytes -> %s" % (
                nid, entry["neurons"], entry["synapses"], entry["image_bytes"], entry["artifact"]))
        print("expected cross-node traffic: %.1f frames/s" % build.manifest["traffic_frames_per_s"])
        return 0

    if args.cmd == "reference":
        from .reference import ReferenceRun
        run = ReferenceRun.from_dir(pathlib.Path(args.build))
        stim = None
        if args.stimulus:
            spec = json.loads(args.stimulus)
            a, b = spec.get("ticks", [0, 10])

            def stim(t, spec=spec, a=a, b=b):
                if a <= t < b:
                    return [(spec["node"], n, spec["current"]) for n in spec["neurons"]]
                return []
        total = run.run(args.ticks, stim)
        run.write_raster(pathlib.Path(args.raster))
        print("%d spikes in %d ticks; raster in %s" % (total, args.ticks, args.raster))
        for nid, tm in run.telemetry().items():
            print("node %d: applied %d late %d early %d" % (
                nid, tm["events_applied"], tm["events_late"], tm["events_early"]))
        return 0

    if args.cmd == "compare":
        import csv
        from .reference import compare_rasters, read_gateway_raster
        with open(args.raster) as fh:
            ref = [(int(r["tick"]), int(r["node"]), int(r["neuron"])) for r in csv.DictReader(fh)]
        obs = read_gateway_raster(pathlib.Path(args.log))
        print(json.dumps(compare_rasters(ref, obs, args.tick_offset)))
        return 0

    if args.cmd == "gateway":
        from .gateway import run as run_gateway
        run_gateway(args.port, args.baud, args.log, args.ws, args.quiet)
        return 0

    if args.cmd == "send":
        from .gateway import send_command
        send_command(args.port, args.baud, args.command)
        return 0

    if args.cmd == "flash-artifact":
        from .flash import flash_artifact
        return flash_artifact(args.port, args.chip, pathlib.Path(args.image), args.baud, dry_run=args.dry_run)

    if args.cmd == "inspect":
        from .artifact import parse
        a = parse(pathlib.Path(args.image).read_bytes())
        print("shard %d version %d: %d neurons in %d populations, %d rows, %d synapses, %d bytes" % (
            a.shard_id, a.shard_version, a.n_neurons, len(a.pops), len(a.rows), a.n_synapses, a.size))
        print("subscribed shards:", list(a.sources()))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
