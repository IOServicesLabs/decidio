"""Bring outside data into a project's sample store.

Source types (decidio.yaml -> sources: <name>: {type: ..., path: ..., labels: ...}):

  folders   <path>/<class>/<files>     images (.jpg .png .bmp .webp) and/or audio (.wav .mp3 .flac .ogg .m4a ...)
  voc       Pascal VOC (zip or folder with Annotations/ + JPEGImages/)
  coco      COCO json (annotations/instances_*.json + image folder or zip; fetch: true downloads missing images)
  yolo      YOLO / Ultralytics / Roboflow export (data.yaml or images/ + labels/ folders, zip or folder)
  esc50     ESC-50 environmental sounds (folder or zip with meta/esc50.csv + audio/)
  speech_commands Speech Commands v0.02 spoken words (folder with <word>/*.wav + testing_list.txt)
  urbansound8k  UrbanSound8K street sounds (folder or zip with metadata/UrbanSound8K.csv + audio/fold*/)
  video     video files (any format ffmpeg reads): paired frames + audio; labels from parent folder names
  quiet     generated quiet-room clips, class "quiet" (count: 200) - so a sound question can answer "nothing"
  starter   the shared starter set: COCO photos + ESC-50 sounds + quiet clips (decidio fetch starter; see starter.py)
"""
from __future__ import annotations

import csv
import io
import json
import re
import subprocess
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image

from . import rules as R
from .config import CAM, ConfigError, Project
from .media import decode_audio, ffmpeg_exe, fit_len, loud_windows, pil_to_cam
from .store import Store

COCO80 = ["person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
          "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow", "elephant",
          "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
          "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
          "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange", "broccoli",
          "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant", "bed", "dining table", "toilet",
          "tv", "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
          "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"]
