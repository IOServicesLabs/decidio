"""Rebuild the starter set's two lists (decidio/starter_data/) from the public datasets. Maintainers only.

  python tools/make_starter.py --coco-annotations annotations_trainval2017.zip --coco-images val2017.zip --esc50 ESC-50-master.zip

Photos: COCO val2017, picked so that every one of the 80 classes is clearly shown in at least `--per-class` photos,
then filled up at random (which keeps everyday clutter and a few photos with no labelled object at all).
Sounds: fold 1 of ESC-50 (8 clips of each of the 50 classes).
The base models are trained on COCO train2017 and ESC-50 folds 2-5, so they have seen none of this.
"""
from __future__ import annotations

import argparse
import csv
import gzip
import io
import json
import zipfile
from pathlib import Path

import numpy as np

OUT = Path(__file__).resolve().parent.parent / "decidio" / "starter_data"


def member(z: zipfile.ZipFile, tail: str) -> str:
    return next(n for n in z.namelist() if n.endswith(tail))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--coco-annotations", required=True)
    ap.add_argument("--coco-images", required=True, help="val2017.zip (only its file sizes are read)")
    ap.add_argument("--esc50", required=True)
    ap.add_argument("--photos", type=int, default=1000)
    ap.add_argument("--per-class", type=int, default=12)
    ap.add_argument("--fold", default="1")
    a = ap.parse_args()
    rng = np.random.default_rng(0)

    with zipfile.ZipFile(a.coco_annotations) as z:
        d = json.loads(z.read(member(z, "instances_val2017.json")))
    with zipfile.ZipFile(a.coco_images) as z:
        sizes = {Path(i.filename).name: i.file_size for i in z.infolist()}
    ims = {im["id"]: im for im in d["images"]}
    anns: dict[int, list] = {}
    for an in d["annotations"]:
        anns.setdefault(an["image_id"], []).append(an)
    shows: dict[int, set] = {i: set() for i in ims}          # classes an image shows clearly
    for i, lst in anns.items():
        side = min(ims[i]["width"], ims[i]["height"])
        for an in lst:
            if not an.get("iscrowd") and max(an["bbox"][2], an["bbox"][3]) >= 0.2 * side:
                shows[i].add(an["category_id"])
    order = [int(i) for i in rng.permutation(sorted(ims))]
    picked: list[int] = []
    count = {c["id"]: 0 for c in d["categories"]}
    while True:                                              # always serve the class that has the fewest photos so far
        short = sorted((n, c) for c, n in count.items() if n < a.per_class)
        got = False
        for _, c in short:
            i = next((i for i in order if i not in picked and c in shows[i]), None)
            if i is not None:
                picked.append(i)
                for k in shows[i]:
                    count[k] += 1
                got = True
                break
            count[c] = 10 ** 6 + count[c]                    # no more photos of it
        if not got:
            break
    for i in order:
        if len(picked) >= a.photos:
            break
        if i not in picked:
            picked.append(i)
    picked = sorted(picked[: max(a.photos, 0)])
    keep = set(picked)
    out = {
        "info": {"source": "COCO 2017 val (cocodataset.org); annotations CC BY 4.0, photos under their Flickr licences",
                 "megabytes": round(sum(sizes[ims[i]["file_name"]] for i in picked) / 1e6)},
        "categories": [{"id": c["id"], "name": c["name"]} for c in d["categories"]],
        "images": [{"id": i, "file_name": ims[i]["file_name"], "width": ims[i]["width"], "height": ims[i]["height"],
                    "coco_url": ims[i]["coco_url"]} for i in picked],
        "annotations": [{"image_id": an["image_id"], "category_id": an["category_id"],
                         "bbox": [round(v, 1) for v in an["bbox"]], "iscrowd": int(an.get("iscrowd", 0))}
                        for i in picked for an in anns.get(i, [])],
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "coco_starter.json.gz").write_bytes(gzip.compress(json.dumps(out, separators=(",", ":")).encode(), mtime=0))
    names = {c["id"]: c["name"] for c in d["categories"]}
    per = {names[c]: sum(1 for i in picked if c in shows[i]) for c in names}
    print(f"photos {len(picked)}, {out['info']['megabytes']} MB, {len(out['annotations'])} boxes; "
          f"no clear object in {sum(1 for i in picked if not shows[i])}; fewest: "
          + ", ".join(f"{k} {v}" for k, v in sorted(per.items(), key=lambda kv: kv[1])[:6]))

    with zipfile.ZipFile(a.esc50) as z:
        rows = list(csv.DictReader(io.StringIO(z.read(member(z, "meta/esc50.csv")).decode())))
        audio = {Path(n).name: z.getinfo(n).file_size for n in z.namelist() if n.endswith(".wav")}
    rows = [r for r in rows if r["fold"] == a.fold]
    buf = io.StringIO()
    w = csv.DictWriter(buf, list(rows[0].keys()), lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    (OUT / "esc50_starter.csv").write_text(buf.getvalue(), encoding="utf-8")
    print(f"sounds {len(rows)}, {sum(audio[r['filename']] for r in rows) / 1e6:.0f} MB, "
          f"{len({r['category'] for r in rows})} classes (ESC-50 fold {a.fold}, CC BY-NC)")
    _ = keep


if __name__ == "__main__":
    main()
