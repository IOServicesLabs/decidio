"""The starter set: a small, mixed pile of public photos and sounds that every decidio project can share.

  photos   COCO val2017 photos chosen to show all 80 COCO object classes, with their boxes    (images.cocodataset.org)
  sounds   one fold of ESC-50: 8 five-second clips for each of its 50 sound classes           (github.com/karolpiczak/ESC-50)
  quiet    generated quiet-room clips (class "quiet"), so a sound question can also say "nothing"

decidio ships only the two lists (decidio/starter_data/); `decidio fetch starter` downloads the files from their
own hosts, once per PC, into ~/.decidio/starter (set DECIDIO_HOME to move it). A project uses it as a source:

  sources:
    starter:
      type: starter
      labels:
        person_present: {true: [person]}            # image questions read the COCO classes
        sound: {dog: [dog], other: "*"}             # audio questions read the ESC-50 classes (+ quiet)
        my_cat: {false: "*"}                        # a new thing: none of the starter shows it

The base models never saw any of it in training, so it also serves as an honest test.
"""
from __future__ import annotations

import csv
import gzip
import io
import json
import os
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from .config import ConfigError, Project

DATA = Path(__file__).parent / "starter_data"
COCO_LIST, ESC_LIST = DATA / "coco_starter.json.gz", DATA / "esc50_starter.csv"
ESC_URL = "https://raw.githubusercontent.com/karolpiczak/ESC-50/master/audio/"
QUIET = 60                   # generated quiet-room clips


def home() -> Path:
    return Path(os.environ.get("DECIDIO_HOME") or Path.home() / ".decidio") / "starter"


def lists() -> tuple[dict, list[dict]]:
    if not COCO_LIST.exists() or not ESC_LIST.exists():
        raise ConfigError(f"the starter lists are missing from {DATA}")
    coco = json.loads(gzip.decompress(COCO_LIST.read_bytes()).decode())
    esc = list(csv.DictReader(io.StringIO(ESC_LIST.read_text(encoding="utf-8"))))
    return coco, esc


def _get(job) -> int:
    url, out = job
    if out.exists() and out.stat().st_size > 0:
        return 0
    tmp = out.with_suffix(out.suffix + ".part")
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "decidio"})
            data = urllib.request.urlopen(req, timeout=60).read()
            tmp.write_bytes(data)
            tmp.replace(out)
            return len(data)
        except Exception:
            if attempt == 3:
                return -1
            time.sleep(1 + attempt)
    return -1


def missing() -> list[tuple[str, Path]]:
    coco, esc = lists()
    d = home()
    jobs = [(im["coco_url"], d / "coco" / "images" / im["file_name"]) for im in coco["images"]]
    jobs += [(ESC_URL + r["filename"], d / "esc50" / "audio" / r["filename"]) for r in esc]
    return [(u, f) for u, f in jobs if not (f.exists() and f.stat().st_size > 0)]


def fetch(log=print) -> Path:
    """Download whatever is not there yet (safe to stop and run again)."""
    coco, esc = lists()
    d = home()
    for sub in ("coco/images", "esc50/audio", "esc50/meta"):
        (d / sub).mkdir(parents=True, exist_ok=True)
    (d / "coco" / "instances_starter.json").write_text(json.dumps(coco), encoding="utf-8")
    (d / "esc50" / "meta" / "esc50.csv").write_text(ESC_LIST.read_text(encoding="utf-8"), encoding="utf-8")
    jobs = missing()
    total = len(coco["images"]) + len(esc)
    if not jobs:
        log(f"starter: ready in {d} ({len(coco['images'])} photos, {len(esc)} sound clips)")
        return d
    log(f"starter: downloading {len(jobs)} of {total} files into {d} (approximately "
        f"{coco['info'].get('megabytes', 160)} MB of photos and {len(esc) * 0.44:.0f} MB of sounds)")
    done = failed = size = 0
    with ThreadPoolExecutor(16) as ex:
        for n in ex.map(_get, jobs):
            done += 1
            failed += n < 0
            size += max(n, 0)
            if done % 25 == 0 or done == len(jobs):
                print(f"\r  {done} / {len(jobs)} files, {size / 1e6:.0f} MB", end="", flush=True)
    print()
    if failed:
        raise SystemExit(f"starter: {failed} files did not download. "
                         "Run  decidio fetch starter  again to get the remaining files")
    log(f"starter: ready in {d}")
    return d


def import_starter(p: Project, name: str, spec: dict, limit: int | None) -> int:
    from . import importers as I
    if missing():
        fetch(I.log)
    d = home()
    rules = spec.get("labels") or {}
    qm = p.qmap()
    for qid in rules:
        if qid not in qm:
            raise ConfigError(f"labels: unknown question {qid!r}")
    img = {k: v for k, v in rules.items() if qm[k].modality != "audio"}      # photos answer the image questions,
    aud = {k: v for k, v in rules.items() if qm[k].modality != "image"}      # sounds the audio questions
    n = 0
    if p.image and img:
        n += I.import_coco(p, name, {"annotations": str(d / "coco" / "instances_starter.json"),
                                     "images": str(d / "coco" / "images"), "labels": img,
                                     "crops": spec.get("crops", 2), "min_size": spec.get("min_size", 0.12)}, limit)
    if p.audio and aud:
        n += I.import_esc50(p, name, {"path": str(d / "esc50"), "labels": aud, "windows": spec.get("windows", 2)}, limit)
        n += I.import_quiet(p, name, {"count": spec.get("quiet", QUIET), "labels": aud}, limit)
    return n
