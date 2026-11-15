"""Export: full-int8 TFLite + the model package (.dcd) the decidio runtime firmware loads from its `model` partition.

.dcd layout (little endian):
  0  "DCD1"
  4  u32 json_len        8  u32 model_len      12  u32 crc32(json + pad + model)
  16 JSON header (UTF-8), zero-padded to a 16-byte boundary, then the .tflite flatbuffer
"""
from __future__ import annotations

import json
import time

import numpy as np

from . import boardrules
from .config import Project
from .dcd import MAGIC, PARTITION_BYTES, package  # noqa: F401  (re-exported: other modules import them from here)
from .model import deployed, keras, tf
from .train import Batches, load_dataset

# ops the runtime firmware registers (firmware/main/decide.cc)
SUPPORTED_OPS = {"CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED", "MEAN", "AVERAGE_POOL_2D", "MAX_POOL_2D", "RELU",
                 "RELU6", "LOGISTIC", "SOFTMAX", "MUL", "ADD", "SUB", "CONCATENATION", "RESHAPE", "QUANTIZE",
                 "DEQUANTIZE", "PAD", "STRIDED_SLICE", "PACK", "SHAPE", "TRANSPOSE", "SQUEEZE", "TANH", "HARD_SWISH"}


def convert(p: Project, model, d: dict, n_rep: int = 1000) -> bytes:
    tr_idx = np.where(~d["val"])[0]
    rng = np.random.default_rng(0)
    pick = rng.choice(tr_idx, size=min(n_rep, len(tr_idx)), replace=False)
    seq = Batches(p, d, pick, False, [np.ones(q.n) for q in p.questions])
    names = [t.name.split(":")[0] for t in model.inputs]

    def rep():
        for b in range(len(seq)):
            x, _ = seq[b]
            for i in range(len(next(iter(x.values())))):
                yield [x[n][i:i + 1] for n in names]

    c = tf.lite.TFLiteConverter.from_keras_model(deployed(p, model))      # auxiliary questions are not flashed
    c.optimizations = [tf.lite.Optimize.DEFAULT]
    c.representative_dataset = rep
    c.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    c.inference_input_type = tf.int8
    c.inference_output_type = tf.int8
    return strip(c.convert())


def strip(tfl: bytes) -> bytes:
    from tensorflow.lite.tools import flatbuffer_utils as fu
    m = fu.convert_bytearray_to_object(bytearray(tfl))
    fu.strip_strings(m)
    m.metadata, m.signatureDefs = None, None
    return bytes(fu.convert_object_to_bytearray(m))


