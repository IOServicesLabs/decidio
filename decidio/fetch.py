"""decidio fetch <dataset>: public datasets into <project>/downloads."""
from __future__ import annotations

import shutil
import tarfile
import urllib.request
import zipfile
from pathlib import Path

GH = "https://github.com/ultralytics/assets/releases/download/v0.0.0/"
HF_WEATHER = "https://huggingface.co/datasets/prithivMLmods/WeatherNet-05-18039/resolve/refs%2Fconvert%2Fparquet/default/train/"
DATASETS = {
    "voc2007": {"files": [GH + "VOCtrainval_06-Nov-2007.zip", GH + "VOCtest_06-Nov-2007.zip"],
                "about": "Pascal VOC 2007, approximately 10k photos, 20 classes with person, car, bus, motorbike, bicycle, dog, cat (approximately 880 MB)"},
    "voc2012": {"files": [GH + "VOCtrainval_11-May-2012.zip"],
                "about": "Pascal VOC 2012, approximately 17k photos, the same 20 classes (approximately 1.9 GB)"},
    "coco128": {"files": [GH + "coco128.zip"], "about": "128 COCO photos in YOLO format (small, to try the yolo importer)"},
    "esc50": {"files": ["https://github.com/karolpiczak/ESC-50/archive/refs/heads/master.zip"], "unzip": "ESC-50",
              "about": "ESC-50, 2000 five-second environmental sounds, 50 classes (siren, car_horn, engine, dog, ...) (approximately 600 MB)"},
    "urbansound8k": {"files": ["https://zenodo.org/record/1203745/files/UrbanSound8K.tar.gz?download=1"],
                     "untar": "UrbanSound8K",
                     "about": "UrbanSound8K, 8732 street sounds <= 4 s, 10 classes with car_horn, engine_idling, siren (approximately 6 GB)"},
    "coco-val2017": {"files": ["http://images.cocodataset.org/zips/val2017.zip",
                               "http://images.cocodataset.org/annotations/annotations_trainval2017.zip"],
                     "about": "COCO 2017 val: 5k photos, 80 classes and annotations (approximately 1 GB)"},
    "coco-train2017": {"files": ["http://images.cocodataset.org/zips/train2017.zip",
                                 "http://images.cocodataset.org/annotations/annotations_trainval2017.zip"],
                       "about": "COCO 2017 train: 118k photos, 80 classes and annotations (approximately 19 GB. The base models use these photos)"},
    "weather": {"files": [(HF_WEATHER + "0000.parquet", "weathernet-0000.parquet"),
                          (HF_WEATHER + "0001.parquet", "weathernet-0001.parquet")],
                "parquet": "weather",
                "about": "WeatherNet-05, 18k weather photos in 5 classes, unpacked to weather/train and weather/test (each "
                         "10th photo) (approximately 550 MB. Needs pip install pyarrow)"},
    "speech-commands": {"files": ["http://download.tensorflow.org/data/speech_commands_v0.02.tar.gz"],
                        "untar": "speech_commands", "into": True,
                        "about": "Speech Commands v0.02, 105k one-second spoken words, 35 words (approximately 2.4 GB)"},
    "librispeech-dev": {"files": ["https://www.openslr.org/resources/12/dev-clean.tar.gz"], "untar": "LibriSpeech/dev-clean",
                        "about": "LibriSpeech dev-clean, 5 h of read speech, 40 speakers (approximately 340 MB)"},
    "librispeech-test": {"files": ["https://www.openslr.org/resources/12/test-clean.tar.gz"], "untar": "LibriSpeech/test-clean",
                         "about": "LibriSpeech test-clean, 5 h of read speech, 40 other speakers (approximately 350 MB)"},
}


def _unpack_parquet(files: list[Path], target: Path) -> None:
    """Hugging Face image parquet (image: {bytes}, label: class index) -> target/train|test/<class>/<n>.jpg.
    Every 10th photo goes to test; exact duplicates are dropped, so no photo is on both sides."""
    try:
        import pyarrow.parquet as pq
    except ImportError:
        raise SystemExit("this dataset is in parquet files. Install pyarrow (pip install pyarrow). Then do the fetch again")
    import hashlib
    import json
    n, seen = 0, set()
    for f in files:
        pf = pq.ParquetFile(f)
        meta = json.loads((pf.schema_arrow.metadata or {}).get(b"huggingface", b"{}"))
        names = meta.get("info", {}).get("features", {}).get("label", {}).get("names")
        for batch in pf.iter_batches(batch_size=256):
            for row in batch.to_pylist():
                digest = hashlib.md5(row["image"]["bytes"]).digest()
                if digest in seen:                 # the same photo twice (scraped sets have some): keep the first
                    continue
                seen.add(digest)
                lab = row["label"]
                cls = names[lab] if names and isinstance(lab, int) else str(lab)
                cls = "".join(c if c.isalnum() else "_" for c in cls.lower()).strip("_")
                out = target / ("test" if n % 10 == 9 else "train") / cls / f"{n:06d}.jpg"
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(row["image"]["bytes"])
                n += 1
    print(f"unpacked {n} photos to {target}")


def fetch(name: str, dst: Path) -> None:
    if name not in DATASETS:
        raise SystemExit(f"unknown dataset {name!r}. Use one of these datasets: "
                         + ", ".join(f"{k} ({v['about']})" for k, v in DATASETS.items()))
    dst.mkdir(parents=True, exist_ok=True)
    d = DATASETS[name]
    outs = []
    for url in d["files"]:
        url, fname = url if isinstance(url, tuple) else (url, url.rsplit("/", 1)[-1].split("?")[0])
        out = dst / fname
        outs.append(out)
        if out.exists() and out.stat().st_size > 0:
            print(f"already downloaded: {out}")
        else:
            print(f"downloading {url}")
            tmp = out.with_suffix(out.suffix + ".part")
            with urllib.request.urlopen(url, timeout=60) as r, tmp.open("wb") as f:
                total = int(r.headers.get("Content-Length", 0))
                done = 0
                while chunk := r.read(1 << 20):
                    f.write(chunk)
                    done += len(chunk)
                    if total:
                        print(f"\r  {done / 1e6:.0f} / {total / 1e6:.0f} MB", end="", flush=True)
            print()
            tmp.replace(out)
        if d.get("unzip"):
            target = dst / d["unzip"]
            if not target.exists():
                with zipfile.ZipFile(out) as z:
                    z.extractall(dst)
                    top = dst / z.namelist()[0].split("/")[0]
                if top != target:
                    shutil.move(str(top), str(target))
                print(f"unpacked to {target}")
        if d.get("untar"):
            target = dst / d["untar"]
            if not target.exists():
                print(f"unpacking {out.name} ...")
                where = target if d.get("into") else dst       # into: the archive has no top folder of its own
                where.mkdir(parents=True, exist_ok=True)
                with tarfile.open(out) as tf:
                    try:
                        tf.extractall(where, filter="data")
                    except TypeError:          # Python without tar extraction filters
                        tf.extractall(where)
                print(f"unpacked to {target}")
    if d.get("parquet") and not (dst / d["parquet"]).exists():
        _unpack_parquet(outs, dst / d["parquet"])
    print(f"{name}: ready in {dst}")
