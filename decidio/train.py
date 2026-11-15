"""Training: on-the-fly augmentation, cross-modal pairing, masked (soft) cross-entropy, class balancing."""
from __future__ import annotations

import json
import math
import os

import numpy as np
from PIL import Image

from .config import CAM, Project
from .ego import simulate
from .media import cam_to_model, log_mel, mel_filterbank
from .model import build, keras, macs, tf


def load_dataset(p: Project) -> dict:
    path = p.out / "dataset.npz"
    if not path.exists():
        raise SystemExit("there is no out/dataset.npz. Run  decidio prep  first")
    d = dict(np.load(path, allow_pickle=False))
    if "prev_has" in d and "prev_idx" not in d:          # datasets written by decidio 0.1.0 before the lean format
        d["prev_idx"] = np.where(d["prev_has"], d["img_idx"], -1).astype(np.int32)
    return d


def recalibrate_bn(p: Project, model, d: dict, idx: np.ndarray, batches: int = 150, log=print) -> None:
    """Re-estimate BatchNorm statistics as an exact average over many clean batches (training-time running averages
    only remember the last few batches, which makes inference noisier than it needs to be)."""
    bns = [l for l in model.layers if isinstance(l, keras.layers.BatchNormalization)]
    if not bns or not len(idx):
        return
    seq = Batches(p, d, idx, False, [np.ones(q.n) for q in p.questions])
    order = np.random.default_rng(0).permutation(len(seq))[:batches]
    for l in bns:
        l._decidio_m = l.momentum
    for i, b in enumerate(order):
        for l in bns:
            l.momentum = i / (i + 1.0)                     # cumulative mean over the batches seen so far
        x, _ = seq[int(b)]
        model(x, training=True)
    for l in bns:
        l.momentum = l._decidio_m
    log(f"BatchNorm statistics recalibrated over {len(order)} batches")


def class_weights(p: Project, y: np.ndarray) -> list[np.ndarray]:
    out = []
    for q in p.questions:
        yq = y[:, q.offset:q.offset + q.n]
        lab = (yq >= 0).all(1)
        cnt = np.bincount(yq[lab].argmax(1), minlength=q.n).astype(np.float64) + 1.0
        w = cnt.sum() / (q.n * cnt)
        out.append(np.clip(w / w[np.bincount(yq[lab].argmax(1), minlength=q.n).argmax()] if lab.any() else w, 0.25, 5.0)
                   if p.train.balance else np.ones(q.n))
    return out


MIN_STEPS = 60


