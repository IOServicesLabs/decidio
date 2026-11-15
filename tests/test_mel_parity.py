"""The firmware's log-mel (firmware/main/mel.c, compiled for the host) must match decidio.media.log_mel."""
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from decidio.config import AudioCfg  # noqa: E402
from decidio.media import log_mel, mel_filterbank  # noqa: E402


def test_mel_parity():
    if not shutil.which("gcc"):
        pytest.skip("needs gcc to compile the firmware's C for this PC")
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "mel"
        subprocess.run(["gcc", "-O2", "-std=c11", "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "firmware/main"),
                        str(ROOT / "tests/host/mel_main.c"), str(ROOT / "firmware/main/mel.c"), "-lm", "-o", str(exe)],
                       check=True)
        worst = 0.0
        for cfg in (AudioCfg(), AudioCfg(n_mels=32, n_fft=1024, win=640, hop=320, clip_seconds=0.75),
                    AudioCfg(sample_rate=8000, fmax=3800, n_mels=24, win=256, hop=128, n_fft=256)):
            fb = mel_filterbank(cfg)
            rng = np.random.default_rng(0)
            t = np.arange(cfg.clip) / cfg.sample_rate
            pcm = (3000 * np.sin(2 * np.pi * 440 * t) + rng.normal(0, 800, cfg.clip)).astype(np.int16)
            mean, std = -6.0, 3.5
            ref = (log_mel(pcm, cfg, fb) - mean) / std
            inp = struct.pack("<6i4f", cfg.sample_rate, cfg.clip, cfg.win, cfg.hop, cfg.n_fft, cfg.n_mels,
                              cfg.fmin, cfg.fmax, mean, std) + pcm.tobytes()
            out = subprocess.run([str(exe)], input=inp, capture_output=True, check=True).stdout
            got = np.frombuffer(out, np.float32).reshape(ref.shape)
            worst = max(worst, float(np.abs(got - ref).max()))
        print(f"max |firmware - python| = {worst:.2e} (normalized log-mel units)")
        assert worst < 1e-3


if __name__ == "__main__":
    test_mel_parity()
