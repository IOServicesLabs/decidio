"""Teachers: big models that label your samples so the tiny model can learn from them (soft labels -> data/teacher.jsonl).

  clef      Cloudflare Clef / Clef-Flash (typed decisions over image + text; audio is summarised as text first).
            Needs a CUDA GPU: Clef-Flash ~24 GB, Clef ~80 GB.   pip install "decidio[teacher]"
  zeroshot  CLIP for image questions + CLAP for audio questions, prompted with each option's criteria text.
            Runs on a normal GPU or (slowly) on CPU.            pip install "decidio[teacher]"
Hand labels in data/labels.csv always override teacher labels.
"""
from __future__ import annotations

import sys

import numpy as np
from PIL import Image

from .config import ConfigError, Project
from .media import load_cam, read_wav
from .store import Store


def _todo(p: Project, st: Store, relabel: bool, need: str | None = None):
    done = set() if relabel else set(st.teacher())
    out = []
    for sid in sorted(st.metas()):
        if sid in done:
            continue
        img, prev, wav = st.files(sid)
        if need == "image" and img is None or need == "audio" and wav is None:
            continue
        out.append((sid, img, prev, wav))
    return out


# ---------------------------------------------------------------- Clef
def _audio_text(pcm: np.ndarray, sr: int, tagger, asr) -> dict:
    x = pcm.astype(np.float32) / 32768.0
    rms = float(np.sqrt(np.mean(x ** 2)))
    tags = tagger({"raw": x, "sampling_rate": sr}, top_k=5)
    st = {"loudness_dbfs": round(20 * np.log10(rms + 1e-9), 1), "sounds": [f"{t['label']} ({t['score']:.2f})" for t in tags]}
    speech = max((t["score"] for t in tags if t["label"].lower().startswith("speech")), default=0.0)
    if asr is not None and speech >= 0.3:
        st["speech_transcript"] = asr({"raw": x, "sampling_rate": sr})["text"].strip()
    return st


def clef(p: Project, model_id: str = "Cloudflare/clef-flash", batch: int = 4, relabel: bool = False) -> None:
    import torch
    from huggingface_hub import snapshot_download
    from transformers import pipeline

    st = Store(p)
    todo = _todo(p, st, relabel)
    print(f"{len(todo)} samples to label with {model_id}")
    if not todo:
        return
    tagger = asr = None
    if p.audio:
        tagger = pipeline("audio-classification", model="MIT/ast-finetuned-audioset-10-10-0.4593", device=0)
        asr = pipeline("automatic-speech-recognition", model="openai/whisper-small", device=0)
    path = snapshot_download(model_id)
    sys.path.insert(0, path)
    from joint_schema_model import collate_records, encode_record, load_release_model  # noqa: E402

    model, processor = load_release_model(path, device="cuda")
    questions = p.teacher_questions()
    qm = p.qmap()
    task = p.teacher.get("task", "A frame and/or a second of audio from a small sensor. Decide from what is visible and audible.")
    for i in range(0, len(todo), batch):
        chunk, encs = todo[i:i + batch], []
        for sid, img, prev, wav in chunk:
            state = {"task": task}
            imgs = []
            if img is not None:
                cur = Image.open(img).convert("RGB").resize((384, 384), Image.BICUBIC)
                if prev is not None:
                    imgs.append(Image.open(prev).convert("RGB").resize((384, 384), Image.BICUBIC))
                    state["frames"] = f"images[0] is {p.image.motion_ms} ms before images[1] (now)"
                imgs.append(cur)
            if wav is not None and tagger is not None:
                pcm, sr = read_wav(wav)
                state["audio"] = _audio_text(pcm, sr, tagger, asr)
            rec = {"state": state, "questions": questions}
            if imgs:
                rec["images"] = imgs
            encs.append(encode_record(processor.tokenizer, rec, processor=processor))
        b = collate_records(encs, processor.tokenizer.pad_token_id, torch.device("cuda"))
        with torch.inference_mode():
            outs = model(b)
        rows = []
        for (sid, *_), enc, logits in zip(chunk, encs, outs):
            probs = {}
            for qe, ql in zip(enc.questions, logits):
                q = qm[qe.question_id]
                pr = ql.float().softmax(-1).tolist()
                v = [0.0] * q.n
                for oid, x in zip(qe.option_ids, pr):
                    try:
                        v[p.option_index(q, oid)] = float(x)
                    except ConfigError:
                        pass
                s = sum(v)
                if s > 0:
                    probs[q.qid] = [x / s for x in v]
            rows.append({"id": sid, "probs": probs, "teacher": model_id})
        st.append_teacher(rows)
        print(f"{min(i + batch, len(todo))}/{len(todo)}")


