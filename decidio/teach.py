"""decidio teach: new questions on a trained base model, in about a minute.

The base's body (camera / microphone branches) stays exactly as it is. Every sample goes through it once, the
feature vectors are kept, and only the small per-question heads are trained on them. Questions listed under keep:
bring their trained head along from the base; they are trained further only if this project has labels for them.
The whole model is then converted to int8 again, so a kept answer can differ in the last digits from the base's.
"""
from __future__ import annotations

import json
import os
import time

import numpy as np

from .config import Project, load
from .model import build, features, head_layers, heads, keras, macs, tf
from .train import Batches, init_from, load_dataset, make_loss

MAX_FEATURES = 24000        # augmented samples run through the body at most


def _features(body, seq, passes: int) -> tuple[list[np.ndarray], np.ndarray]:
    xs, ys = None, []
    for k in range(passes):
        for b in range(len(seq)):
            x, yw = seq[b]
            f = body(x, training=False)
            f = [t.numpy() for t in (f if isinstance(f, (list, tuple)) else [f])]
            xs = [[v] for v in f] if xs is None else [a + [v] for a, v in zip(xs, f)]
            ys.append(yw)
        if k + 1 < passes:
            seq.on_epoch_end()
    return [np.concatenate(a) for a in xs], np.concatenate(ys)


def balanced_weights(p: Project, y: np.ndarray) -> list[np.ndarray]:
    """Every option of a question counts the same in total, however few examples it has. (Full training caps this
    at 5x; here a few dozen examples of the new thing stand against thousands of "everything else", and a head that
    is allowed to follow the counts simply answers "everything else".)"""
    out = []
    for q in p.questions:
        yq = y[:, q.offset:q.offset + q.n]
        lab = (yq >= 0).all(1)
        cnt = np.bincount(yq[lab].argmax(1), minlength=q.n).astype(np.float64)
        w = np.where(cnt > 0, cnt.sum() / (max(np.count_nonzero(cnt), 1) * np.maximum(cnt, 1)), 0.0)
        out.append(w if p.train.balance else np.ones(q.n))
    return out


def run(p: Project, epochs: int | None = None, log=print) -> keras.Model:
    if not p.base:
        raise SystemExit("teach needs  base: <name or folder>  in decidio.yaml. To see the shipped base models, "
                         "run  decidio bases . Without a base, use  decidio run")
    t0 = time.time()
    tf.keras.utils.set_random_seed(p.train.seed)
    base = load(p.base)
    d = load_dataset(p)
    tr_idx, va_idx = np.where(~d["val"])[0], np.where(d["val"])[0]

    model = build(p)
    init_from(model, str(p.base), log)
    src = keras.models.load_model(base.out / "model.keras", compile=False)
    theirs = {l.name: l for l in src.layers}
    hl = head_layers(p, model)
    head_names = {l.name for ls in hl.values() for l in ls}
    for l in model.layers:                                   # the whole body must come from the base
        if l.weights and l.name not in head_names:
            s = theirs.get(l.name)
            if s is None or any(a.shape != b.shape for a, b in zip(s.get_weights(), l.get_weights())):
                raise SystemExit(f"layer {l.name} does not agree with the base {base.name}. "
                                 "The inputs and the model must be the same as in the base")

    weights = balanced_weights(p, d["y"][tr_idx])
    tr = Batches(p, d, tr_idx, True, weights)
    va = Batches(p, d, va_idx, False, [np.ones(q.n) for q in p.questions])
    passes = max(1, min(4, MAX_FEATURES // max(len(tr.order), 1)))
    body = features(model)
    xt, yt = _features(body, tr, passes)
    xv, yv = _features(body, va, 1)
    log(f"features: {len(yt)} training views of {len(tr_idx)} samples, {len(yv)} validation samples "
        f"({time.time() - t0:.0f} s)")

    hm = heads(p, dropout=float(os.environ.get("DECIDIO_TEACH_DROPOUT", 0.3)))
    mine = {l.name: l for l in hm.layers}
    labelled = {q.qid: bool((d["y"][tr_idx][:, q.offset:q.offset + q.n] >= 0).all(1).any()) for q in p.questions}
    for q in p.questions:
        kept = q.qid in p.keep
        if not labelled[q.qid] and not kept:
            raise SystemExit(f"{q.qid}: there are no labelled training samples. "
                             "Import or label samples, or add the question to keep:")
        for l in hl[q.qid]:
            mine[l.name].set_weights(l.get_weights())        # kept heads start as the base's; new ones as initialised
            mine[l.name].trainable = labelled[q.qid]         # a kept question without labels here stays as it is
        log(f"  {q.qid:<20} " + ("kept from the base" + (", trained further" if labelled[q.qid] else "") if kept else "new"))

    ep = epochs or (p.train.epochs if "epochs" in (p.raw.get("train") or {}) else 40)
    bs = p.train.batch
    steps = max(1, int(np.ceil(len(yt) / bs)))
    lr = p.train.lr if "lr" in (p.raw.get("train") or {}) else 3e-3
    hm.compile(optimizer=keras.optimizers.Adam(keras.optimizers.schedules.CosineDecay(lr, steps * ep)), loss=make_loss(p))
    hm.fit(xt, yt, validation_data=(xv, yv) if len(yv) else None, epochs=ep, batch_size=bs, shuffle=True, verbose=0,
           callbacks=[keras.callbacks.CSVLogger(str(p.out / "train_log.csv"))])
    for ls in hl.values():
        for l in ls:
            l.set_weights(mine[l.name].get_weights())

    p.out.mkdir(parents=True, exist_ok=True)
    model.save(p.out / "model.keras")
    (p.out / "train_info.json").write_text(json.dumps({
        "params": int(model.count_params()), "macs": int(macs(model)), "train": int(len(tr_idx)), "val": int(len(va_idx)),
        "base": base.name, "taught": [q.qid for q in p.questions if labelled[q.qid]],
        "kept": [q.qid for q in p.questions if q.qid in p.keep], "seconds": round(time.time() - t0, 1),
    }, indent=2))
    log(f"heads trained in {time.time() - t0:.0f} s; saved {p.out / 'model.keras'}")
    return model
