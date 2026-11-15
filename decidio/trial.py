"""decidio try: the exported int8 model on your own photos and sounds, on the PC, with the board's preprocessing."""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from PIL import Image

from .config import CAM, Project
from .importers import AUDIO_EXT, IMAGE_EXT
from .media import cam_to_model, center_square, decode_audio, log_mel, loud_windows, mel_filterbank, pil_to_cam


def answers(p: Project, logits: np.ndarray) -> dict:
    out = {}
    for q in p.deployed:
        z = logits[q.offset:q.offset + q.n].astype(np.float64)
        pr = np.exp(z - z.max())
        pr /= pr.sum()
        if q.qtype == "noul":
            out[q.qid] = {"p_true": float(pr[1])}
        elif q.qtype == "choice":
            k = int(pr.argmax())
            out[q.qid] = {"choice": q.options[k], "confidence": float(pr[k]),
                          "probabilities": {o: round(float(v), 3) for o, v in zip(q.options, pr)}}
        else:
            out[q.qid] = {"score": float((pr * np.arange(q.n)).sum())}
    return out


def line(ans: dict) -> str:
    parts = []
    for q, a in ans.items():
        if "p_true" in a:
            parts.append(f"{q} {a['p_true']:.2f}")
        elif "choice" in a:
            parts.append(f"{q} {a['choice']} ({a['confidence']:.2f})")
        else:
            parts.append(f"{q} {a['score']:.1f}")
    return " | ".join(parts)


def run(p: Project, files: list[str], sound: str | None = None, as_json: bool = False, log=print) -> list[dict]:
    from .export import Int8Runner

    tfl = p.out / "model_int8.tflite"
    if not tfl.exists():
        raise SystemExit(f"there is no {tfl}. Run  decidio export  first")
    runner = Int8Runner(tfl.read_bytes(), p)
    hdr = json.loads((p.out / "model_header.json").read_text())
    fb = mel_filterbank(p.audio) if p.audio else None

    def mel_of(pcm):
        m = log_mel(pcm[None], p.audio, fb)[0]
        return ((m - hdr["audio"]["mean"]) / hdr["audio"]["std"])[None, ..., None].astype(np.float32)

    def clip_of(path):
        # the loudest clip_seconds of the file (what an import would keep)
        return loud_windows(decode_audio(Path(path), p.audio.sample_rate), p.audio.clip, 1)[0].astype(np.int16)

    quiet = np.random.default_rng(7).normal(0, 30, p.audio.clip).astype(np.int16) if p.audio else None
    fixed_pcm = clip_of(sound) if (sound and p.audio) else None
    gray = np.full((CAM, CAM, p.image.colors), 128, np.uint8) if p.image else None
    paths = []
    for f in files:
        f = Path(f)
        paths += sorted(x for x in f.rglob("*") if x.suffix.lower() in IMAGE_EXT | AUDIO_EXT) if f.is_dir() else [f]
    results = []
    for f in paths:
        ext = f.suffix.lower()
        x = {}
        if ext in IMAGE_EXT:
            if not p.image:
                log(f"{f.name}: skipped (this model has no camera input)")
                continue
            im, _ = center_square(Image.open(f))
            cur = pil_to_cam(im, p.image.colors)
            x["image"] = (cam_to_model(cur, cur, p.image)[None].astype(np.float32) / 255.0)
            if p.audio:
                x["mel"] = mel_of(fixed_pcm if fixed_pcm is not None else quiet)
        elif ext in AUDIO_EXT:
            if not p.audio:
                log(f"{f.name}: skipped (this model has no microphone input)")
                continue
            x["mel"] = mel_of(clip_of(f))
            if p.image:
                x["image"] = cam_to_model(gray, gray, p.image)[None].astype(np.float32) / 255.0
        else:
            log(f"{f.name}: skipped (the file type is not known. "
                f"Images: {' '.join(sorted(IMAGE_EXT))}. Sounds: {' '.join(sorted(AUDIO_EXT))})")
            continue
        ans = answers(p, runner(x)[0])
        results.append({"file": str(f), "answers": ans})
        log(json.dumps(results[-1]) if as_json else f"{f.name:32s} {line(ans)}")
    return results