# ---------------------------------------------------------------- zero-shot CLIP / CLAP
def _prompts(p: Project, modality: str) -> dict[str, list[str]]:
    out = {}
    for q in p.questions:
        mod = q.modality or ("image" if p.image and not p.audio else "audio" if p.audio and not p.image else None)
        if mod != modality:
            continue
        texts = q.option_text()
        if not all(t.strip() for t in texts):
            raise ConfigError(f"{q.qid}: zero-shot needs a description for each option. "
                              f"For noul questions, use criteria: {{true: ..., false: ...}}")
        out[q.qid] = texts
    return out


def zeroshot(p: Project, image_model: str, audio_model: str, batch: int = 16, relabel: bool = False,
             temperature: float = 1.0) -> None:
    import torch
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    st = Store(p)
    rows: dict[str, dict] = {}
    if p.image and (prompts := _prompts(p, "image")):
        from transformers import CLIPModel, CLIPProcessor
        model, proc = CLIPModel.from_pretrained(image_model).to(dev).eval(), CLIPProcessor.from_pretrained(image_model)
        todo = _todo(p, st, relabel, "image")
        print(f"CLIP {image_model}: {len(todo)} images, questions {list(prompts)}")
        text = {q: _encode_text(model, proc, t, dev, clap=False) for q, t in prompts.items()}
        for i in range(0, len(todo), batch):
            ch = todo[i:i + batch]
            ims = [Image.fromarray(np.repeat(a, 3, -1) if a.shape[-1] == 1 else a)
                   for a in (load_cam(x[1], p.image.colors) for x in ch)]
            with torch.no_grad():
                f = model.get_image_features(**proc(images=ims, return_tensors="pt").to(dev))
                f = f / f.norm(dim=-1, keepdim=True)
            for q, tf in text.items():
                pr = ((model.logit_scale.exp() / temperature) * f @ tf.T).softmax(-1).cpu().numpy()
                for (sid, *_), v in zip(ch, pr):
                    rows.setdefault(sid, {})[q] = v.tolist()
            print(f"  {min(i + batch, len(todo))}/{len(todo)}")
    if p.audio and (prompts := _prompts(p, "audio")):
        from transformers import ClapModel, ClapProcessor
        model, proc = ClapModel.from_pretrained(audio_model).to(dev).eval(), ClapProcessor.from_pretrained(audio_model)
        todo = _todo(p, st, relabel, "audio")
        print(f"CLAP {audio_model}: {len(todo)} clips, questions {list(prompts)}")
        text = {q: _encode_text(model, proc, t, dev, clap=True) for q, t in prompts.items()}
        sr_model = proc.feature_extractor.sampling_rate
        for i in range(0, len(todo), batch):
            ch = todo[i:i + batch]
            clips = []
            for x in ch:
                pcm, sr = read_wav(x[3])
                t_new = np.arange(int(len(pcm) * sr_model / sr)) * sr / sr_model
                clips.append(np.interp(t_new, np.arange(len(pcm)), pcm.astype(np.float32) / 32768.0))
            with torch.no_grad():
                f = model.get_audio_features(**proc(audios=clips, sampling_rate=sr_model, return_tensors="pt").to(dev))
                f = f / f.norm(dim=-1, keepdim=True)
            for q, tf in text.items():
                pr = ((model.logit_scale_a.exp() / temperature) * f @ tf.T).softmax(-1).cpu().numpy()
                for (sid, *_), v in zip(ch, pr):
                    rows.setdefault(sid, {})[q] = v.tolist()
            print(f"  {min(i + batch, len(todo))}/{len(todo)}")
    tag = f"zeroshot:{image_model if p.image else ''}|{audio_model if p.audio else ''}"
    st.append_teacher([{"id": sid, "probs": pr, "teacher": tag} for sid, pr in rows.items()])
    print(f"wrote teacher labels for {len(rows)} samples")


def _encode_text(model, proc, texts, dev, clap: bool):
    import torch
    with torch.no_grad():
        t = proc(text=texts, return_tensors="pt", padding=True).to(dev)
        f = model.get_text_features(**t)
    return f / f.norm(dim=-1, keepdim=True)
