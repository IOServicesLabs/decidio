"""The student: thin depthwise-separable CNN per enabled input -> per-question routed joint head (one logit per option)."""
from __future__ import annotations

import os

os.environ.setdefault("TF_USE_LEGACY_KERAS", "1")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import urllib.request  # noqa: E402
from pathlib import Path  # noqa: E402

import numpy as np  # noqa: E402
import tensorflow as tf  # noqa: E402

from .config import ImageCfg, Project  # noqa: E402

keras = tf.keras
L = keras.layers


def _ch(c: int, width: float) -> int:
    return max(8, int(round(c * width / 8)) * 8)


BN_MOMENTUM = 0.9      # set per run by train.run: 0.99 when epochs are long (stable statistics), 0.9 for small data


def _bn(name):
    return L.BatchNormalization(momentum=BN_MOMENTUM, name=name)


def _ds(x, ch, stride, name):
    x = L.DepthwiseConv2D(3, strides=stride, padding="same", use_bias=False, name=f"{name}_dw")(x)
    x = L.ReLU(6.0, name=f"{name}_dw_act")(_bn(f"{name}_dw_bn")(x))
    x = L.Conv2D(ch, 1, use_bias=False, name=f"{name}_pw")(x)
    return L.ReLU(6.0, name=f"{name}_pw_act")(_bn(f"{name}_pw_bn")(x))


# ImageNet-pretrained MobileNetV1 (Keras' own weights, also published on GitHub): transfer learning for image questions
BACKBONES = {
    "mobilenet_v1_0.25": (0.25, "mobilenet_2_5_128_tf_no_top.h5"),
    "mobilenet_v1_0.5": (0.5, "mobilenet_5_0_128_tf_no_top.h5"),
}
WEIGHTS_URL = "https://github.com/fchollet/deep-learning-models/releases/download/v0.6/"


def _weights(name: str) -> Path:
    d = Path.home() / ".decidio" / "weights"
    d.mkdir(parents=True, exist_ok=True)
    f = d / BACKBONES[name][1]
    if not f.exists():
        print(f"downloading {WEIGHTS_URL + f.name}")
        tmp = f.with_suffix(".part")
        urllib.request.urlretrieve(WEIGHTS_URL + f.name, tmp)
        tmp.replace(f)
    return f


def _pretrained_branch(img, c: ImageCfg, backbone: str):
    """MobileNetV1 with its first convolution rebuilt for decidio's input: gray (or RGB) in [0,1] plus an optional
    motion channel. The pretrained RGB filters are folded in exactly (x' = 2x - 1; gray = R = G = B), motion starts at 0."""
    alpha, _ = BACKBONES[backbone]
    base = keras.applications.MobileNet(input_shape=(c.size, c.size, 3), alpha=alpha, include_top=False, weights=None)
    base.load_weights(str(_weights(backbone)))
    w = base.get_layer("conv1").get_weights()[0].astype(np.float64)          # (3, 3, 3, F)
    f = w.shape[-1]
    k = np.zeros((3, 3, c.channels, f))
    if c.colors == 3:
        k[:, :, :3] = 2 * w
    else:
        k[:, :, 0] = 2 * w.sum(axis=2)
    stem = L.Conv2D(f, 3, strides=2, padding="same", use_bias=True, name="v_stem")
    x = stem(img)
    stem.set_weights([k.astype(np.float32), (-w.sum(axis=(0, 1, 2))).astype(np.float32)])
    after = False
    for layer in base.layers:                                                # MobileNetV1 is a plain chain
        if layer.name == "conv1":
            after = True
            continue
        if not after:
            continue
        cfg = layer.get_config()
        cfg["name"] = "mb_" + cfg["name"]
        if isinstance(layer, L.BatchNormalization):
            cfg["momentum"] = BN_MOMENTUM
        fresh = layer.__class__.from_config(cfg)
        x = fresh(x)
        if layer.get_weights():
            fresh.set_weights(layer.get_weights())
    return x


def build(p: Project) -> keras.Model:
    w, emb = p.model["width"], int(p.model["emb"])
    inputs, branches = [], {}

    if p.image:
        c = p.image
        img = keras.Input((c.size, c.size, c.channels), name="image")
        if p.model.get("backbone", "tiny") in BACKBONES:
            x = _pretrained_branch(img, c, p.model["backbone"])
        else:
            x = L.Conv2D(_ch(8, w), 3, strides=2, padding="same", use_bias=False, name="v_stem")(img)
            x = L.ReLU(6.0, name="v_stem_act")(_bn("v_stem_bn")(x))
            for i, (ch, st) in enumerate([(16, 1), (32, 2), (32, 1), (64, 2), (64, 2)]):
                x = _ds(x, _ch(ch, w), st, f"v{i}")
        v = L.Dropout(0.2, name="v_drop")(L.GlobalAveragePooling2D(name="v_gap")(x))
        branches["image"] = L.Dense(emb, activation="relu", name="v_emb")(v)
        inputs.append(img)

    if p.audio:
        a = p.audio
        mel = keras.Input((a.n_frames, a.n_mels, 1), name="mel")
        y = L.Conv2D(_ch(16, w), (5, 3), strides=2, padding="same", use_bias=False, name="a_stem")(mel)
        y = L.ReLU(6.0, name="a_stem_act")(_bn("a_stem_bn")(y))
        for i, (ch, st) in enumerate([(32, 1), (32, 2), (64, 1), (64, 2)]):
            y = _ds(y, _ch(ch, w), st, f"a{i}")
        av = L.Dropout(0.2, name="a_drop")(L.GlobalAveragePooling2D(name="a_gap")(y))
        branches["audio"] = L.Dense(emb, activation="relu", name="a_emb")(av)
        inputs.append(mel)

    return keras.Model(inputs, _heads(p, branches), name="decidio")


