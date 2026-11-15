"""eval: per-question metrics on the held-out split, float and int8, written to out/report.md + out/report.json."""
from __future__ import annotations

import json

import numpy as np

from .config import Project
from .model import keras
from .train import load_dataset, predict_val


def metrics(p: Project, y: np.ndarray, logits: np.ndarray) -> dict:
    out = {}
    for q in p.questions:
        sl = slice(q.offset, q.offset + q.n)
        if sl.stop > logits.shape[1]:                # an auxiliary question: not in the flashed (int8) model
            continue
        yq, lq = y[:, sl], logits[:, sl]
        lab = (yq >= 0).all(1)
        if not lab.any():
            out[q.qid] = {"n": 0}
            continue
        t, pr = yq[lab].argmax(1), lq[lab].argmax(1)
        prob = np.exp(lq[lab] - lq[lab].max(1, keepdims=True))
        prob /= prob.sum(1, keepdims=True)
        m = {"n": int(lab.sum()), "accuracy": float((t == pr).mean()),
             "majority_baseline": float(np.bincount(t, minlength=q.n).max() / len(t))}
        conf = np.zeros((q.n, q.n), int)
        np.add.at(conf, (t, pr), 1)
        m["confusion"] = conf.tolist()
        m["per_option"] = {}
        for k, o in enumerate(q.options):
            tp, fn, fp = conf[k, k], conf[k].sum() - conf[k, k], conf[:, k].sum() - conf[k, k]
            m["per_option"][o] = {"n": int(conf[k].sum()), "recall": float(tp / max(tp + fn, 1)),
                                  "precision": float(tp / max(tp + fp, 1))}
        if q.qtype == "noul":
            s, pos = prob[:, 1], t == 1
            m["auc"] = auc(s, pos)
            best = max(((f1(s >= th, pos), th) for th in np.linspace(0.05, 0.95, 19)), default=(0, 0.5))
            m["f1@0.5"] = f1(s >= 0.5, pos)
            m["best_f1"], m["best_threshold"] = float(best[0]), float(best[1])
        if q.qtype == "score":
            m["mae"] = float(np.abs((prob * np.arange(q.n)).sum(1) - t).mean())
        out[q.qid] = m
    return out


def f1(pred, pos) -> float:
    tp, fp, fn = (pred & pos).sum(), (pred & ~pos).sum(), (~pred & pos).sum()
    return float(2 * tp / max(2 * tp + fp + fn, 1))


def auc(s, pos) -> float:
    if pos.all() or (~pos).all():
        return float("nan")
    order = np.argsort(s)
    ranks = np.empty(len(s))
    ranks[order] = np.arange(1, len(s) + 1)
    npos = pos.sum()
    return float((ranks[pos].sum() - npos * (npos + 1) / 2) / (npos * (len(s) - npos)))


def run(p: Project, runner=None, log=print) -> dict:
    d = load_dataset(p)
    model = keras.models.load_model(p.out / "model.keras", compile=False)
    y, lf = predict_val(p, model, d)
    res = {"float": metrics(p, y, lf)}
    if runner is not None:
        _, lq = predict_val(p, model, d, int8_runner=runner)
        res["int8"] = metrics(p, y, lq)
        res["int8_agreement"] = {q.qid: float((lf[:, q.offset:q.offset + q.n].argmax(1) ==
                                               lq[:, q.offset:q.offset + q.n].argmax(1)).mean()) for q in p.deployed}
    info = json.loads((p.out / "train_info.json").read_text()) if (p.out / "train_info.json").exists() else {}
    res["model"] = info
    (p.out / "report.json").write_text(json.dumps(res, indent=2))
    (p.out / "report.md").write_text(markdown(p, res))
    for q in p.questions:
        m = res["float"][q.qid]
        if not m.get("n"):
            log(f"  {q.qid:<20} no labeled validation samples")
            continue
        extra = f" auc {m['auc']:.3f}" if "auc" in m else ""
        q8 = f" | int8 {res['int8'][q.qid]['accuracy']:.3f}" if q.qid in res.get("int8", {}) else (" | aux, not flashed" if q.aux else "")
        log(f"  {q.qid:<20} n={m['n']:<6} acc {m['accuracy']:.3f} (majority {m['majority_baseline']:.3f}){extra}{q8}")
    log(f"wrote {p.out / 'report.md'}")
    return res


