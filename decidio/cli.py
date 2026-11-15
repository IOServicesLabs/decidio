"""decidio command line.  `decidio <command> -h` for options; -C <project folder> (default: current folder)."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import __version__
from .config import ConfigError, load

TEMPLATES = Path(__file__).parent / "templates"


def _p(a):
    return load(a.project)


def cmd_new(a):
    dst = Path(a.name)
    if (dst / "decidio.yaml").exists():
        raise SystemExit(f"{dst} already contains a decidio.yaml. Use a different folder.")
    src = TEMPLATES / f"{a.template}.yaml"
    if not src.exists():
        raise SystemExit(f"there is no template {a.template!r}. Use one of these templates: "
                         f"{', '.join(t.stem for t in TEMPLATES.glob('*.yaml'))}")
    dst.mkdir(parents=True, exist_ok=True)
    text = src.read_text(encoding="utf-8")
    text = text.replace(f"name: {a.template}", f"name: {dst.name}", 1)
    (dst / "decidio.yaml").write_text(text, encoding="utf-8")
    for sub in ("data/samples", "downloads", "out"):
        (dst / sub).mkdir(parents=True, exist_ok=True)
    print(f"wrote {dst / 'decidio.yaml'} from the template {a.template}")


def cmd_templates(a):
    for t in sorted(TEMPLATES.glob("*.yaml")):
        first = next((l[2:].strip() for l in t.read_text(encoding="utf-8").splitlines() if l.startswith("# ")), "")
        print(f"  {t.stem:<18} {first}")


def cmd_info(a):
    from .store import Store
    p = _p(a)
    st = Store(p)
    metas, labels, teacher = st.metas(), st.labels(), st.teacher()
    n_img = sum(1 for s in metas if st.files(s)[0])
    n_wav = sum(1 for s in metas if st.files(s)[2])
    print(f"{p.name}: board {p.board}, inputs " + ", ".join(x for x, on in (("image", p.image), ("audio", p.audio)) if on))
    print(f"samples {len(metas)} (image {n_img}, audio {n_wav}), hand labels {len(labels)}, teacher labels {len(teacher)}")
    for q in p.questions:
        h = sum(1 for r in labels.values() if q.qid in r)
        t = sum(1 for r in teacher.values() if q.qid in r)
        print(f"  {q.qid:<20} {q.qtype:<6} {'/'.join(q.options):<40} labels {h}, teacher {t}")


def cmd_fetch(a):
    from .fetch import fetch
    for name in a.datasets:
        if name == "starter":                     # shared by every project: ~/.decidio/starter
            from .starter import fetch as fetch_starter
            fetch_starter()
        else:
            fetch(name, Path(a.project) / "downloads")


def cmd_bases(a):
    from .config import BASES, CONFIG_NAME
    found = sorted(d for d in BASES.iterdir() if (d / CONFIG_NAME).is_file()) if BASES.is_dir() else []
    for d in found:
        b = load(d)
        ins = " + ".join(x for x, on in (("camera", b.image), ("microphone", b.audio)) if on)
        ready = "" if (d / "out" / "model.keras").exists() else "   (not trained in this install. To train it, run  decidio run -C <a copy of the folder>)"
        print(f"{b.name}: {ins}, board {b.board}{ready}")
        for q in b.deployed:
            print(f"    {q.qid:<18} {q.qtype:<6} {' / '.join(q.options)}")
    if not found:
        print("this install has no base models")
    print("to use a base model, run  decidio new <folder> -t teach\n"
          "Then set  base: <name>  in the decidio.yaml of the new folder.")


def cmd_teach(a):
    from . import dataset, evaluate, export, teach
    p = _p(a)
    if not a.skip_prep:
        dataset.build(p)
    teach.run(p, a.epochs)
    ex = export.run(p)
    evaluate.run(p, ex["runner"])


def cmd_import(a):
    from .importers import run
    run(_p(a), a.sources or None, a.limit)


def cmd_import_files(a):
    """Shortcut: decidio import-files <folder> --question q  (folder/<option>/<files>)."""
    from .importers import import_folders
    p = _p(a)
    n = import_folders(p, a.name or Path(a.folder).name, {"path": a.folder, "question": a.question,
                                                         "windows": a.windows}, a.limit)
    print(f"imported {n} files")


def cmd_import_video(a):
    from .importers import import_video
    p = _p(a)
    spec = {"paths": a.files, "every": a.every}
    if a.question:
        q = p.qmap()[a.question]
        spec["labels"] = {a.question: {o: [o] for o in q.options}}
    print(f"imported {import_video(p, a.name or 'video', spec, a.limit)} samples")


def cmd_label(a):
    p = _p(a)
    if a.how == "ui":
        from .labelui import serve
        serve(p, a.port, open_browser=not a.no_browser)
    elif a.how == "clef":
        from .teachers import clef
        clef(p, a.model or "Cloudflare/clef-flash", a.batch, a.relabel)
    elif a.how == "zeroshot":
        from .teachers import zeroshot
        zeroshot(p, a.image_model, a.audio_model, a.batch, a.relabel)


def cmd_prep(a):
    from .dataset import build
    build(_p(a))


def cmd_train(a):
    from .train import run
    run(_p(a), a.epochs, init=a.init)


def cmd_eval(a):
    from .evaluate import run
    p = _p(a)
    runner = None
    if (p.out / "model_int8.tflite").exists():
        from .export import Int8Runner
        runner = Int8Runner((p.out / "model_int8.tflite").read_bytes(), p)
    run(p, runner)


def cmd_test(a):
    from .evaluate import test
    test(_p(a), a.with_project, moving=True if a.moving else (False if a.still else None))


def cmd_try(a):
    from .trial import run
    run(_p(a), a.files, a.sound, a.json)


def cmd_calibrate(a):
    import numpy as np
    from .model import keras
    from .train import load_dataset, recalibrate_bn
    p = _p(a)
    d = load_dataset(p)
    model = keras.models.load_model(p.out / "model.keras", compile=False)
    recalibrate_bn(p, model, d, np.where(~d["val"])[0], a.batches)
    model.save(p.out / "model.keras")


def cmd_export(a):
    from .export import arduino, run
    p = _p(a)
    if not a.only_arduino:
        run(p)
    if a.arduino:
        arduino(p, a.arduino)


def cmd_run(a):
    from . import dataset, evaluate, export, train
    p = _p(a)
    if not a.skip_prep:
        dataset.build(p)
    train.run(p, a.epochs, init=a.init)
    ex = export.run(p)
    evaluate.run(p, ex["runner"])


def cmd_collect(a):
    from .serialio import collect
    collect(_p(a), a.port, a.count, a.every, a.prefix)


def cmd_monitor(a):
    from .serialio import monitor
    monitor(a.port, a.log)


def cmd_flash(a):
    from .firmware import flash_firmware, flash_model
    p = None
    try:
        p = _p(a)
    except ConfigError:
        pass
    if a.what in ("firmware", "all"):
        flash_firmware(None, a.port, a.baud)
    if a.what in ("model", "all"):
        if not p:
            raise SystemExit("flash model needs a project. Run the command in a project folder, or add  -C <project folder>.")
        flash_model(p, a.port, a.baud)


def cmd_builds(a):
    from . import builds
    if a.make:
        from . import recipes
        recipes.build()
        builds.make(Path(a.out) if a.out else builds.OUT)
    else:
        builds.listing()


def cmd_ports(a):
    from .firmware import ports
    found = ports()
    if a.json:
        import json
        print(json.dumps(found))
        return
    if not found:
        print("no USB serial port found. Use a USB cable that carries data. "
              "On a XIAO board, hold the B (BOOT) button while you connect the cable.")
        return
    for f in found:
        print(f"{f['port']:<14} {f['usb']}  {f['what']}")
    print("The command  decidio install <idea> --port <port>  asks the board which camera and microphone it has.")


def cmd_install(a):
    from .builds import install
    install(a.idea, a.port, a.board, a.baud, firmware=not a.keep_firmware)


def cmd_wifi(a):
    from .serialio import wifi
    password = None
    if a.ssid and not a.forget:
        import getpass
        password = getpass.getpass(f"type the password for {a.ssid} (for an open network, push Enter. The password does not show): ")
    wifi(a.port, a.ssid, password, a.forget)


def cmd_build_firmware(a):
    from .firmware import build_runtime
    build_runtime(a.board, a.out)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="decidio", description="decidio makes small models that answer typed questions from the camera and the microphone of an ESP32-S3 board.")
    ap.add_argument("--version", action="version", version=f"decidio {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add(name, fn, help_):
        s = sub.add_parser(name, help=help_)
        s.add_argument("-C", "--project", default=".", help="project folder (default: the current folder)")
        s.set_defaults(fn=fn)
        return s

    s = add("new", cmd_new, "make a project folder from a template")
    s.add_argument("name")
    s.add_argument("--template", "-t", default="blank")
    add("templates", cmd_templates, "show the project templates")
    add("info", cmd_info, "show the number of samples and labels for each question")
    s = add("fetch", cmd_fetch, "download public datasets into <project>/downloads")
    s.add_argument("datasets", nargs="+", help="starter (for all projects, in ~/.decidio/starter) | voc2007 voc2012 esc50 urbansound8k coco128 coco-val2017")
    add("bases", cmd_bases, "show the trained base models that come with decidio and the questions that they answer")
    s = add("teach", cmd_teach, "teach new questions to a base model in approximately one minute: prep + train the heads + export + eval")
    s.add_argument("--epochs", type=int)
    s.add_argument("--skip-prep", action="store_true")
    s = add("import", cmd_import, "import the samples from the sources: in decidio.yaml")
    s.add_argument("sources", nargs="*")
    s.add_argument("--limit", type=int)
    s = add("import-files", cmd_import_files, "import the image or sound files in <folder>/<option>/ for one question")
    s.add_argument("folder")
    s.add_argument("--question", "-q", required=True)
    s.add_argument("--name")
    s.add_argument("--windows", type=int, default=3, help="number of audio windows from each long file")
    s.add_argument("--limit", type=int)
    s = add("import-video", cmd_import_video, "import frames and their sound from video files")
    s.add_argument("files", nargs="+")
    s.add_argument("--every", type=float, default=1.0, help="seconds between samples")
    s.add_argument("--question", "-q", help="label each sample with the name of its parent folder (the folder name is the option id)")
    s.add_argument("--name")
    s.add_argument("--limit", type=int)
    s = add("label", cmd_label, "label samples: ui | clef | zeroshot")
    s.add_argument("how", choices=["ui", "clef", "zeroshot"])
    s.add_argument("--port", type=int, default=8765)
    s.add_argument("--no-browser", action="store_true")
    s.add_argument("--model", help="clef: Cloudflare/clef-flash (default) or Cloudflare/clef")
    s.add_argument("--image-model", default="openai/clip-vit-base-patch32")
    s.add_argument("--audio-model", default="laion/clap-htsat-unfused")
    s.add_argument("--batch", type=int, default=8)
    s.add_argument("--relabel", action="store_true", help="also label the samples that the teacher labelled before")
    add("prep", cmd_prep, "make out/dataset.npz from the samples and the labels")
    s = add("train", cmd_train, "train the model")
    s.add_argument("--epochs", type=int)
    s.add_argument("--init", help="start from the weights of this trained project (fine-tune)")
    add("eval", cmd_eval, "measure the model on the validation samples and write out/report.md")
    s = add("export", cmd_export, "write the int8 model and the model package out/model.dcd")
    s.add_argument("--arduino", help="also write decidio_model.h and decidio_model.cpp into this Arduino sketch folder (the model is in the code)")
    s.add_argument("--only-arduino", action="store_true", help="do not export the model again. Only write the sketch files")
    s = add("calibrate", cmd_calibrate, "calculate the BatchNorm statistics of a trained model again (the train command also does this)")
    s.add_argument("--batches", type=int, default=150)
    s = add("test", cmd_test, "run the exported int8 model on the labelled samples of a different project")
    s.add_argument("--with", dest="with_project", required=True, help="project folder with samples and labels")
    s.add_argument("--moving", action="store_true", help="show the samples as if from a moving camera (simulated drone or car movement)")
    s.add_argument("--still", action="store_true", help="show the samples as if from a camera that does not move (default: the platform of the project)")
    s = add("try", cmd_try, "run the exported int8 model on this computer with photos or sounds (files or folders)")
    s.add_argument("files", nargs="+")
    s.add_argument("--sound", help="for models with sound: use this sound with each photo (default: a quiet room)")
    s.add_argument("--json", action="store_true", help="show all probabilities, one JSON line for each file")
    s = add("run", cmd_run, "prep + train + export + eval")
    s.add_argument("--epochs", type=int)
    s.add_argument("--init", help="start from the weights of this trained project (fine-tune)")
    s.add_argument("--skip-prep", action="store_true")
    s = add("collect", cmd_collect, "record samples from the board (runtime firmware) through USB")
    s.add_argument("--port", required=True)
    s.add_argument("--count", type=int, default=0)
    s.add_argument("--every", type=int, default=1000, help="milliseconds between samples")
    s.add_argument("--prefix", default="dev-")
    s = add("monitor", cmd_monitor, "show the decisions of the board")
    s.add_argument("--port", required=True)
    s.add_argument("--log", help="add the decisions to the end of this .jsonl file")
    s = add("flash", cmd_flash, "write the runtime firmware, the model, or both to the board (ESP-IDF is not necessary)")
    s.add_argument("what", choices=["firmware", "model", "all"])
    s.add_argument("--port", required=True)
    s.add_argument("--board", choices=["xiao_sense", "s3cam"], help="not necessary: the firmware is the same for all boards")
    s.add_argument("--baud", type=int, default=921600)
    s = add("builds", cmd_builds, "show the ready-made ideas (rules for the base models) and the boards that they operate on")
    s.add_argument("--make", action="store_true", help="write all builds into installer/builds for the browser installer")
    s.add_argument("--out", help="with --make: a different output folder")
    s = add("ports", cmd_ports, "show the USB serial ports and the probable board on each port")
    s.add_argument("--json", action="store_true", help="show one JSON list (for scripts and LLM agents)")
    s = add("install", cmd_install, "write the firmware and one ready-made idea to a board (to see the ideas, run: decidio builds)")
    s.add_argument("idea")
    s.add_argument("--port", required=True)
    s.add_argument("--board", choices=["xiao_sense", "s3cam"],
                   help="do not ask the board for its sensors. xiao_sense: the board has a microphone. s3cam: the board has no microphone")
    s.add_argument("--baud", type=int, default=921600)
    s.add_argument("--keep-firmware", action="store_true", help="only replace the model package and its rules")
    s = add("wifi", cmd_wifi, "connect the board to Wi-Fi through USB, or show its address")
    s.add_argument("ssid", nargs="?", help="network name (2.4 GHz). Without a name, the command shows the current state")
    s.add_argument("--port", required=True)
    s.add_argument("--forget", action="store_true", help="remove the stored network from the board")
    s = add("build-firmware", cmd_build_firmware, "build the runtime firmware again with ESP-IDF (only after you change the C code)")
    s.add_argument("--board", default="esp32s3", choices=["esp32s3"], help="one firmware for every supported board")
    s.add_argument("--out", help="folder for the .bin files (default: decidio/firmware_bins/esp32s3)")

    a = ap.parse_args(argv)
    try:
        a.fn(a)
    except ConfigError as e:
        raise SystemExit(f"decidio.yaml: {e}")
    except KeyboardInterrupt:
        sys.exit(130)