MAX_JOIN = 8      # TFLite-Micro's CONCATENATION takes at most 10 inputs: more than that and the board cannot run it


def _join(outs: list, name: str):
    """All the heads' logits as one vector, in order, never joining more than MAX_JOIN tensors at once."""
    if len(outs) == 1:
        return outs[0]
    if len(outs) > MAX_JOIN:
        outs = [L.Concatenate(name=f"{name}_{i // MAX_JOIN}")(outs[i:i + MAX_JOIN]) if len(outs[i:i + MAX_JOIN]) > 1
                else outs[i] for i in range(0, len(outs), MAX_JOIN)]
        return _join(outs, name + "_all") if len(outs) > MAX_JOIN else L.Concatenate(name=name)(outs)
    return L.Concatenate(name=name)(outs)


def _heads(p: Project, branches: dict):
    """The per-question heads on the branch features ({"image": tensor, "audio": tensor}) -> all logits, concatenated."""
    hidden = int(p.model["hidden"])
    feats = list(branches.values())
    ctx = L.Concatenate(name="ctx")(feats) if len(feats) > 1 else feats[0]
    outs = []
    for q in p.questions:
        if len(feats) > 1:
            # each question routes evidence between modalities with its own gates (modality hint -> fixed route)
            parts = []
            for k, f in branches.items():
                if q.modality and q.modality != k:
                    continue
                g = L.Dense(1, activation="sigmoid", name=f"{q.qid}_gate_{k}")(ctx)
                parts.append(L.Multiply(name=f"{q.qid}_m_{k}")([f, g]))
            h = L.Add(name=f"{q.qid}_mix")(parts) if len(parts) > 1 else parts[0]
        else:
            h = ctx
        h = L.Dense(hidden, activation="relu", name=f"{q.qid}_hidden")(h)
        outs.append(L.Dense(q.n, name=f"{q.qid}_logits")(h))
    return _join(outs, "logits")


FEATURES = {"image": "v_emb", "audio": "a_emb"}     # the layer whose output is each branch's feature vector


def features(model: keras.Model) -> keras.Model:
    """The body of a model alone: inputs -> the feature vector of every branch (what the heads read)."""
    have = {l.name for l in model.layers}
    return keras.Model(model.inputs, [model.get_layer(n).output for n in FEATURES.values() if n in have], name="decidio_body")


def heads(p: Project, dropout: float = 0.0) -> keras.Model:
    """The heads alone, reading feature vectors: same layer names as in build(), so weights move over by name.
    dropout: features dropped at random while training (few examples: keeps a head from leaning on a handful of them)."""
    emb = int(p.model["emb"])
    ins = {k: keras.Input((emb,), name=f"{FEATURES[k]}_in") for k, on in (("image", p.image), ("audio", p.audio)) if on}
    fs = {k: L.Dropout(dropout, name=f"{FEATURES[k]}_drop")(t) for k, t in ins.items()} if dropout else ins
    return keras.Model(list(ins.values()), _heads(p, fs), name="decidio_heads")


def head_layers(p: Project, model: keras.Model) -> dict[str, list]:
    """question id -> its layers that hold weights (gates, hidden, logits)."""
    out = {}
    for q in p.questions:
        names = {f"{q.qid}_hidden", f"{q.qid}_logits"} | {f"{q.qid}_gate_{k}" for k in FEATURES}
        out[q.qid] = [l for l in model.layers if l.name in names]
    return out


LOGIT_LIMIT = 12.0     # flashed logits are held to +-12: all the int8 output's 256 steps then cover the range that matters


def _bounded(x, name: str):
    """clip(x, -LOGIT_LIMIT, +LOGIT_LIMIT) from ops the firmware has (ADD, SUB, RELU). One very sure question (logit 60)
    would otherwise stretch the output's int8 scale for every question, leaving p_true only a handful of values."""
    a = L.ReLU(name=f"{name}_lo")(x + LOGIT_LIMIT)
    b = L.ReLU(name=f"{name}_hi")(2 * LOGIT_LIMIT - a)
    return LOGIT_LIMIT - b


def deployed(p: Project, model: keras.Model) -> keras.Model:
    """The model that gets flashed: auxiliary questions' heads cut off, logits bounded, joined in a way the board can run."""
    outs = [_bounded(model.get_layer(f"{q.qid}_logits").output, f"{q.qid}_bound") for q in p.deployed]
    return keras.Model(model.inputs, _join(outs, "logits_live"), name="decidio")


def macs(m: keras.Model) -> int:
    n = 0
    for l in m.layers:
        if isinstance(l, L.DepthwiseConv2D):
            _, h, w_, c = l.output_shape
            n += h * w_ * c * l.kernel_size[0] * l.kernel_size[1]
        elif isinstance(l, L.Conv2D):
            _, h, w_, c = l.output_shape
            n += h * w_ * c * l.kernel_size[0] * l.kernel_size[1] * l.input_shape[-1]
        elif isinstance(l, L.Dense):
            n += l.input_shape[-1] * l.units
    return n
