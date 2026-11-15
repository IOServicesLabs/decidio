"""Cameras that move (a drone, a car, a hand): their own motion between the two frames of the motion channel.

estimate_shift() / motion_channel() are what the board runs (firmware/main/ego.c, the same integer arithmetic): the
global shift between the earlier frame and now is found by block matching (2x-downsampled search +-12 px, then +-1 px
at full resolution) and taken out before the difference, so pans, tilts, drift and shake leave the motion channel
empty and only things that move on their own (or near things, by parallax) light it up.

simulate() makes training (and test) pairs look like a moving camera: a shift, a zoom (forward travel), a roll, the
rolling-shutter lean of a fast pan, motion blur and shake.
"""
from __future__ import annotations

import math

import numpy as np
from PIL import Image

from .config import CAM

R = 6               # coarse search radius in half-resolution pixels (+-12 px at 96)
M1 = 2 * R + 1      # full-resolution margin (the fine search reaches 2R + 1)
PAD = 32            # reflected border so simulated motion never samples outside the photo


def _half(lum: np.ndarray) -> np.ndarray:
    v = lum.astype(np.int32)
    return (v[0::2, 0::2] + v[0::2, 1::2] + v[1::2, 0::2] + v[1::2, 1::2]) >> 2


def _pick(sad: np.ndarray, centre: tuple[int, int]) -> tuple[int, int]:
    """Loop semantics of ego.c: start at the centre, row-major (dy outer, dx inner), replace only on strictly less."""
    if sad[centre] <= sad.min():
        return centre
    k = int(np.argmin(sad))
    return divmod(k, sad.shape[1])


def estimate_shift(cur: np.ndarray, prev: np.ndarray) -> tuple[int, int]:
    """uint8 luma [96, 96] x2 -> (dx, dy) with cur(x, y) ~ prev(x + dx, y + dy), |dx|, |dy| <= 2R + 1."""
    from numpy.lib.stride_tricks import sliding_window_view as win
    c, p = _half(cur), _half(prev)
    n = c.shape[0]
    core = c[R:n - R, R:n - R]
    sad = np.abs(win(p, core.shape) - core).sum(axis=(2, 3))            # [2R+1, 2R+1], index (dy + R, dx + R)
    by, bx = _pick(sad, (R, R))
    by, bx = by - R, bx - R
    cf, pf = cur.astype(np.int32), prev.astype(np.int32)
    core = cf[M1:CAM - M1, M1:CAM - M1]
    sad = np.empty((3, 3), np.int64)
    for j, dy in enumerate((-1, 0, 1)):
        for i, dx in enumerate((-1, 0, 1)):
            y0, x0 = M1 + 2 * by + dy, M1 + 2 * bx + dx
            sad[j, i] = np.abs(core - pf[y0:y0 + core.shape[0], x0:x0 + core.shape[1]]).sum()
    fy, fx = _pick(sad, (1, 1))
    return 2 * bx + fx - 1, 2 * by + fy - 1


def motion_channel(cur_l: np.ndarray, prev_l: np.ndarray, idx: np.ndarray) -> np.ndarray:
    """|now - earlier frame moved back by the camera's shift|, sampled at the model's grid (idx, like decide.cc)."""
    dx, dy = estimate_shift(cur_l, prev_l)
    ys, xs = np.clip(idx + dy, 0, CAM - 1), np.clip(idx + dx, 0, CAM - 1)
    a = cur_l[idx][:, idx].astype(np.int16)
    b = prev_l[ys][:, xs].astype(np.int16)
    return np.abs(a - b).astype(np.uint8)


# ---------------------------------------------------------------- simulation

def draw(rng, still_share: float = 0.15) -> dict:
    """Camera motion between the earlier frame and now (motion_ms apart), in 96-px frame pixels."""
    r = rng.random()
    amp = 0.0 if r < still_share else (2.5 if r < 0.55 else (6.0 if r < 0.85 else 12.0))
    if amp == 0:
        return {"tx": 0.0, "ty": 0.0, "scale": 1.0, "rot": 0.0, "lean": 0.0, "blur": rng.uniform(0, 0.8), "shake": 0.0}
    ang = rng.uniform(0, 2 * math.pi)
    mag = amp * math.sqrt(rng.uniform(0.1, 1.0))
    return {
        "tx": mag * math.cos(ang), "ty": mag * math.sin(ang) * 0.7,    # pans are wider than tilts
        "scale": math.exp(rng.uniform(-0.02, 0.06) * amp / 6),          # mostly forward (the view grows)
        "rot": math.radians(rng.uniform(-3, 3) * amp / 12),             # roll (a drone banking)
        "lean": rng.uniform(0, 0.35),                                   # rolling shutter: share of the motion over a readout
        "blur": rng.uniform(0.05, 0.3) * mag + rng.uniform(0, 1.0),     # exposure share of the motion + shake
        "shake": rng.normal(0, 0.6),
    }


def _warp(src: np.ndarray, m: dict, prev: bool, offset: tuple[float, float]) -> np.ndarray:
    """One frame through the camera: affine output -> input, from a reflect-padded copy of src."""
    h = CAM / 2
    s, a = (m["scale"], m["rot"]) if prev else (1.0, 0.0)
    tx, ty = ((m["tx"] + m["shake"], m["ty"]) if prev else (0.0, 0.0))
    lean = m["lean"] * m["tx"] / CAM                                   # x offset per row (both frames lean alike)
    ca, sa = math.cos(a) * s, math.sin(a) * s
    ox, oy = offset
    # input = R*S*(u - h) + h + t + lean*(v - h)
    coef = (ca, -sa + lean, h - ca * h - (-sa + lean) * h + tx + ox + PAD,
            sa, ca, h - sa * h - ca * h + ty + oy + PAD)
    im = Image.fromarray(src)
    return np.asarray(im.transform((CAM, CAM), Image.AFFINE, coef, Image.BILINEAR), np.float32)


def simulate(cur: np.ndarray, prev: np.ndarray, rng, m: dict | None = None) -> tuple[np.ndarray, np.ndarray]:
    """uint8 [96, 96, C] pair (prev may equal cur, or carry objects' own motion) -> the pair a moving camera sees."""
    m = m or draw(rng)
    C = cur.shape[-1]
    out = []
    for f, is_prev in ((cur, False), (prev, True)):
        src = np.pad(f, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="reflect")
        src = src[..., 0] if C == 1 else src
        L = m["blur"]
        n = max(1, int(round(L)))
        d = math.hypot(m["tx"], m["ty"]) or 1.0
        ux, uy = (m["tx"] / d, m["ty"] / d) if (m["tx"] or m["ty"]) else (math.cos(rng.uniform(0, 6.3)), 0.0)
        acc = np.zeros((CAM, CAM), np.float32) if C == 1 else np.zeros((CAM, CAM, C), np.float32)
        for k in range(n):
            t = (k + 0.5) / n - 0.5 if n > 1 else 0.0
            acc += _warp(src, m, is_prev, (ux * L * t, uy * L * t))
        a = acc / n
        out.append(np.clip(a[..., None] if C == 1 else a, 0, 255).astype(np.uint8))
    return out[0], out[1]