def markdown(p: Project, res: dict) -> str:
    info = res.get("model", {})
    L = [f"# {p.name} - evaluation", ""]
    if info:
        L += [f"Model: {info.get('params', 0):,} parameters, {info.get('macs', 0) / 1e6:.2f}M multiply-adds per decision. "
              f"Trained on {info.get('train', 0):,} samples, evaluated on {info.get('val', 0):,} held-out samples.", ""]
    L += ["| question | n | accuracy | majority baseline | int8 accuracy | notes |", "|---|---|---|---|---|---|"]
    for q in p.questions:
        m = res["float"].get(q.qid)
        if m is None:
            continue
        if not m.get("n"):
            L.append(f"| {q.qid} | 0 | | | | no labeled validation samples |")
            continue
        i8 = res.get("int8", {}).get(q.qid, {}).get("accuracy")
        note = f"AUC {m['auc']:.3f}, best F1 {m['best_f1']:.3f} at p>{m['best_threshold']:.2f}" if "auc" in m else (
            f"mean abs error {m['mae']:.2f}" if "mae" in m else "")
        if q.aux:
            note = (note + "; " if note else "") + "auxiliary: trained, not flashed"
        L.append(f"| {q.qid} | {m['n']} | {m['accuracy']:.3f} | {m['majority_baseline']:.3f} | "
                 f"{'' if i8 is None else f'{i8:.3f}'} | {note} |")
    for q in p.questions:
        m = res["float"].get(q.qid)
        if not m or not m.get("n") or q.n > 16:      # no confusion table for big auxiliary questions
            continue
        L += ["", f"## {q.qid}", "", "| option | n | recall | precision |", "|---|---|---|---|"]
        for o, v in m["per_option"].items():
            L.append(f"| {o} | {v['n']} | {v['recall']:.3f} | {v['precision']:.3f} |")
        L += ["", "Confusion (rows = truth, columns = prediction): " + ", ".join(q.options), "", "```"]
        L += ["  ".join(f"{c:>6}" for c in row) for row in m["confusion"]]
        L.append("```")
    return "\n".join(L) + "\n"


def test(p: Project, other_root: str, log=print, moving: bool | None = None) -> dict:
    """Run this project's exported int8 model (exactly what gets flashed) on another project's labelled samples.
    moving: True = as seen from a moving camera (simulated, fixed per sample), False = still, None = the project's own."""
    import json as _json
    from .config import load
    from .dataset import build
    from .export import Int8Runner
    from .train import Batches
    tfl = p.out / "model_int8.tflite"
    if not tfl.exists():
        raise SystemExit("there is no out/model_int8.tflite. Run  decidio export  first")
    hdr = _json.loads((p.out / "model_header.json").read_text())
    feat = (hdr["audio"]["mean"], hdr["audio"]["std"]) if hdr.get("audio") else None
    other = load(other_root)
    d = build(p, log=log, src=other, save=False, feat=feat, all_val=True)
    runner = Int8Runner(tfl.read_bytes(), p)
    model = keras.models.load_model(p.out / "model.keras", compile=False)
    idx = np.arange(len(d["y"]))
    seq = Batches(p, d, idx, False, [np.ones(q.n) for q in p.questions], moving=moving)
    ys, lf, lq = [], [], []
    for b in range(len(seq)):
        x, yw = seq[b]
        ys.append(yw[:, :p.n_logits])
        lf.append(model.predict(x, verbose=0))
        lq.append(runner(x))
    y, lf, lq = np.concatenate(ys), np.concatenate(lf), np.concatenate(lq)
    res = {"with": other.root.name,"float": metrics(p, y, lf), "int8": metrics(p, y, lq),
           "int8_agreement": {q.qid: float((lf[:, q.offset:q.offset + q.n].argmax(1) ==
                                            lq[:, q.offset:q.offset + q.n].argmax(1)).mean()) for q in p.deployed}}
    name = other.name.replace("/", "_") + ("-moving" if seq.moving else "")
    (p.out / f"test_{name}.json").write_text(_json.dumps(res, indent=2))
    md = markdown(p, {**res, "float": res["int8"], "model": {}}).replace(f"# {p.name} - evaluation",
                                                                          f"# {p.name} - test on {other.name}{' from a moving camera (simulated)' if seq.moving else ''} (int8)")
    (p.out / f"test_{name}.md").write_text(md)
    for q in p.deployed:
        m = res["int8"][q.qid]
        if m.get("n"):
            extra = f" auc {m['auc']:.3f}" if "auc" in m else ""
            log(f"  {q.qid:<20} n={m['n']:<6} int8 acc {m['accuracy']:.3f} (majority {m['majority_baseline']:.3f}){extra}"
                f"  float==int8 {res['int8_agreement'][q.qid]:.3f}")
    log(f"wrote {p.out / f'test_{name}.md'}")
    return res