class Batches(keras.utils.Sequence):
    """Yields ({image, mel}, y+weights). Samples missing a modality are paired with a sample that has only that modality."""

    def __init__(self, p: Project, d: dict, idx: np.ndarray, train: bool, weights: list[np.ndarray],
                 moving: bool | None = None):
        super().__init__()
        self.p, self.d, self.idx, self.train, self.w = p, d, np.asarray(idx), train, weights
        # moving camera: photos get simulated camera motion (training: random; validation / test: fixed per sample)
        self.moving = bool(p.image and (p.image.platform == "moving" if moving is None else moving))
        self.rng = np.random.default_rng(p.train.seed + (0 if train else 1))
        self.bs = p.train.batch
        self.fb = mel_filterbank(p.audio) if p.audio else None
        n = len(d["y"])
        self.has_i = d["img_idx"] >= 0 if p.image else np.zeros(n, bool)
        self.has_a = d["aud_idx"] >= 0 if p.audio else np.zeros(n, bool)
        s = set(self.idx.tolist())
        self.img_pool = np.array([i for i in range(n) if i in s and self.has_i[i] and not self.has_a[i]], np.int64)
        self.aud_pool = np.array([i for i in range(n) if i in s and self.has_a[i] and not self.has_i[i]], np.int64)
        self.epoch = 0
        self.order = self._order()

    def _order(self):
        o = self.idx.copy()
        if self.train:
            need = MIN_STEPS * self.bs                       # small datasets: repeat (augmented) samples
            if len(o) < need:
                o = np.concatenate([o] * math.ceil(need / max(len(o), 1)))[:need]
            self.rng.shuffle(o)
            cap = self.p.train.max_per_epoch
            if cap and len(o) > cap:
                o = o[:cap]
        return o

    def on_epoch_end(self):
        self.epoch += 1
        self.order = self._order()

    def __len__(self):
        return max(1, math.ceil(len(self.order) / self.bs))

    def __getitem__(self, b):
        ids = self.order[b * self.bs:(b + 1) * self.bs]
        # one generator per batch: the same batches whether they are built in order or by parallel workers
        rng = np.random.default_rng([self.p.train.seed, self.epoch, int(b)] if self.train else int(b) + 1000)
        p, d = self.p, self.d
        ys, imgs, pcms = [], [], []
        for i in ids:
            y = d["y"][i].copy()
            if p.image:
                j = i if self.has_i[i] else self._partner(self.img_pool, rng)
                if j is not None and j != i:
                    y = merge(p, y, d["y"][j])
                imgs.append(self._image(j, rng))
            if p.audio:
                j = i if self.has_a[i] else self._partner(self.aud_pool, rng)
                if j is not None and j != i:
                    y = merge(p, y, d["y"][j])
                pcms.append(self._audio(j, rng))
            ys.append(y)
        y = np.stack(ys)
        wcols = []
        for q, w in zip(p.questions, self.w):
            yq = y[:, q.offset:q.offset + q.n]
            lab = (yq >= 0).all(1)
            wcols.append(np.where(lab, w[np.maximum(yq, 0).argmax(1)], 0.0))
        out = {}
        if p.image:
            out["image"] = np.stack(imgs).astype(np.float32) / 255.0
        if p.audio:
            m = log_mel(np.stack(pcms), p.audio, self.fb)
            out["mel"] = ((m - float(d["feat_mean"])) / float(d["feat_std"]))[..., None].astype(np.float32)
        return out, np.concatenate([y, np.stack(wcols, 1).astype(np.float32)], 1)

    def _partner(self, pool, rng):
        return int(pool[rng.integers(len(pool))]) if len(pool) else None

    # ------------------------------------------------------------ image
    def _image(self, j, rng) -> np.ndarray:
        c, d = self.p.image, self.d
        if j is None:
            cur = np.full((CAM, CAM, c.colors), 128, np.uint8)
            return cam_to_model(cur, cur, c)
        cur = d["img"][d["img_idx"][j]]
        pi = d["prev_idx"][j]
        still = pi < 0
        prev = cur if still else d["prev"][pi]
        if self.train:
            boxes = d["boxes"][d["box_start"][j]:d["box_start"][j] + d["box_count"][j]]
            cur, prev = augment_frames(cur, prev, boxes, still, c, rng, self.moving)
        else:
            if self.moving and still:
                cur, prev = simulate(cur, prev, np.random.default_rng(int(j) + 7919))
            cur, prev = sensor_noise(cur, prev, np.random.default_rng(int(j)))
        return cam_to_model(cur, prev, c)

    # ------------------------------------------------------------ audio
    def _audio(self, j, rng) -> np.ndarray:
        a = self.p.audio
        if j is None:
            # no sound of its own: a quiet room (a real mic never gives digital silence, and log-mel of zeros is extreme)
            return (rng.normal(0, 30, a.clip) if self.train else np.random.default_rng(7).normal(0, 30, a.clip)).astype(np.int16)
        x = self.d["pcm"][self.d["aud_idx"][j]]
        return augment_audio(x, a, rng) if self.train else x


def merge(p: Project, y: np.ndarray, other: np.ndarray) -> np.ndarray:
    """Union of two samples' labels; questions both answer differently become unknown."""
    y = y.copy()
    for q in p.questions:
        sl = slice(q.offset, q.offset + q.n)
        a, b = y[sl], other[sl]
        if (b < 0).any():
            continue
        if (a < 0).any():
            y[sl] = b
        elif a.argmax() != b.argmax():
            y[sl] = -1.0
    return y


