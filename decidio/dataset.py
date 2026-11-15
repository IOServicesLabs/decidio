"""prep: the sample store + labels -> out/dataset.npz (raw, un-augmented arrays; augmentation happens while training)."""
from __future__ import annotations

import hashlib

import numpy as np

from .config import CAM, Project
from .media import load_cam, log_mel, mel_filterbank, read_wav, fit_len
from .store import Store, targets


def _is_val(group: str, frac: float, seed: int) -> bool:
    h = int(hashlib.sha1(f"{seed}:{group}".encode()).hexdigest()[:8], 16)
    return (h % 10000) < frac * 10000


def build(p: Project, log=print, src: Project | None = None, save: bool = True, feat: tuple | None = None,
          all_val: bool = False) -> dict:
    """src: read samples/labels from another project (decidio test); feat: fixed (mean, std) for audio."""
    st = Store(src or p)
    metas = st.metas()
    teacher, labels = st.teacher(), st.labels()
    rows = []
    for sid, m in sorted(metas.items()):
        img, prev, wav = st.files(sid)
        if p.image is None:
            img = prev = None
        if p.audio is None:
            wav = None
        if img is None and wav is None:
            continue
        rows.append((sid, m, img, prev, wav))
    sids = [r[0] for r in rows]
    y = targets(p, sids, teacher, labels)
    keep = (y >= 0).any(1)
    rows = [r for r, k in zip(rows, keep) if k]
    y = y[keep]
    if len(rows) < 8:
        raise SystemExit(f"there are only {len(rows)} labeled samples, and 8 are necessary. Import, collect or label more samples")

    n = len(rows)
    groups = [r[1].group for r in rows]
    val = np.array([_is_val(g, p.train.val, p.train.seed) for g in groups])
    if all_val:
        val[:] = True
    elif val.all() or not val.any():
        val[:] = False
        val[np.random.default_rng(p.train.seed).permutation(n)[: max(1, n // 7)]] = True

    out = {"ids": np.array(sids if False else [r[0] for r in rows]), "groups": np.array(groups), "val": val, "y": y}
    if p.image:
        c = p.image.colors
        has_img = [r[2] is not None for r in rows]
        has_prev = [r[2] is not None and r[3] is not None for r in rows]
        img = np.zeros((sum(has_img), CAM, CAM, c), np.uint8)          # preallocated: big imports fit in memory
        prev = np.zeros((sum(has_prev), CAM, CAM, c), np.uint8)        # only real earlier frames (stills have none)
        img_idx, prev_idx = np.full(n, -1, np.int32), np.full(n, -1, np.int32)
        box_start, box_count, boxes = np.zeros(n, np.int32), np.zeros(n, np.int32), []
        ki = kp = 0
        for i, (sid, m, im, pv, _) in enumerate(rows):
            if im is None:
                continue
            img[ki] = load_cam(im, c)
            img_idx[i] = ki
            ki += 1
            if pv is not None:
                prev[kp] = load_cam(pv, c)
                prev_idx[i] = kp
                kp += 1
            box_start[i], box_count[i] = len(boxes), len(m.boxes)
            boxes += m.boxes
        out.update(img=img, prev=prev, img_idx=img_idx, prev_idx=prev_idx, box_start=box_start, box_count=box_count,
                   boxes=np.array(boxes, np.float32).reshape(-1, 4))
    if p.audio:
        a = p.audio
        aud_idx = np.full(n, -1, np.int32)
        pcms = []
        for i, (sid, m, _, _, wav) in enumerate(rows):
            if wav is None:
                continue
            pcm, sr = read_wav(wav)
            if sr != a.sample_rate:
                raise SystemExit(f"{wav}: the sample rate is {sr} Hz, but the project uses {a.sample_rate} Hz")
            aud_idx[i] = len(pcms)
            pcms.append(fit_len(pcm, a.clip))
        pcm = np.stack(pcms) if pcms else np.zeros((0, a.clip), np.int16)
        tr = [aud_idx[i] for i in range(n) if aud_idx[i] >= 0 and not val[i]]
        if feat is not None:
            mean, std = feat
        elif tr:
            fb = mel_filterbank(a)
            sel = np.array(tr)[np.random.default_rng(0).permutation(len(tr))[:2000]]
            m = np.concatenate([log_mel(pcm[ch], a, fb) for ch in np.array_split(sel, max(1, len(sel) // 256))])
            mean, std = float(m.mean()), float(m.std() + 1e-6)
        else:
            mean, std = -8.0, 4.0
        out.update(pcm=pcm, aud_idx=aud_idx, feat_mean=mean, feat_std=std)
    if save:
        p.out.mkdir(parents=True, exist_ok=True)
        np.savez(p.out / "dataset.npz", **out)

    has_i = (out.get("img_idx", np.full(n, -1)) >= 0)
    has_a = (out.get("aud_idx", np.full(n, -1)) >= 0)
    log(f"samples {n}: train {int((~val).sum())}, val {int(val.sum())} | image {int(has_i.sum())}, audio {int(has_a.sum())}, both {int((has_i & has_a).sum())}")
    for q in p.questions:
        yq = y[:, q.offset:q.offset + q.n]
        lab = (yq >= 0).all(1)
        dist = np.bincount(yq[lab].argmax(1), minlength=q.n)
        log(f"  {q.qid:<20} labeled {int(lab.sum()):>6}  " + "  ".join(f"{o}={d}" for o, d in zip(q.options, dist)))
    return out