IMAGE_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
AUDIO_EXT = {".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".opus", ".wma"}
VIDEO_EXT = {".mp4", ".mov", ".avi", ".mkv", ".webm", ".m4v", ".3gp"}


def log(*a):
    print(*a, flush=True)


def slug(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "-", str(s)).strip("-")[:60] or "x"


# ---------------------------------------------------------------- file trees (folder or zip)

class Tree:
    """Uniform read access to a folder or a zip archive."""

    def __init__(self, path: Path | str):
        self.path = Path(path)
        if not self.path.exists():
            raise ConfigError(f"{self.path} does not exist")
        self.zip = zipfile.ZipFile(self.path) if self.path.is_file() and zipfile.is_zipfile(self.path) else None
        if self.zip:
            self._names = [n for n in self.zip.namelist() if not n.endswith("/")]
        else:
            self._names = [str(f.relative_to(self.path)).replace("\\", "/") for f in self.path.rglob("*") if f.is_file()]

    def names(self) -> list[str]:
        return self._names

    def read(self, name: str) -> bytes:
        return self.zip.read(name) if self.zip else (self.path / name).read_bytes()

    def local(self, name: str) -> Path | None:
        return None if self.zip else self.path / name


def _open_image(data: bytes) -> Image.Image:
    im = Image.open(io.BytesIO(data))
    im.load()
    return im


# ---------------------------------------------------------------- detection sources

def square_crops(w: int, h: int, n: int) -> list[tuple[int, int, int]]:
    s = min(w, h)
    span = max(w, h) - s
    if n <= 1 or span < s * 0.1:
        offs = [span // 2]
    else:
        offs = sorted({int(round(span * i / (n - 1))) for i in range(n)})
    return [(o, 0, s) if w >= h else (0, o, s) for o in offs]


def crop_objects(boxes, crop, min_size: float) -> tuple[list[R.Obj], list[list[float]]]:
    """boxes: [(cls, x0, y0, x1, y1, difficult)] in image px -> objects in the crop + visible boxes in 96-px coords."""
    cx, cy, s = crop
    objs, vis = [], []
    for cls, x0, y0, x1, y1, hard in boxes:
        ix0, iy0, ix1, iy1 = max(x0, cx), max(y0, cy), min(x1, cx + s), min(y1, cy + s)
        if ix1 <= ix0 or iy1 <= iy0:
            continue
        area_box = max((x1 - x0) * (y1 - y0), 1e-6)
        inter = (ix1 - ix0) * (iy1 - iy0)
        visible = (not hard) and inter / area_box >= 0.6 and max(ix1 - ix0, iy1 - iy0) / s >= min_size
        objs.append(R.Obj(cls, inter / (s * s), visible))
        if visible:
            k = CAM / s
            vis.append([(ix0 - cx) * k, (iy0 - cy) * k, (ix1 - ix0) * k, (iy1 - iy0) * k])
    return objs, vis


def object_crops(w: int, h: int, boxes, margin: float, n_bg: int, rng) -> list[tuple[int, int, int]]:
    """Square crops around each object (box + margin, like a fovea look) plus n_bg random background squares."""
    out = []
    for cls, x0, y0, x1, y1, hard in boxes:
        if hard:
            continue
        s = int(max(x1 - x0, y1 - y0) * (1 + 2 * margin))
        s = max(16, min(s, w, h))
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        out.append((int(min(max(0, cx - s / 2), w - s)), int(min(max(0, cy - s / 2), h - s)), s))
    for _ in range(n_bg):
        s = int(rng.uniform(0.2, 0.5) * min(w, h))
        out.append((int(rng.integers(0, w - s + 1)), int(rng.integers(0, h - s + 1)), s))
    return out


def _add_detection_image(p, st, rules, im, boxes, sid, group, source, spec, extra=None):
    w, h = im.size
    mode = spec.get("crops", 2)
    if mode == "objects":
        rng = np.random.default_rng(zlib.crc32(sid.encode()))
        crops = object_crops(w, h, boxes, float(spec.get("margin", 0.25)), int(spec.get("background", 1)), rng)
    else:
        crops = square_crops(w, h, int(mode))
    for ci, crop in enumerate(crops):
        objs, vis = crop_objects(boxes, crop, float(spec.get("min_size", 0.12)))
        labels = R.for_objects(p, rules, objs)
        if extra:
            labels.update(extra)               # whole-photo answers (from captions) hold for every crop
        if all(v is None for v in labels.values()):
            continue
        cx, cy, s = crop
        img = pil_to_cam(im.crop((cx, cy, cx + s, cy + s)), p.image.colors)
        st.add(f"{sid}-{ci}", image=img, group=group, source=source, boxes=vis, labels=labels)


def import_voc(p: Project, name: str, spec: dict, limit: int | None) -> int:
    rules = R.check(p, spec.get("labels"))
    n = 0
    st = Store(p)
    for path in _paths(spec):
        t = Tree(path)
        anns = sorted(x for x in t.names() if "/Annotations/" in "/" + x and x.endswith(".xml"))
        imgs = {Path(x).stem: x for x in t.names() if "/JPEGImages/" in "/" + x}
        log(f"{name}: {path} - {len(anns)} annotations")
        for a in anns:
            root = ET.fromstring(t.read(a))
            stem = Path(a).stem
            if stem not in imgs:
                continue
            boxes = []
            for o in root.iter("object"):
                bb = o.find("bndbox")
                boxes.append((o.findtext("name", "").strip().lower(), float(bb.findtext("xmin")), float(bb.findtext("ymin")),
                              float(bb.findtext("xmax")), float(bb.findtext("ymax")), o.findtext("difficult", "0") == "1"))
            year = re.search(r"VOC(\d{4})", a)
            gid = f"voc{year.group(1) if year else ''}-{stem}"
            _add_detection_image(p, st, rules, _open_image(t.read(imgs[stem])), boxes, f"{slug(name)}-{gid}", gid, name, spec)
            n += 1
            if n % 1000 == 0:
                log(f"  {n} images")
            if limit and n >= limit:
                break
    st.flush()
    return n


def import_coco(p: Project, name: str, spec: dict, limit: int | None) -> int:
    rules = R.check(p, spec.get("labels"))
    ann_path = Path(spec["annotations"])
    images = Tree(spec["images"]) if spec.get("images") else None
    fetch = bool(spec.get("fetch", False))
    cache = p.root / "downloads" / "coco_images"
    log(f"{name}: reading {ann_path}")
    d = _coco_annotations(ann_path)
    cats = {c["id"]: c["name"].lower() for c in d["categories"]}
    wanted = {c for r in rules.values() for v in r.values() if v != "*" for c in v}
    by_img: dict[int, list] = {}
    for a in d["annotations"]:
        x, y, w, h = a["bbox"]
        by_img.setdefault(a["image_id"], []).append((cats[a["category_id"]], x, y, x + w, y + h, bool(a.get("iscrowd"))))
    ims = [im for im in d["images"]]
    cap_rules = R.check(p, spec.get("caption_labels"))
    caps: dict[int, list[str]] = {}
    if cap_rules:
        cpath = Path(spec.get("captions") or str(ann_path).replace("instances_", "captions_"))
        if not cpath.is_absolute():
            cpath = p.root / cpath
        for a in _coco_annotations(cpath)["annotations"]:
            caps.setdefault(a["image_id"], []).append(a["caption"])
    # keep images with a wanted class, plus as many without (negatives), capped by max_images
    pos = [im for im in ims if any(b[0] in wanted for b in by_img.get(im["id"], []))]
    pos_ids = {im["id"] for im in pos}
    neg = [im for im in ims if im["id"] not in pos_ids] if spec.get("negatives", True) else []
    cap = int(spec.get("max_images", 0)) or len(ims)
    rng = np.random.default_rng(0)
    rng.shuffle(pos)
    rng.shuffle(neg)
    if int(spec.get("max_images", 0)):
        sel = pos[: int(cap * 0.7)] + neg[: cap - min(len(pos), int(cap * 0.7))]
    else:
        sel = pos + neg                        # no cap: every photo
    if limit:
        sel = sel[:limit]
    names = {Path(x).name: x for x in images.names()} if images else {}

    def get(im):
        fn = im["file_name"]
        if fn in names:
            return images.read(names[fn])
        local = cache / fn
        if local.exists():
            return local.read_bytes()
        if fetch and im.get("coco_url"):
            cache.mkdir(parents=True, exist_ok=True)
            data = urllib.request.urlopen(im["coco_url"], timeout=60).read()
            local.write_bytes(data)
            return data
        return None

    st, n, missing = Store(p), 0, 0
    with ThreadPoolExecutor(8) as ex:
        for im, data in zip(sel, ex.map(get, sel)):
            if data is None:
                missing += 1
                continue
            gid = f"coco-{im['id']}"
            extra = R.for_captions(p, cap_rules, caps.get(im["id"], [])) if cap_rules else None
            _add_detection_image(p, st, rules, _open_image(data), by_img.get(im["id"], []), f"{slug(name)}-{gid}", gid, name, spec,
                                 extra)
            n += 1
            if n % 1000 == 0:
                log(f"  {n} images")
    st.flush()
    if missing:
        log(f"  {missing} images are not on this computer. To download them, set fetch: true")
    return n


def _coco_annotations(path: Path) -> dict:
    """The annotation json, or the same file inside a downloaded annotations*.zip next to (or one above) its folder."""
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8"))
    for folder in (path.parent, path.parent.parent):
        for z in sorted(folder.glob("annotations*.zip")):
            with zipfile.ZipFile(z) as zf:
                for n in zf.namelist():
                    if n.endswith("/" + path.name) or n == path.name:
                        return json.loads(zf.read(n).decode("utf-8"))
    raise ConfigError(f"{path} does not exist, and no annotations*.zip file contains it")


def import_yolo(p: Project, name: str, spec: dict, limit: int | None) -> int:
    import yaml
    rules = R.check(p, spec.get("labels"))
    st, n = Store(p), 0
    for path in _paths(spec):
        t = Tree(path)
        names = spec.get("names")
        if names == "coco80":
            names = COCO80
        if not names:
            dy = next((x for x in t.names() if Path(x).name in ("data.yaml", "dataset.yaml")), None)
            if dy:
                names = (yaml.safe_load(t.read(dy)) or {}).get("names")
        if isinstance(names, dict):
            names = [names[k] for k in sorted(names, key=int)]
        if not names:
            raise ConfigError(f"{name}: no class names found. Add names: [...] or names: coco80 to the source")
        names = [str(x).lower() for x in names]
        label_files = {re.sub(r"/labels/", "/images/", "/" + x)[1:].rsplit(".", 1)[0]: x for x in t.names()
                       if x.endswith(".txt") and "/labels/" in "/" + x}
        imgs = [x for x in t.names() if Path(x).suffix.lower() in IMAGE_EXT and "/images/" in "/" + x]
        log(f"{name}: {path} - {len(imgs)} images, {len(names)} classes")
        for x in sorted(imgs):
            im = _open_image(t.read(x))
            w, h = im.size
            boxes = []
            lf = label_files.get(x.rsplit(".", 1)[0])
            if lf:
                for line in t.read(lf).decode().splitlines():
                    v = line.split()
                    if len(v) >= 5:
                        c, cx, cy, bw, bh = int(v[0]), *map(float, v[1:5])
                        boxes.append((names[c], (cx - bw / 2) * w, (cy - bh / 2) * h, (cx + bw / 2) * w, (cy + bh / 2) * h, False))
            gid = f"yolo-{slug(Path(x).stem)}"
            _add_detection_image(p, st, rules, im, boxes, f"{slug(name)}-{gid}", gid, name, spec)
            n += 1
            if limit and n >= limit:
                break
    st.flush()
    return n


# ---------------------------------------------------------------- classification sources

def _class_rules(p: Project, spec: dict) -> dict:
    if spec.get("labels"):
        return R.check(p, spec["labels"])
    q = spec.get("question")
    if not q:
        raise ConfigError("folders: add labels: {...} or question: <id> to the source "
                          "(with question:, the folder names are the option ids)")
    qq = p.qmap().get(q)
    if not qq:
        raise ConfigError(f"unknown question {q!r}")
    return R.check(p, {q: {o: [o] for o in qq.options}})


def import_folders(p: Project, name: str, spec: dict, limit: int | None) -> int:
    """<path>/<class>/<files>. Options: class: <name> (every file is this class, whatever its folder),
    per_class: N (at most N files per class, an even spread), windows: K (1 s windows per audio file)."""
    rules = _class_rules(p, spec)
    k = int(spec.get("windows", 3))
    one, per = spec.get("class"), int(spec.get("per_class", 0))
    st, n = Store(p), 0
    for path in _paths(spec):
        t = Tree(path)
        files = sorted(x for x in t.names() if (one or "/" in x) and Path(x).suffix.lower() in IMAGE_EXT | AUDIO_EXT)
        log(f"{name}: {path} - {len(files)} files")
        by_cls: dict[str, list[str]] = {}
        for x in files:
            by_cls.setdefault(str(one).lower() if one else Path(x).parent.name.lower(), []).append(x)
        for cls, xs in by_cls.items():
            labels = R.for_class(p, rules, cls)
            if all(v is None for v in labels.values()):
                continue
            if per and len(xs) > per:
                xs = [xs[i] for i in np.linspace(0, len(xs) - 1, per).astype(int)]
            for x in xs:
                ext = Path(x).suffix.lower()
                gid = f"{slug(name)}-{slug(Path(x).parent.name)}-{slug(Path(x).stem)}"
                if ext in IMAGE_EXT:
                    if not p.image:
                        continue
                    try:
                        im = _open_image(t.read(x))
                    except Exception:              # a broken file in a scraped set
                        continue
                    crop, _ = _center(im)
                    st.add(gid, image=pil_to_cam(crop, p.image.colors), group=gid, source=name, labels=labels)
                else:
                    if not p.audio:
                        continue
                    pcm = _audio_from_tree(t, x, p.audio.sample_rate)
                    for wi, clip in enumerate(loud_windows(pcm, p.audio.clip, k)):
                        st.add(f"{gid}-w{wi}", pcm=clip, group=gid, source=name, labels=labels)
                n += 1
                if limit and n >= limit:
                    break
            if limit and n >= limit:
                break
    st.flush()
    return n


def import_speech_commands(p: Project, name: str, spec: dict, limit: int | None) -> int:
    """Speech Commands v0.02: <word>/<speaker>_nohash_<n>.wav, one-second words, + _background_noise_ (class "noise").
    split: train (default; everything not in testing_list.txt) or test. per_class: N words per class (default 300).
    Every clip is also class "speech" for rules that do not list its word (e.g. speech_present: {true: [speech]})."""
    if not p.audio:
        raise ConfigError("speech_commands needs inputs.audio")
    rules = R.check(p, spec.get("labels"))
    per, split = int(spec.get("per_class", 300)), str(spec.get("split", "train"))
    st, n = Store(p), 0
    for path in _paths(spec):
        t = Tree(path)
        names = [x for x in t.names() if x.endswith(".wav")]
        tl = next((x for x in t.names() if x.endswith("testing_list.txt")), None)
        test = set(t.read(tl).decode().split()) if tl else set()
        by_word: dict[str, list[str]] = {}
        for x in sorted(names):
            word, key = Path(x).parent.name, "/".join(x.split("/")[-2:])
            if word.startswith("_"):
                if split == "train":
                    by_word.setdefault("noise", []).append(x)
            elif (key in test) == (split == "test"):
                by_word.setdefault(word.lower(), []).append(x)
        log(f"{name}: {path} - {sum(map(len, by_word.values()))} clips, {len(by_word)} classes ({split})")
        for word, xs in by_word.items():
            labels = _speech_labels(p, rules, word)
            if all(v is None for v in labels.values()):
                continue
            if word == "noise":                    # a few long recordings: cut into one-second pieces
                for x in xs:
                    pcm = _audio_from_tree(t, x, p.audio.sample_rate)
                    stem = slug(Path(x).stem)
                    for wi in range(min(len(pcm) // p.audio.clip, 40)):
                        st.add(f"{slug(name)}-noise-{stem}-{wi}", pcm=pcm[wi * p.audio.clip:(wi + 1) * p.audio.clip],
                               group=f"sc-noise-{stem}", source=name, labels=labels)
                    n += 1
                continue
            if per and len(xs) > per:
                xs = [xs[i] for i in np.linspace(0, len(xs) - 1, per).astype(int)]
            for x in xs:
                pcm = _audio_from_tree(t, x, p.audio.sample_rate)
                speaker = Path(x).stem.split("_")[0]   # one speaker stays on one side of the split
                st.add(f"{slug(name)}-{word}-{slug(Path(x).stem)}", pcm=fit_len(pcm, p.audio.clip), group=f"sc-{speaker}",
                       source=name, labels=labels)
                n += 1
                if limit and n >= limit:
                    break
            if limit and n >= limit:
                break
    st.flush()
    return n


def _speech_labels(p: Project, rules: dict, word: str) -> dict:
    """Answers for a spoken word: by the word where a rule lists it, else by the class "speech"."""
    if word == "noise":
        return R.for_class(p, rules, "noise")
    listed = {q for q, r in rules.items() if any(v != "*" and word in v for v in r.values())}
    a, b = R.for_class(p, rules, word), R.for_class(p, rules, "speech")
    return {q: (a[q] if q in listed else b[q]) for q in rules}


def import_esc50(p: Project, name: str, spec: dict, limit: int | None) -> int:
    if not p.audio:
        raise ConfigError("esc50 needs inputs.audio")
    rules = R.check(p, spec.get("labels"))
    k = int(spec.get("windows", 2))
    st, n = Store(p), 0
    for path in _paths(spec):
        t = Tree(path)
        meta = next((x for x in t.names() if x.endswith("meta/esc50.csv")), None)
        if not meta:
            raise ConfigError(f"{path}: meta/esc50.csv not found")
        audio = {Path(x).name: x for x in t.names() if "/audio/" in "/" + x and x.endswith(".wav")}
        rows = list(csv.DictReader(io.StringIO(t.read(meta).decode())))
        log(f"{name}: {path} - {len(rows)} clips")
        folds = {str(f) for f in spec.get("folds") or []}      # folds: [2, 3, 4, 5] = only these of ESC-50's five
        for r in rows:
            labels = R.for_class(p, rules, r["category"])
            if all(v is None for v in labels.values()) or r["filename"] not in audio:
                continue
            if folds and str(r.get("fold")) not in folds:
                continue
            pcm = _audio_from_tree(t, audio[r["filename"]], p.audio.sample_rate)
            gid = f"esc50-{r['src_file']}"
            for wi, clip in enumerate(loud_windows(pcm, p.audio.clip, k)):
                st.add(f"{slug(name)}-{slug(Path(r['filename']).stem)}-w{wi}", pcm=clip, group=gid, source=name, labels=labels)
            n += 1
            if limit and n >= limit:
                break
    st.flush()
    return n


def import_urbansound8k(p: Project, name: str, spec: dict, limit: int | None) -> int:
    """UrbanSound8K: 8732 clips (<= 4 s) in 10 classes incl. car_horn, engine_idling, siren; labels by class name."""
    if not p.audio:
        raise ConfigError("urbansound8k needs inputs.audio")
    rules = R.check(p, spec.get("labels"))
    k = int(spec.get("windows", 1))
    st, n = Store(p), 0
    for path in _paths(spec):
        t = Tree(path)
        meta = next((x for x in t.names() if x.endswith("metadata/UrbanSound8K.csv")), None)
        if not meta:
            raise ConfigError(f"{path}: metadata/UrbanSound8K.csv not found")
        audio = {"/".join(x.split("/")[-2:]): x for x in t.names() if "/audio/fold" in "/" + x}
        rows = list(csv.DictReader(io.StringIO(t.read(meta).decode())))
        log(f"{name}: {path} - {len(rows)} clips")
        for r in rows:
            key = f"fold{r['fold']}/{r['slice_file_name']}"
            labels = R.for_class(p, rules, r["class"])
            if all(v is None for v in labels.values()) or key not in audio:
                continue
            pcm = _audio_from_tree(t, audio[key], p.audio.sample_rate)
            gid = f"us8k-{r['fsID']}"                      # slices of one recording stay on one side of the split
            for wi, clip in enumerate(loud_windows(pcm, p.audio.clip, k)):
                st.add(f"{slug(name)}-{slug(Path(r['slice_file_name']).stem)}-w{wi}", pcm=clip, group=gid, source=name,
                       labels=labels)
            n += 1
            if limit and n >= limit:
                break
    st.flush()
    return n


def import_quiet(p: Project, name: str, spec: dict, limit: int | None) -> int:
    """Quiet-room clips (class "quiet"): low noise of varied level and colour, some with a faint mains hum.
    A real microphone never gives digital silence; this is what "nothing to hear" looks like to the model."""
    if not p.audio:
        raise ConfigError("quiet needs inputs.audio")
    rules = R.check(p, spec.get("labels"))
    labels = R.for_class(p, rules, "quiet")
    if all(v is None for v in labels.values()):
        return 0
    a, st = p.audio, Store(p)
    count = min(int(spec.get("count", 200)), limit or 10 ** 9)
    t = np.arange(a.clip) / a.sample_rate
    for i in range(count):
        rng = np.random.default_rng(zlib.crc32(f"{name}-quiet-{i}".encode()))
        x = rng.normal(0, 1, a.clip)
        k = rng.uniform(0.0, 0.97)                               # 0 = white, towards 1 = low rumble
        for j in range(1, a.clip):
            x[j] += k * x[j - 1]
        x *= 10 ** rng.uniform(0.5, 2.0) / (x.std() + 1e-9)      # RMS 3..100 of 32768
        if rng.random() < 0.3:
            x += rng.uniform(2, 30) * np.sin(2 * np.pi * rng.choice([50.0, 60.0, 100.0, 120.0]) * t + rng.uniform(0, 6.28))
        st.add(f"{slug(name)}-quiet-{i:04d}", pcm=np.clip(x, -32768, 32767).astype(np.int16), group=f"quiet-{i}",
               source=name, labels=labels)
    st.flush()
    return count


def import_video(p: Project, name: str, spec: dict, limit: int | None) -> int:
    """Paired samples from videos: frame t, frame t - motion_ms, and the clip_seconds of audio ending at t."""
    rules = R.check(p, spec["labels"]) if spec.get("labels") else None
    every = float(spec.get("every", 1.0))
    st, n = Store(p), 0
    files = []
    for path in _paths(spec):
        path = Path(path)
        files += [path] if path.is_file() else sorted(f for f in path.rglob("*") if f.suffix.lower() in VIDEO_EXT)
    for f in files:
        labels = R.for_class(p, rules, f.parent.name.lower()) if rules else {}
        frames, step_ms = (_video_frames(f, p) if p.image else (None, 1000))
        pcm = None
        if p.audio:
            try:
                pcm = decode_audio(f, p.audio.sample_rate)
            except subprocess.CalledProcessError:
                pcm = None
        dur_ms = len(frames) * step_ms if frames is not None else (len(pcm) * 1000 // p.audio.sample_rate if pcm is not None else 0)
        lag = p.image.motion_lag * p.image.frame_ms // step_ms if (p.image and p.image.motion) else 0
        start_ms = max(lag * step_ms, int(p.audio.clip_seconds * 1000) if p.audio else 0)
        made = 0
        for t_ms in range(start_ms, dur_ms, int(every * 1000)):
            img = prev = clip = None
            if frames is not None:
                k = t_ms // step_ms
                if k >= len(frames):
                    break
                img = frames[k]
                prev = frames[k - lag] if lag else None
            if pcm is not None:
                end = t_ms * p.audio.sample_rate // 1000
                clip = fit_len(pcm[max(0, end - p.audio.clip):end], p.audio.clip) if end >= p.audio.clip else None
                if clip is None:
                    continue
            gid = f"{slug(name)}-{slug(f.stem)}"
            st.add(f"{gid}-{t_ms:08d}", image=img, prev=prev, pcm=clip, group=f"{gid}-{t_ms // 10000}", source=name, labels=labels)
            made += 1
        log(f"{name}: {f.name} - {made} samples")
        n += made
        if limit and n >= limit:
            break
    st.flush()
    return n


def _video_frames(f: Path, p: Project):
    c = p.image
    step_ms = c.motion_ms if c.motion else 1000
    fmt = "rgb24" if c.colors == 3 else "gray"
    vf = f"fps=1000/{step_ms},crop='min(iw,ih)':'min(iw,ih)',scale={CAM}:{CAM}:flags=area,format={fmt}"
    out = subprocess.run([ffmpeg_exe(), "-v", "error", "-i", str(f), "-vf", vf, "-f", "rawvideo", "-"],
                         capture_output=True, check=True).stdout
    frame = CAM * CAM * c.colors
    arr = np.frombuffer(out[: len(out) // frame * frame], np.uint8).reshape(-1, CAM, CAM, c.colors)
    return arr, step_ms


# ---------------------------------------------------------------- helpers

def _paths(spec: dict) -> list[str]:
    ps = spec.get("paths") or ([spec["path"]] if spec.get("path") else [])
    if not ps:
        raise ConfigError("a source needs path: or paths:")
    return ps


def _center(im: Image.Image):
    w, h = im.size
    s = min(w, h)
    x0, y0 = (w - s) // 2, (h - s) // 2
    return im.crop((x0, y0, x0 + s, y0 + s)), (x0, y0, s)


def _audio_from_tree(t: Tree, name: str, sr: int) -> np.ndarray:
    local = t.local(name)
    if local is not None:
        return decode_audio(local, sr)
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d) / Path(name).name
        tmp.write_bytes(t.read(name))
        return decode_audio(tmp, sr)


def _import_starter(p: Project, name: str, spec: dict, limit: int | None) -> int:
    from .starter import import_starter
    return import_starter(p, name, spec, limit)


IMPORTERS = {"folders": import_folders, "voc": import_voc, "coco": import_coco, "yolo": import_yolo,
             "esc50": import_esc50, "urbansound8k": import_urbansound8k, "video": import_video,
             "quiet": import_quiet, "starter": _import_starter,
             "speech_commands": import_speech_commands}


def run(p: Project, names: list[str] | None = None, limit: int | None = None) -> None:
    if not p.sources:
        raise ConfigError("there are no sources: in decidio.yaml. Add a source (see GUIDE.md: Sources)")
    for name, spec in p.sources.items():
        if names and name not in names:
            continue
        typ = spec.get("type")
        if typ not in IMPORTERS:
            raise ConfigError(f"source {name}: type must be one of {list(IMPORTERS)}")
        spec = dict(spec)
        for k in ("path", "annotations", "images"):
            if spec.get(k) and not Path(spec[k]).is_absolute():
                spec[k] = str(p.root / spec[k])
        if spec.get("paths"):
            spec["paths"] = [x if Path(x).is_absolute() else str(p.root / x) for x in spec["paths"]]
        n = IMPORTERS[typ](p, name, spec, limit)
        log(f"{name}: imported {n} items")