def augment_frames(cur, prev, boxes, still, c, rng, moving=False):
    cur, prev = cur.astype(np.float32), prev.astype(np.float32)
    if c.motion and still:
        # a still photo has no motion channel: move some of its objects a little to make one
        prev = cur.copy()
        for x, y, w, h in boxes:
            if rng.random() < 0.5:
                dx, dy = rng.integers(-5, 6, 2)
                x0, y0 = int(max(0, x)), int(max(0, y))
                x1, y1 = int(min(CAM, x + w)), int(min(CAM, y + h))
                tx0, ty0 = max(0, x0 + dx), max(0, y0 + dy)
                tx1, ty1 = min(CAM, x1 + dx), min(CAM, y1 + dy)
                if tx1 > tx0 and ty1 > ty0:
                    prev[ty0:ty1, tx0:tx1] = cur[ty0 - dy:ty1 - dy, tx0 - dx:tx1 - dx]
        if rng.random() < 0.15:
            prev = prev * rng.uniform(0.9, 1.1)          # exposure step between frames
    if moving and (still or rng.random() < 0.5):
        # a camera on a drone / car / hand: its own motion between the frames, blur, rolling-shutter lean, shake
        cur, prev = simulate(np.clip(cur, 0, 255).astype(np.uint8), np.clip(prev, 0, 255).astype(np.uint8), rng)
        cur, prev = cur.astype(np.float32), prev.astype(np.float32)
    m = float(cur.mean())
    gain, bright = rng.uniform(0.7, 1.3), rng.uniform(0.7, 1.3)
    cur = (cur - m) * gain + m * bright
    prev = (prev - m) * gain + m * bright
    if c.colors == 3:
        tint = rng.uniform(0.9, 1.1, 3)
        cur, prev = cur * tint, prev * tint
    s = rng.uniform(0.8, 1.0) if c.crop else 1.0
    side = s * CAM
    x0, y0 = rng.uniform(0, CAM - side, 2) if c.crop else (0.0, 0.0)
    flip = c.flip and rng.random() < 0.5
    out = []
    for f in (cur, prev):
        f = np.clip(f, 0, 255).astype(np.uint8)
        im = Image.fromarray(f[..., 0] if c.colors == 1 else f)
        im = im.transform((CAM, CAM), Image.EXTENT, (x0, y0, x0 + side, y0 + side), Image.BILINEAR)
        if flip:
            im = im.transpose(Image.FLIP_LEFT_RIGHT)
        a = np.asarray(im, np.float32)
        a = (a[..., None] if c.colors == 1 else a) + rng.normal(0, 2.0, (CAM, CAM, c.colors))
        out.append(np.clip(a, 0, 255).astype(np.uint8))
    return out[0], out[1]


def sensor_noise(cur, prev, rng, sigma=2.0):
    """Independent pixel noise on both frames, as a real camera has (keeps the motion channel realistic)."""
    out = []
    for f in (cur, prev):
        out.append(np.clip(f.astype(np.float32) + rng.normal(0, sigma, f.shape), 0, 255).astype(np.uint8))
    return out[0], out[1]


