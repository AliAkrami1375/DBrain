"""ctypes binding to the C core (libdbrain) for bit-exact reference runs.

The library is found through DBRAIN_LIB, next to this package, or in the
repository's build directory (build/core/libdbrain.so).
"""

import ctypes
import os
import pathlib
from typing import List, Optional, Sequence, Tuple

_lib: Optional[ctypes.CDLL] = None

TELEMETRY_FIELDS = [
    "bundles_tx", "bundles_rx", "bundles_ignored", "bundles_bad", "bundles_stale", "events_tx",
    "events_rx", "events_applied", "events_late", "events_early", "events_norow", "seq_gaps",
    "tick_overruns", "tick_resyncs", "sync_residual_us", "sync_samples", "heartbeats_rx",
    "control_retries", "control_failures", "elections", "heap_low_water", "frames_dropped_rx",
]


class Telemetry(ctypes.Structure):
    _fields_ = [(f, ctypes.c_int32 if f == "sync_residual_us" else ctypes.c_uint32) for f in TELEMETRY_FIELDS]

    def as_dict(self):
        return {f: getattr(self, f) for f in TELEMETRY_FIELDS}


class NeuronState(ctypes.Structure):
    _fields_ = [("v", ctypes.c_int32), ("u", ctypes.c_int32)]


class NeuronParams(ctypes.Structure):
    _fields_ = [("model", ctypes.c_uint8), ("reserved0", ctypes.c_uint8), ("refrac_ticks", ctypes.c_uint16),
                ("a", ctypes.c_int32), ("b", ctypes.c_int32), ("c", ctypes.c_int32), ("d", ctypes.c_int32),
                ("i_bias", ctypes.c_int32), ("v_rest", ctypes.c_int32), ("v_reset", ctypes.c_int32),
                ("v_thresh", ctypes.c_int32), ("alpha", ctypes.c_int32), ("beta", ctypes.c_int32),
                ("reserved1", ctypes.c_uint32)]


class RecipeMember(ctypes.Structure):
    _fields_ = [("node_id", ctypes.c_uint16), ("n_neurons", ctypes.c_uint16)]


class RecipeStruct(ctypes.Structure):
    _fields_ = [("seed", ctypes.c_uint32), ("topology", ctypes.c_uint8), ("exc_percent", ctypes.c_uint8),
                ("w_exc", ctypes.c_int16), ("w_inh", ctypes.c_int16), ("local_fan_in", ctypes.c_uint16),
                ("remote_fan_in", ctypes.c_uint16), ("drive", ctypes.c_uint16)]


def _candidates():
    env = os.environ.get("DBRAIN_LIB")
    if env:
        yield pathlib.Path(env)
    here = pathlib.Path(__file__).resolve().parent
    yield here / "libdbrain.so"
    repo = here.parent.parent
    yield repo / "build" / "core" / "libdbrain.so"
    yield repo / "build" / "core" / "libdbrain.dylib"


def load() -> ctypes.CDLL:
    global _lib
    if _lib is not None:
        return _lib
    for c in _candidates():
        if c.exists():
            _lib = ctypes.CDLL(str(c))
            break
    if _lib is None:
        raise RuntimeError("libdbrain not found; build the host tree (cmake) or set DBRAIN_LIB")
    L = _lib
    L.fix_selftest.restype = ctypes.c_bool
    L.neuron_init.argtypes = [ctypes.POINTER(NeuronState), ctypes.POINTER(NeuronParams)]
    L.neuron_step.restype = ctypes.c_bool
    L.neuron_step.argtypes = [ctypes.POINTER(NeuronState), ctypes.POINTER(NeuronParams), ctypes.c_int32]
    L.artifact_parse.restype = ctypes.c_int
    L.artifact_parse.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    L.artifact_strerror.restype = ctypes.c_char_p
    L.artifact_strerror.argtypes = [ctypes.c_int]
    L.shard_sizeof.restype = ctypes.c_size_t
    L.shard_arena_size.restype = ctypes.c_size_t
    L.shard_arena_size.argtypes = [ctypes.c_uint16]
    L.shard_bind.restype = ctypes.c_int
    L.shard_bind.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t,
                             ctypes.POINTER(Telemetry)]
    L.shard_reset.argtypes = [ctypes.c_void_p]
    L.shard_tick.restype = ctypes.c_uint16
    L.shard_tick.argtypes = [ctypes.c_void_p]
    L.shard_deliver.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint16),
                                ctypes.c_uint16]
    L.shard_inject.restype = ctypes.c_int
    L.shard_inject.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_int32, ctypes.c_uint32]
    L.shard_subscribes.restype = ctypes.c_int
    L.shard_subscribes.argtypes = [ctypes.c_void_p, ctypes.c_uint16]
    L.shard_tick_get.restype = ctypes.c_uint32
    L.shard_tick_get.argtypes = [ctypes.c_void_p]
    L.shard_fired_get.restype = ctypes.POINTER(ctypes.c_uint16)
    L.shard_fired_get.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint16)]
    L.shard_state_get.restype = ctypes.POINTER(NeuronState)
    L.shard_state_get.argtypes = [ctypes.c_void_p]
    L.shard_neurons.restype = ctypes.c_uint16
    L.shard_neurons.argtypes = [ctypes.c_void_p]
    L.shard_id.restype = ctypes.c_uint16
    L.shard_id.argtypes = [ctypes.c_void_p]
    L.recipe_build.restype = ctypes.c_int32
    L.recipe_build.argtypes = [ctypes.POINTER(RecipeStruct), ctypes.c_uint16, ctypes.c_uint16,
                               ctypes.POINTER(RecipeMember), ctypes.c_uint16, ctypes.c_void_p, ctypes.c_uint32]
    L.recipe_drive.argtypes = [ctypes.POINTER(RecipeStruct), ctypes.c_void_p]
    L.fix_mul_fn.restype = ctypes.c_int32
    L.fix_mul_fn.argtypes = [ctypes.c_int32, ctypes.c_int32]
    return L