class Int8Runner:
    """Runs the int8 model like the firmware: inputs found by shape, int8 in, int8 out."""

    def __init__(self, tfl: bytes, p: Project):
        self.it = tf.lite.Interpreter(model_content=tfl,
                                      experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
        self.it.allocate_tensors()
        self.inputs = {}
        for det in self.it.get_input_details():
            shp = tuple(int(v) for v in det["shape"][1:])
            if p.image and shp == (p.image.size, p.image.size, p.image.channels):
                self.inputs["image"] = det
            elif p.audio and shp == (p.audio.n_frames, p.audio.n_mels, 1):
                self.inputs["mel"] = det
        if len(self.inputs) != len(self.it.get_input_details()):
            raise SystemExit("cannot find the model inputs by their shapes. The image and mel inputs must have different shapes")
        self.out = self.it.get_output_details()[0]

    def __call__(self, x: dict) -> np.ndarray:
        n = len(next(iter(x.values())))
        res = []
        for i in range(n):
            for k, det in self.inputs.items():
                sc, zp = det["quantization"]
                self.it.set_tensor(det["index"], np.clip(np.round(x[k][i:i + 1] / sc) + zp, -128, 127).astype(np.int8))
            self.it.invoke()
            sc, zp = self.out["quantization"]
            res.append((self.it.get_tensor(self.out["index"])[0].astype(np.float32) - zp) * sc)
        return np.stack(res)

    def ops(self) -> list[str]:
        return sorted({o["op_name"] for o in self.it._get_ops_details()})

    def arena_estimate(self) -> int:
        """Bytes: largest pair of live activations + scratch margin (the firmware grows the arena if this is short)."""
        sizes = {t["index"]: int(np.prod(t["shape"])) * np.dtype(t["dtype"]).itemsize
                 for t in self.it.get_tensor_details() if len(t["shape"])}
        ops = self.it._get_ops_details()
        act = {d["index"] for d in self.it.get_input_details()} | {i for o in ops for i in o["outputs"]}
        peak = max(sum(sizes.get(i, 0) for i in list(o["inputs"]) + list(o["outputs"]) if i in act) for o in ops)
        return int(peak * 1.5) + 24 * 1024


class _quiet_stderr:
    """TFLM prints 'Failed to resize buffer' while we search for the smallest arena: hide it."""

    def __enter__(self):
        import os
        import sys
        sys.stderr.flush()
        self.fd = os.dup(2)
        self.null = os.open(os.devnull, os.O_WRONLY)
        os.dup2(self.null, 2)

    def __exit__(self, *a):
        import os
        os.dup2(self.fd, 2)
        os.close(self.fd)
        os.close(self.null)


def arena_bytes(tfl: bytes, runner: Int8Runner) -> int:
    try:
        from tflite_micro.python.tflite_micro import runtime
    except Exception:
        return runner.arena_estimate()

    def ok(kb):
        with _quiet_stderr():
            return _ok(kb)

    def _ok(kb):
        try:
            it = runtime.Interpreter.from_bytes(tfl, arena_size=kb * 1024)
            for i in range(len(runner.inputs)):
                det = it.get_input_details(i)
                it.set_input(np.zeros(det["shape"], det["dtype"]), i)
            it.invoke()
            return True
        except Exception:
            return False

    lo, hi = 8, 1024
    if not ok(hi):
        return runner.arena_estimate()
    while lo < hi:
        mid = (lo + hi) // 2
        hi, lo = (mid, lo) if ok(mid) else (hi, mid + 1)
    return lo * 1024 + 8 * 1024


def _modality(p: Project, q) -> str:
    """'i' or 'a' for a question tied to one input, '' when it may use both."""
    m = q.modality or ("image" if p.image and not p.audio else "audio" if p.audio and not p.image else "")
    return m[:1]


def header(p: Project, d: dict, arena: int) -> dict:
    qm = p.qmap()
    h = {
        "format": 1, "name": p.name, "created": time.strftime("%Y-%m-%dT%H:%M:%S"), "board": p.board,
        "image": None, "audio": None, "n_logits": p.n_deploy, "arena_kb": (arena + 1023) // 1024,
        # "m": which sensor answers the question (i camera, a microphone): a board without that sensor switches it off
        "questions": [{"id": q.qid, "type": q.qtype, "options": q.options, **({"m": _modality(p, q)} if _modality(p, q) else {})}
                      for q in p.deployed],
        "actions": [],
    }
    if p.image:
        c = p.image
        h["image"] = {"size": c.size, "colors": c.colors, "motion": c.motion, "ego": int(c.ego), "platform": c.platform,
                      "lag": c.motion_lag if c.motion else 0,
                      "frame_ms": c.frame_ms}
    if p.audio:
        a = p.audio
        h["audio"] = {"sr": a.sample_rate, "clip": a.clip, "win": a.win, "hop": a.hop, "nfft": a.n_fft, "n_mels": a.n_mels,
                      "fmin": a.fmin, "fmax": a.fmax, "mean": float(d["feat_mean"]), "std": float(d["feat_std"]), "eps": 1e-6}
    h["actions"] = boardrules.wire_all(p.actions, h["questions"])
    return h


def run(p: Project, log=print) -> dict:
    d = load_dataset(p)
    model = keras.models.load_model(p.out / "model.keras", compile=False)
    tfl = convert(p, model, d)
    runner = Int8Runner(tfl, p)
    bad = [o for o in runner.ops() if o not in SUPPORTED_OPS]
    if bad:
        raise SystemExit(f"the runtime firmware does not support these operations: {bad}")
    wide = max([len(o["inputs"]) for o in runner.it._get_ops_details() if o["op_name"] == "CONCATENATION"], default=0)
    if wide > 10:      # TFLite-Micro's kMaxInputNum: the board would fail in AllocateTensors
        raise SystemExit(f"a CONCATENATION joins {wide} tensors, but the runtime firmware accepts a maximum of 10")
    arena = arena_bytes(tfl, runner)
    h = header(p, d, arena)
    blob = package(h, tfl)
    (p.out / "model_int8.tflite").write_bytes(tfl)
    (p.out / "model.dcd").write_bytes(blob)
    (p.out / "model_header.json").write_text(json.dumps(h, indent=2))
    log(f"int8 model {len(tfl) / 1024:.1f} KiB, arena ~{h['arena_kb']} KiB, ops: {', '.join(runner.ops())}")
    log(f"wrote {p.out / 'model.dcd'} ({len(blob) / 1024:.1f} KiB)")
    return {"tflite": tfl, "runner": runner, "header": h}


OPS_METHOD = {
    "CONV_2D": "AddConv2D", "DEPTHWISE_CONV_2D": "AddDepthwiseConv2D", "FULLY_CONNECTED": "AddFullyConnected",
    "MEAN": "AddMean", "AVERAGE_POOL_2D": "AddAveragePool2D", "MAX_POOL_2D": "AddMaxPool2D", "RELU": "AddRelu",
    "RELU6": "AddRelu6", "LOGISTIC": "AddLogistic", "SOFTMAX": "AddSoftmax", "MUL": "AddMul", "ADD": "AddAdd",
    "SUB": "AddSub", "CONCATENATION": "AddConcatenation", "RESHAPE": "AddReshape", "QUANTIZE": "AddQuantize",
    "DEQUANTIZE": "AddDequantize", "PAD": "AddPad", "STRIDED_SLICE": "AddStridedSlice", "PACK": "AddPack",
    "SHAPE": "AddShape", "TRANSPOSE": "AddTranspose", "SQUEEZE": "AddSqueeze", "TANH": "AddTanh",
    "HARD_SWISH": "AddHardSwish",
}


def _cf(x: float) -> str:
    s = f"{float(x):.9g}"
    return (s if any(c in s for c in ".en") else s + ".0") + "f"


def arduino(p: Project, out_dir, log=print) -> None:
    """decidio_model.h / .cpp for Arduino sketches: the model compiled into your own firmware, no model partition."""
    from pathlib import Path as _P
    out = _P(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    tfl = (p.out / "model_int8.tflite").read_bytes()
    h = json.loads((p.out / "model_header.json").read_text())
    runner = Int8Runner(tfl, p)
    ops = runner.ops()
    im, au = h.get("image") or {}, h.get("audio") or {}
    simple = [(a["name"], *boardrules.simple(a, h["questions"])) for a in h["actions"]
              if boardrules.simple(a, h["questions"])]      # the sketch runs one-condition rules only
    L = ["// Generated by `decidio export --arduino` from " + p.name + " - do not edit.", "#pragma once", "#include <stdint.h>", "",
         f'#define DECIDIO_NAME "{p.name}"',
         f"#define DECIDIO_HAS_IMAGE {1 if im else 0}", f"#define DECIDIO_IMG_SIZE {im.get('size', 0)}",
         f"#define DECIDIO_IMG_COLORS {im.get('colors', 0)}", f"#define DECIDIO_IMG_MOTION {1 if im.get('motion') else 0}", f"#define DECIDIO_IMG_EGO {1 if im.get('ego') else 0}",
         f"#define DECIDIO_HAS_AUDIO {1 if au else 0}"]
    for k, v in (("SR", "sr"), ("CLIP", "clip"), ("WIN", "win"), ("HOP", "hop"), ("NFFT", "nfft"), ("NMELS", "n_mels")):
        L.append(f"#define DECIDIO_{k} {int(au.get(v, 0))}")
    L += [f"#define DECIDIO_NFRAMES {1 + (au['clip'] - au['win']) // au['hop'] if au else 0}",
          f"#define DECIDIO_FMIN {_cf(au.get('fmin', 0))}", f"#define DECIDIO_FMAX {_cf(au.get('fmax', 0))}",
          f"#define DECIDIO_MEAN {_cf(au.get('mean', 0))}", f"#define DECIDIO_STD {_cf(au.get('std', 1))}",
          f"#define DECIDIO_NUM_Q {len(h['questions'])}", f"#define DECIDIO_NUM_LOGITS {h['n_logits']}",
          f"#define DECIDIO_NUM_RULES {len(simple)}", f"#define DECIDIO_ARENA_KB {h['arena_kb']}",
          f"#define DECIDIO_OP_COUNT {len(ops)}",
          "#define DECIDIO_REGISTER_OPS(r) do { " + " ".join(f"(r).{OPS_METHOD[o]}();" for o in ops) + " } while (0)", "",
          "struct DecidioQ { const char* id; uint8_t type; uint8_t offset, count; const char* const* opt; };   // type 0 noul, 1 choice, 2 score",
          "struct DecidioRule { const char* name; uint8_t q, o; float above; };",
          "extern const DecidioQ DECIDIO_Q[DECIDIO_NUM_Q];",
          "extern const DecidioRule DECIDIO_RULES[DECIDIO_NUM_RULES > 0 ? DECIDIO_NUM_RULES : 1];",
          "extern const uint8_t decidio_model[];", "extern const unsigned int decidio_model_len;", ""]
    (out / "decidio_model.h").write_text("\n".join(L), encoding="utf-8")
    types = {"noul": 0, "choice": 1, "score": 2}
    C = ["// Generated by `decidio export --arduino` - do not edit.", '#include "decidio_model.h"', ""]
    off = 0
    for i, q in enumerate(h["questions"]):
        C.append(f"static const char* const Q{i}_OPT[] = {{{', '.join(json.dumps(o) for o in q['options'])}}};")
    C.append("const DecidioQ DECIDIO_Q[DECIDIO_NUM_Q] = {")
    for i, q in enumerate(h["questions"]):
        C.append(f"  {{{json.dumps(q['id'])}, {types[q['type']]}, {off}, {len(q['options'])}, Q{i}_OPT}},")
        off += len(q["options"])
    C.append("};")
    C.append("const DecidioRule DECIDIO_RULES[DECIDIO_NUM_RULES > 0 ? DECIDIO_NUM_RULES : 1] = {")
    C += [f"  {{{json.dumps(n)}, {q}, {o}, {_cf(above)}}}," for n, q, o, above in simple] or ['  {"", 0, 0, 2.0f},']
    C += ["};", "", "alignas(16) const uint8_t decidio_model[] = {"]
    C += ["  " + ", ".join(f"0x{b:02x}" for b in tfl[i:i + 20]) + "," for i in range(0, len(tfl), 20)]
    C += ["};", f"const unsigned int decidio_model_len = {len(tfl)};", ""]
    (out / "decidio_model.cpp").write_text("\n".join(C), encoding="utf-8")
    log(f"wrote {out / 'decidio_model.h'} and decidio_model.cpp ({len(tfl) / 1024:.1f} KiB model, ops {', '.join(ops)})")
