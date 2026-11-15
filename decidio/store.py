"""The sample store of a project.

data/samples/<id>.png        current frame (96x96, gray or RGB)     - optional
data/samples/<id>_prev.png   frame motion_ms earlier                - optional (still photo when absent)
data/samples/<id>.wav        16-bit mono clip                       - optional
data/meta.jsonl              {"id", "group", "source", "boxes"}      group = split unit (no leakage)
data/labels.csv              hard labels: id,<question>,...          (imports, label UI, hand edits)
data/teacher.jsonl           soft labels: {"id", "probs": {q: [...]}, "teacher"}
"""
from __future__ import annotations

import csv
import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .config import Project
from .media import save_cam, write_wav


@dataclass
class Meta:
    sid: str
    group: str
    source: str = ""
    boxes: list = field(default_factory=list)     # [x, y, w, h] in 96-px frame coordinates


class Store:
    def __init__(self, p: Project):
        self.p = p
        self.dir = p.samples
        self.meta_path = p.data / "meta.jsonl"
        self.labels_path = p.data / "labels.csv"
        self.teacher_path = p.data / "teacher.jsonl"
        self._meta_buf: list[str] = []
        self._label_buf: dict[str, dict] = {}

    # ---------------------------------------------------------------- write
    def add(self, sid: str, image=None, prev=None, pcm=None, group=None, source="", boxes=None, labels=None):
        self.dir.mkdir(parents=True, exist_ok=True)
        if image is not None:
            save_cam(self.dir / f"{sid}.png", image)
        if prev is not None:
            save_cam(self.dir / f"{sid}_prev.png", prev)
        if pcm is not None:
            write_wav(self.dir / f"{sid}.wav", pcm, self.p.audio.sample_rate if self.p.audio else 16000)
        m = {"id": sid, "group": group or sid, "source": source}
        if boxes:
            m["boxes"] = [[round(float(v), 1) for v in b] for b in boxes]
        self._meta_buf.append(json.dumps(m))
        if labels:
            self._label_buf[sid] = {k: v for k, v in labels.items() if v is not None}
        if len(self._meta_buf) >= 500:
            self.flush()

    def flush(self) -> None:
        if self._meta_buf:
            self.p.data.mkdir(parents=True, exist_ok=True)
            with self.meta_path.open("a", encoding="utf-8") as f:
                f.write("\n".join(self._meta_buf) + "\n")
            self._meta_buf = []
        if self._label_buf:
            self.update_labels(self._label_buf)
            self._label_buf = {}

    # ---------------------------------------------------------------- read
    def metas(self) -> dict[str, Meta]:
        out = {}
        if self.meta_path.exists():
            for line in self.meta_path.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    d = json.loads(line)
                    out[d["id"]] = Meta(d["id"], d.get("group", d["id"]), d.get("source", ""), d.get("boxes", []))
        # files without meta (copied in by hand) are their own group
        if self.dir.exists():
            for f in self.dir.iterdir():
                stem = f.stem[:-5] if f.stem.endswith("_prev") else f.stem
                if f.suffix in (".png", ".wav") and stem not in out:
                    out[stem] = Meta(stem, stem, "files")
        return out

    def files(self, sid: str) -> tuple[Path | None, Path | None, Path | None]:
        img, prev, wav = self.dir / f"{sid}.png", self.dir / f"{sid}_prev.png", self.dir / f"{sid}.wav"
        return (img if img.exists() else None, prev if prev.exists() else None, wav if wav.exists() else None)

    def labels(self) -> dict[str, dict[str, str]]:
        out: dict[str, dict[str, str]] = {}
        if self.labels_path.exists():
            with self.labels_path.open(newline="", encoding="utf-8") as f:
                for r in csv.DictReader(f):
                    sid = (r.pop("id", "") or "").strip()
                    if sid:
                        out[sid] = {k: v.strip() for k, v in r.items() if k and v and v.strip()}
        return out

    def update_labels(self, updates: dict[str, dict]) -> None:
        """Merge {id: {question: value-or-''}}; '' clears a cell."""
        cur = self.labels()
        for sid, row in updates.items():
            r = cur.setdefault(sid, {})
            for k, v in row.items():
                if v in (None, ""):
                    r.pop(k, None)
                else:
                    r[k] = str(v)
        cols = [q.qid for q in self.p.questions]
        extra = sorted({k for r in cur.values() for k in r} - set(cols))
        self.p.data.mkdir(parents=True, exist_ok=True)
        tmp = self.labels_path.with_suffix(".tmp")
        with tmp.open("w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, ["id", *cols, *extra])
            w.writeheader()
            for sid in sorted(cur):
                if cur[sid]:
                    w.writerow({"id": sid, **cur[sid]})
        tmp.replace(self.labels_path)

    def teacher(self) -> dict[str, dict[str, list[float]]]:
        out: dict[str, dict[str, list[float]]] = {}
        if self.teacher_path.exists():
            for line in self.teacher_path.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    d = json.loads(line)
                    out.setdefault(d["id"], {}).update(d.get("probs", {}))
        return out

    def append_teacher(self, rows: list[dict]) -> None:
        self.p.data.mkdir(parents=True, exist_ok=True)
        with self.teacher_path.open("a", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")


def targets(p: Project, sids: list[str], teacher: dict, labels: dict) -> np.ndarray:
    """[N, n_logits] soft targets; -1 = unknown (masked). Hard labels override the teacher."""
    y = np.full((len(sids), p.n_logits), -1.0, np.float32)
    for i, sid in enumerate(sids):
        t, h = teacher.get(sid, {}), labels.get(sid, {})
        for q in p.questions:
            sl = slice(q.offset, q.offset + q.n)
            if q.qid in h:
                try:
                    k = p.option_index(q, h[q.qid])
                except ValueError:
                    continue
                y[i, sl] = 0.0
                y[i, q.offset + k] = 1.0
            elif q.qid in t and len(t[q.qid]) == q.n:
                v = np.clip(np.asarray(t[q.qid], np.float32), 0, None)
                y[i, sl] = v / max(float(v.sum()), 1e-6)
    return y