def _artifact_view_size() -> int:
    # artifact_view_t: pointer, u32, 4×u16, 2×u32, 4 pointers; sized generously
    return 16 * ctypes.sizeof(ctypes.c_void_p) + 64


def parse_check(image: bytes) -> None:
    """Raises ValueError when the C loader rejects the image."""
    L = load()
    buf = ctypes.create_string_buffer(image, len(image))
    view = ctypes.create_string_buffer(_artifact_view_size())
    err = L.artifact_parse(buf, len(image), view)
    if err != 0:
        raise ValueError("artifact rejected by the C loader: %s" % L.artifact_strerror(err).decode())


def neuron_run(params_bytes: bytes, inputs: Sequence[int]) -> List[Tuple[int, int, bool]]:
    """Steps one neuron through the C core; returns (v, u, fired) per tick."""
    L = load()
    p = NeuronParams.from_buffer_copy(params_bytes)
    s = NeuronState()
    L.neuron_init(ctypes.byref(s), ctypes.byref(p))
    out = []
    for i in inputs:
        f = L.neuron_step(ctypes.byref(s), ctypes.byref(p), i)
        out.append((s.v, s.u, bool(f)))
    return out


class Shard:
    """One shard running in the C core."""

    def __init__(self, image: bytes):
        L = load()
        self._L = L
        self.image = ctypes.create_string_buffer(image, len(image))
        self.tm = Telemetry()
        self._obj = ctypes.create_string_buffer(L.shard_sizeof())
        n = int.from_bytes(image[12:14], "little")
        self._arena = ctypes.create_string_buffer(L.shard_arena_size(n))
        err = L.shard_bind(self._obj, self.image, len(image), self._arena, len(self._arena), ctypes.byref(self.tm))
        if err != 0:
            raise ValueError("shard_bind: %s" % L.artifact_strerror(err).decode())
        self.n_neurons = L.shard_neurons(self._obj)
        self.shard_id = L.shard_id(self._obj)

    @property
    def tick(self) -> int:
        return self._L.shard_tick_get(self._obj)

    def set_tick(self, tick: int) -> None:
        """Positions the counter, as a node does when it first synchronises."""
        self.reset()
        # the C API has no setter; emulate by advancing silently on an empty shard is wrong,
        # so bindings that need a non-zero start run the shard from tick 0 and offset their records.
        if tick != 0:
            raise ValueError("set_tick supports 0 only; offset your records instead")

    def reset(self) -> None:
        self._L.shard_reset(self._obj)

    def step(self) -> List[int]:
        self._L.shard_tick(self._obj)
        n = ctypes.c_uint16()
        p = self._L.shard_fired_get(self._obj, ctypes.byref(n))
        return [p[i] for i in range(n.value)]

    def deliver(self, src_shard: int, src_tick: int, events: Sequence[int]) -> None:
        if not events:
            return
        arr = (ctypes.c_uint16 * len(events))(*events)
        self._L.shard_deliver(self._obj, src_shard, src_tick, arr, len(events))

    def inject(self, neuron: int, current: int, at_tick: int) -> int:
        return self._L.shard_inject(self._obj, neuron, current, at_tick)

    def subscribes(self, shard: int) -> bool:
        return bool(self._L.shard_subscribes(self._obj, shard))

    def state(self, i: int) -> Tuple[int, int]:
        s = self._L.shard_state_get(self._obj)[i]
        return s.v, s.u

    def telemetry(self) -> dict:
        return self.tm.as_dict()


def recipe_build_c(recipe_bytes: bytes, self_id: int, version: int, members: Sequence[Tuple[int, int]],
                   cap: int = 1 << 22) -> bytes:
    L = load()
    r = RecipeStruct.from_buffer_copy(recipe_bytes)
    arr = (RecipeMember * len(members))(*[RecipeMember(a, b) for a, b in members])
    buf = ctypes.create_string_buffer(cap)
    size = L.recipe_build(ctypes.byref(r), self_id, version, arr, len(members), buf, cap)
    if size < 0:
        raise ValueError("recipe_build: %s" % L.artifact_strerror(size).decode())
    return buf.raw[:size]


def recipe_drive_c(recipe_bytes: bytes, shard: Shard) -> None:
    L = load()
    r = RecipeStruct.from_buffer_copy(recipe_bytes)
    L.recipe_drive(ctypes.byref(r), shard._obj)
