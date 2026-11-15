"""The firmware's camera-shift estimate and motion channel (firmware/main/ego.c + rt_ego_diff, compiled for the host)
must equal decidio.ego bit for bit: photos through simulated drone / car motion, plus flat, noisy and out-of-range pairs."""
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from decidio import ego  # noqa: E402
from decidio.media import luma  # noqa: E402

VOC = Path(os.environ.get("VOC2012_ZIP", "/nonexistent"))   # optional: VOCtrainval_11-May-2012.zip, for real photos


def pairs(n=120):
    rng = np.random.default_rng(1)
    out = []
    if VOC.exists():
        from PIL import Image
        from decidio.media import center_square, pil_to_cam
        z = zipfile.ZipFile(VOC)
        names = [x for x in z.namelist() if x.endswith(".jpg")][:n]
        for x in names:
            im, _ = center_square(Image.open(io.BytesIO(z.read(x))))
            cur = pil_to_cam(im, 3)
            c, p = ego.simulate(cur, cur, rng)
            out.append((luma(c), luma(p)))
    else:                                            # no photos here: smooth random textures
        for _ in range(n):
            base = rng.integers(0, 256, (12, 12)).astype(np.uint8)
            from PIL import Image
            cur = np.asarray(Image.fromarray(base).resize((96, 96), Image.BICUBIC))[..., None]
            c, p = ego.simulate(cur, cur, rng)
            out.append((c[..., 0], p[..., 0]))
    flat = np.full((96, 96), 120, np.uint8)
    out.append((flat, flat))                                                        # no texture: no shift (ties keep 0)
    out.append((rng.integers(0, 256, (96, 96)).astype(np.uint8), rng.integers(0, 256, (96, 96)).astype(np.uint8)))
    a = rng.integers(0, 256, (96, 96)).astype(np.uint8)
    out.append((a, np.roll(a, (20, -25), (0, 1))))                                  # beyond the search range
    return out


def test_ego_parity():
    if not shutil.which("gcc"):
        pytest.skip("needs gcc to compile the firmware's C for this PC")
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "ego"
        subprocess.run(["gcc", "-O2", "-std=c11", "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "firmware/main"),
                        str(ROOT / "tests/host/ego_main.c"), str(ROOT / "firmware/main/ego.c"), "-o", str(exe)],
                       check=True)
        ps = pairs()
        for size in (96, 64):
            inp = struct.pack("<2i", size, len(ps)) + b"".join(c.tobytes() + p.tobytes() for c, p in ps)
            out = subprocess.run([str(exe)], input=inp, capture_output=True, check=True).stdout
            idx = (np.arange(size) * 96) // size
            step = 8 + size * size
            for k, (c, p) in enumerate(ps):
                dx, dy = struct.unpack_from("<2i", out, k * step)
                got = np.frombuffer(out, np.uint8, size * size, k * step + 8).reshape(size, size)
                assert (dx, dy) == ego.estimate_shift(c, p), (k, (dx, dy), ego.estimate_shift(c, p))
                assert np.array_equal(got, ego.motion_channel(c, p, idx)), k
        print(f"ego parity OK: {len(ps)} pairs x 2 sizes, shifts and motion channels identical")


if __name__ == "__main__":
    test_ego_parity()
