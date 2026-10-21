"""Point-neuron parameter blocks and the exact integration scheme of neuron.c."""

import struct
from dataclasses import dataclass

from . import fix

MODEL_IZH = 0
MODEL_LIF = 1

K_SQ = fix.ratio(4, 100)
K_LIN = fix.from_int(5)
K_CONST = fix.from_int(140)
V_PEAK = fix.from_int(30)
V_MIN = fix.from_int(-200)
V_MAX = fix.from_int(1000)
I_MAX = fix.from_int(10000)

PARAMS_FORMAT = "<BBHiiiiiiiiiiI"   # 48 bytes
PARAMS_LEN = struct.calcsize(PARAMS_FORMAT)
assert PARAMS_LEN == 48


@dataclass
class Params:
    model: int = MODEL_IZH
    refrac_ticks: int = 0
    a: int = 0
    b: int = 0
    c: int = 0
    d: int = 0
    i_bias: int = 0
    v_rest: int = 0
    v_reset: int = 0
    v_thresh: int = 0
    alpha: int = 0
    beta: int = 0

    @classmethod
    def izh(cls, a: float, b: float, c: float, d: float, i_bias: float = 0.0) -> "Params":
        return cls(MODEL_IZH, 0, fix.from_float(a), fix.from_float(b), fix.from_float(c),
                   fix.from_float(d), fix.from_float(i_bias))

    @classmethod
    def lif(cls, v_rest: float, v_reset: float, v_thresh: float, tau_m_ms: float, r_m: float,
            refrac_ticks: int = 0, i_bias: float = 0.0) -> "Params":
        alpha = 1.0 / tau_m_ms
        return cls(MODEL_LIF, refrac_ticks, 0, 0, 0, 0, fix.from_float(i_bias), fix.from_float(v_rest),
                   fix.from_float(v_reset), fix.from_float(v_thresh), fix.from_float(alpha),
                   fix.from_float(alpha * r_m))

    def pack(self) -> bytes:
        return struct.pack(PARAMS_FORMAT, self.model, 0, self.refrac_ticks, self.a, self.b, self.c, self.d,
                           self.i_bias, self.v_rest, self.v_reset, self.v_thresh, self.alpha, self.beta, 0)

    @classmethod
    def unpack(cls, data: bytes) -> "Params":
        f = struct.unpack(PARAMS_FORMAT, data[:PARAMS_LEN])
        return cls(f[0], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12])


# Standard Izhikevich cell types (a, b, c, d).
RS = Params.izh(0.02, 0.2, -65, 8)
IB = Params.izh(0.02, 0.2, -55, 4)
CH = Params.izh(0.02, 0.2, -50, 2)
FS = Params.izh(0.1, 0.2, -65, 2)
LTS = Params.izh(0.02, 0.25, -65, 2)


def init_state(p: Params):
    if p.model == MODEL_LIF:
        return [p.v_rest, 0]
    return [p.c, fix.mul(p.b, p.c)]


def step(state, p: Params, inp: int) -> bool:
    """Advances one tick in place; returns True on a spike. Mirrors neuron_step()."""
    inp = fix.clamp(fix.add_sat(inp, p.i_bias), -I_MAX, I_MAX)
    if p.model == MODEL_LIF:
        if state[1] > 0:
            state[1] -= 1
            state[0] = p.v_reset
            return False
        v = fix.add_sat(state[0], fix.mul_sat(p.alpha, fix.sub_sat(p.v_rest, state[0])))
        v = fix.add_sat(v, fix.mul_sat(p.beta, inp))
        if v >= p.v_thresh:
            state[0] = p.v_reset
            state[1] = p.refrac_ticks
            return True
        state[0] = v
        return False
    v, u = state
    for _ in range(2):
        vc = fix.clamp(v, V_MIN, V_MAX)
        sq = fix.mul(fix.mul(K_SQ, vc), vc)
        dv = fix.add_sat(sq, fix.mul_sat(K_LIN, vc))
        dv = fix.add_sat(dv, K_CONST)
        dv = fix.sub_sat(dv, u)
        dv = fix.add_sat(dv, inp)
        v = fix.add_sat(vc, fix.mul_sat(fix.HALF, dv))
        v = fix.clamp(v, V_MIN, V_MAX)
    u = fix.add_sat(u, fix.mul_sat(p.a, fix.sub_sat(fix.mul_sat(p.b, v), u)))
    if v >= V_PEAK:
        state[0] = p.c
        state[1] = fix.add_sat(u, p.d)
        return True
    state[0] = v
    state[1] = u
    return False
