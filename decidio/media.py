"""Audio and image primitives shared by import, prep, training and the label UI.

The firmware mirrors log_mel() (decidio_rt/mel.c), cam_to_model() and luma() (decide.cc) exactly.
"""
from __future__ import annotations

import subprocess
import wave
from pathlib import Path

import numpy as np
from PIL import Image

from .config import CAM, AudioCfg, ImageCfg

MEL_EPS = 1e-6


# ---------------------------------------------------------------- audio frontend

def hann(n: int) -> np.ndarray:
    i = np.arange(n, dtype=np.float64)
    return (0.5 - 0.5 * np.cos(2.0 * np.pi * i / n)).astype(np.float32)


def mel_filterbank(a: AudioCfg) -> np.ndarray:
    """Triangular HTK-mel filters [n_mels, n_fft/2+1] (firmware computes the same at boot)."""
    def hz2mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    def mel2hz(m):
        return 700.0 * (10.0 ** (m / 2595.0) - 1.0)

    n_bins = a.n_fft // 2 + 1
    freqs = np.arange(n_bins) * a.sample_rate / a.n_fft
    pts = mel2hz(np.linspace(hz2mel(a.fmin), hz2mel(a.fmax), a.n_mels + 2))
    fb = np.zeros((a.n_mels, n_bins), np.float32)
    for m in range(a.n_mels):
        lo, c, hi = pts[m], pts[m + 1], pts[m + 2]
        fb[m] = np.maximum(0.0, np.minimum((freqs - lo) / (c - lo), (hi - freqs) / (hi - c)))
    if (fb.sum(1) == 0).any():
        raise ValueError("a mel filter is empty. Decrease n_mels, or increase n_fft or fmin")
    return fb


def log_mel(pcm: np.ndarray, a: AudioCfg, fb: np.ndarray) -> np.ndarray:
    """int16 [..., clip] -> log-mel [..., n_frames, n_mels]; frames end-aligned like the streaming firmware."""
    x = pcm.astype(np.float32) / 32768.0
    starts = (a.clip - a.win) - a.hop * np.arange(a.n_frames)[::-1]
    idx = np.arange(a.win)[None, :] + starts[:, None]
    spec = np.fft.rfft(x[..., idx] * hann(a.win), n=a.n_fft)
    power = (spec.real ** 2 + spec.imag ** 2).astype(np.float32)
    return np.log(power @ fb.T + MEL_EPS).astype(np.float32)


def fit_len(pcm: np.ndarray, n: int) -> np.ndarray:
    if len(pcm) >= n:
        s = (len(pcm) - n) // 2
        return pcm[s:s + n]
    return np.pad(pcm, (0, n - len(pcm)))


def read_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: the file must be 16-bit PCM")
        pcm = np.frombuffer(w.readframes(w.getnframes()), np.int16)
        if w.getnchannels() > 1:
            pcm = pcm.reshape(-1, w.getnchannels()).mean(1).astype(np.int16)
        return pcm, w.getframerate()


def write_wav(path: Path, pcm: np.ndarray, sr: int) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(np.asarray(pcm, np.int16).tobytes())


def ffmpeg_exe() -> str:
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return "ffmpeg"


def decode_audio(path: Path, sr: int) -> np.ndarray:
    """Any audio/video file -> int16 mono at sr (16-bit wav at the right rate is read directly)."""
    p = Path(path)
    if p.suffix.lower() == ".wav":
        try:
            pcm, rate = read_wav(p)
            if rate == sr:
                return pcm
        except (ValueError, wave.Error):
            pass
    cmd = [ffmpeg_exe(), "-v", "error", "-i", str(p), "-vn", "-ac", "1", "-ar", str(sr), "-f", "s16le", "-"]
    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(out, np.int16).copy()


def loud_windows(pcm: np.ndarray, n: int, k: int, min_rel_db: float = -25.0) -> list[np.ndarray]:
    """Up to k non-overlapping windows of n samples, loudest first, within min_rel_db of the loudest."""
    if len(pcm) <= n:
        return [fit_len(pcm, n)]
    hop = max(1, n // 4)
    starts = np.arange(0, len(pcm) - n + 1, hop)
    x = pcm.astype(np.float32)
    energy = np.array([np.mean(x[s:s + n] ** 2) for s in starts]) + 1e-9
    order = np.argsort(-energy)
    top = energy[order[0]]
    picked = []
    for i in order:
        if len(picked) >= k or 10 * np.log10(energy[i] / top) < min_rel_db:
            break
        s = starts[i]
        if all(abs(s - t) >= n for t in picked):
            picked.append(s)
    return [pcm[s:s + n] for s in picked]


# ---------------------------------------------------------------- images

def luma(img: np.ndarray) -> np.ndarray:
    """uint8 [..., 3] -> uint8 luma (integer BT.601, same as firmware)."""
    if img.shape[-1] == 1:
        return img[..., 0]
    r, g, b = (img[..., i].astype(np.uint16) for i in range(3))
    return ((r * 77 + g * 150 + b * 29) >> 8).astype(np.uint8)


def pil_to_cam(im: Image.Image, colors: int) -> np.ndarray:
    """PIL image (already the square region to keep) -> uint8 [96, 96, colors]; gray = luma() like the board."""
    if im.mode in ("L", "I", "I;16", "F", "1") and colors == 1:
        return np.asarray(im.convert("L").resize((CAM, CAM), Image.BILINEAR), np.uint8)[..., None]
    a = np.asarray(im.convert("RGB").resize((CAM, CAM), Image.BILINEAR), np.uint8)
    return a if colors == 3 else luma(a)[..., None]


def center_square(im: Image.Image) -> tuple[Image.Image, tuple[int, int, int]]:
    w, h = im.size
    s = min(w, h)
    x0, y0 = (w - s) // 2, (h - s) // 2
    return im.crop((x0, y0, x0 + s, y0 + s)), (x0, y0, s)


def load_cam(path: Path, colors: int) -> np.ndarray:
    im = Image.open(path)
    if im.size != (CAM, CAM):
        im, _ = center_square(im)
    return pil_to_cam(im, colors)


def save_cam(path: Path, img: np.ndarray) -> None:
    Image.fromarray(img[..., 0] if img.shape[-1] == 1 else img).save(path)


def cam_to_model(cur: np.ndarray, prev: np.ndarray | None, c: ImageCfg) -> np.ndarray:
    """uint8 [96,96,colors] (+ earlier frame) -> uint8 [size, size, channels], nearest downscale like decide.cc."""
    idx = (np.arange(c.size) * CAM) // c.size
    x = cur[idx][:, idx]
    if not c.motion:
        return x
    if c.ego:
        from .ego import motion_channel
        m = motion_channel(luma(cur), luma(prev if prev is not None else cur), idx)
        return np.concatenate([x, m[..., None]], -1)
    p = (prev if prev is not None else cur)[idx][:, idx]
    m = np.abs(luma(x).astype(np.int16) - luma(p).astype(np.int16)).astype(np.uint8)
    return np.concatenate([x, m[..., None]], -1)