def augment_audio(x, a, rng):
    y = x.astype(np.float32) * 10 ** (rng.uniform(-a.gain_db, a.gain_db) / 20)
    y = np.roll(y, rng.integers(-a.sample_rate // 10, a.sample_rate // 10 + 1))
    pw = float(np.mean(y ** 2)) + 1.0
    y = y + rng.normal(0, np.sqrt(pw / 10 ** (rng.uniform(25, 45) / 10)), y.shape)
    return np.clip(y, -32768, 32767).astype(np.int16)


def make_loss(p: Project):
    spans = [(q.offset, q.n) for q in p.questions]
    nl = p.n_logits

    def loss(yw, logits):
        y, w = yw[:, :nl], yw[:, nl:]
        tot, cnt = 0.0, 0.0
        for k, (off, n) in enumerate(spans):
            yq = tf.maximum(y[:, off:off + n], 0.0)
            ce = -tf.reduce_sum(yq * tf.nn.log_softmax(logits[:, off:off + n]), -1)
            tot += tf.reduce_sum(ce * w[:, k])
            cnt += tf.reduce_sum(tf.cast(w[:, k] > 0, tf.float32))
        return tot / tf.maximum(cnt, 1.0)

    return loss


def init_from(model, other_root: str, log=print) -> None:
    """Start from another project's trained weights: every layer with the same name and shapes is copied
    (the camera / microphone branches always match when the inputs match; heads match when question ids match)."""
    from .config import load
    other = load(other_root)
    if not (other.out / "model.keras").exists():
        raise SystemExit(f"{other.root} has no trained model (out/model.keras)")
    src = keras.models.load_model(other.out / "model.keras", compile=False)
    by_name = {l.name: l for l in src.layers}
    copied = 0
    for l in model.layers:
        s = by_name.get(l.name)
        if s is None or not l.weights:
            continue
        ws, wd = s.get_weights(), l.get_weights()
        if len(ws) == len(wd) and all(a.shape == b.shape for a, b in zip(ws, wd)):
            l.set_weights(ws)
            copied += 1
    log(f"initialised {copied} of {sum(1 for l in model.layers if l.weights)} layers from {other.name}")


def fit_workers(p: Project) -> dict:
    """Batches (augmentation, audio features) are built on the CPU: spread them over processes where the OS can fork
    them cheaply (Linux, macOS), over a few threads elsewhere."""
    n = p.train.workers
    if n == 0:
        return {}
    if os.name == "posix":
        return {"workers": n if n > 0 else max(1, min(12, (os.cpu_count() or 2) - 2)), "use_multiprocessing": True,
                "max_queue_size": 48}
    return {"workers": n if n > 0 else 4, "use_multiprocessing": False, "max_queue_size": 24}


def run(p: Project, epochs: int | None = None, log=print, init: str | None = None) -> keras.Model:
    tf.keras.utils.set_random_seed(p.train.seed)
    d = load_dataset(p)
    val = d["val"]
    tr_idx, va_idx = np.where(~val)[0], np.where(val)[0]
    # full training changes the model's body: a question kept from the base but without labels here would drift
    # with nothing to hold it (measured: person AUC 0.93 -> 0.83, the sound questions down to chance)
    adrift = [q.qid for q in p.questions if q.qid in p.keep
              and not (d["y"][tr_idx][:, q.offset:q.offset + q.n] >= 0).all(1).any()]
    if adrift:
        raise SystemExit(f"keep: {', '.join(adrift)} - this project has no labels for {'it' if len(adrift) == 1 else 'them'}. "
                         "Full training changes the body of the model, so kept questions need labels. "
                         "Do one of these steps: add labels to a source (the starter set has the classes of the base), "
                         "remove the questions from keep:, or use  decidio teach . "
                         "decidio teach trains only the heads. It does not change the body or the kept heads.")
    weights = class_weights(p, d["y"][tr_idx])
    tr = Batches(p, d, tr_idx, True, weights)
    va = Batches(p, d, va_idx, False, [np.ones(q.n) for q in p.questions])   # unbalanced: stable early stopping
    from . import model as _m
    _m.BN_MOMENTUM = 0.99 if len(tr) >= 150 else 0.9
    model = build(p)
    init = init or (str(p.base) if p.base else None)       # a project with base: starts from it
    if init:
        init_from(model, init, log)
    log(f"model: {model.count_params():,} params, {macs(model) / 1e6:.2f}M multiply-adds per decision")
    ep = epochs or p.train.epochs
    lr = p.train.lr if "lr" in (p.raw.get("train") or {}) else (1e-3 if p.model.get("backbone", "tiny") != "tiny" else 3e-3)
    sched = keras.optimizers.schedules.CosineDecay(lr, len(tr) * ep)
    model.compile(optimizer=keras.optimizers.AdamW(sched, weight_decay=1e-4), loss=make_loss(p))
    p.out.mkdir(parents=True, exist_ok=True)
    cbs = [keras.callbacks.CSVLogger(str(p.out / "train_log.csv")), keras.callbacks.TerminateOnNaN()]
    if p.train.patience > 0:
        cbs.append(keras.callbacks.EarlyStopping("val_loss", patience=p.train.patience, restore_best_weights=True))
    model.fit(tr, validation_data=va, epochs=ep, verbose=2, callbacks=cbs, **fit_workers(p))
    recalibrate_bn(p, model, d, tr_idx, log=log)
    model.save(p.out / "model.keras")
    (p.out / "train_info.json").write_text(json.dumps({
        "params": int(model.count_params()), "macs": int(macs(model)), "train": int(len(tr_idx)), "val": int(len(va_idx)),
    }, indent=2))
    log(f"saved {p.out / 'model.keras'}")
    return model


def predict_val(p: Project, model, d=None, int8_runner=None):
    """(targets, logits) on the validation split, inputs built exactly as in training (no augmentation)."""
    d = d or load_dataset(p)
    va_idx = np.where(d["val"])[0]
    seq = Batches(p, d, va_idx, False, [np.ones(q.n) for q in p.questions])
    ys, ls = [], []
    for b in range(len(seq)):
        x, yw = seq[b]
        ys.append(yw[:, :p.n_logits])
        ls.append(model.predict(x, verbose=0) if int8_runner is None else int8_runner(x))
    return np.concatenate(ys), np.concatenate(ls)
