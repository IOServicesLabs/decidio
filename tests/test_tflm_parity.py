"""The exported int8 model must give the same answers in TensorFlow Lite Micro (the interpreter the board runs)
as in TensorFlow Lite, on real validation inputs.   python tests/test_tflm_parity.py <project folder> [n]"""
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))


def run(project: str, n: int = 200):
    from tflite_micro.python.tflite_micro import runtime
    from decidio.config import load
    from decidio.export import Int8Runner
    from decidio.train import Batches, load_dataset

    p = load(project)
    tfl = (p.out / "model_int8.tflite").read_bytes()
    lite = Int8Runner(tfl, p)
    micro = runtime.Interpreter.from_bytes(tfl, arena_size=1024 * 1024)
    order = {}
    for i in range(len(lite.inputs)):
        shp = tuple(int(v) for v in micro.get_input_details(i)["shape"][1:])
        order[i] = "image" if p.image and shp == (p.image.size, p.image.size, p.image.channels) else "mel"
    d = load_dataset(p)
    idx = np.where(d["val"])[0][:n]
    seq = Batches(p, d, idx, False, [np.ones(q.n) for q in p.questions])
    diffs, agree, total = [], 0, 0
    for b in range(len(seq)):
        x, _ = seq[b]
        a = lite(x)
        for k in range(len(a)):
            for i, name in order.items():
                det = lite.inputs[name]
                sc, zp = det["quantization"]
                micro.set_input(np.clip(np.round(x[name][k:k + 1] / sc) + zp, -128, 127).astype(np.int8), i)
            micro.invoke()
            sc, zp = lite.out["quantization"]
            m = (micro.get_output(0)[0].astype(np.float32) - zp) * sc
            diffs.append(float(np.abs(m - a[k]).max() / sc))
            for q in p.deployed:
                sl = slice(q.offset, q.offset + q.n)
                agree += int(m[sl].argmax() == a[k][sl].argmax())
                total += 1
    print(f"{p.name}: {len(diffs)} samples, max |TFLM - TFLite| = {max(diffs):.0f} int8 steps, "
          f"same answer {agree}/{total} ({agree / total:.1%})")
    assert agree / total > 0.99


if __name__ == "__main__":
    run(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 200)
