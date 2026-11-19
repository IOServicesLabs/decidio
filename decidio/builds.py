"""Installable builds: one of the ideas in recipes.json on a shipped base model, ready to flash.

An idea build is the same firmware and the same int8 model as the base it uses; only the rules in the model package
differ. So a build is made in a moment and without TensorFlow:

    decidio builds                         list the ideas and the boards each runs on
    decidio install <idea> --port COM5     flash the firmware, ask the board what sensors it has, then send that
                                           idea's package on the base model that fits (--board skips the asking)
    decidio builds --make                  write every build under installer/builds/ for the browser installer
                                           (installer/index.html); a few tens of MB, not kept in the repository
"""
from __future__ import annotations

import hashlib
import json
import tempfile
from pathlib import Path

from . import __version__, dcd, recipes
from .firmware import ask_sensors, firmware_pairs, flash_firmware, flash_package, fw_dir, model_offset

PKG = Path(__file__).resolve().parent
OUT = PKG.parent / "installer" / "builds"
# The firmware is the same on every board; what differs is the base model a build carries. "Board" here is the kind of
# board: one with a microphone gets `general`, one without gets `general-cam` (smaller and faster: no sound branch).
BOARD_BASE = {"xiao_sense": "general", "s3cam": "general-cam", "mic_only": "general", "plain": "general-cam"}
BOARD_NAME = {"xiao_sense": "Camera and microphone (Seeed XIAO ESP32S3 Sense)",
              "s3cam": "Camera, no microphone (ESP32-S3-CAM, ESP32-S3-EYE pinout)",
              "mic_only": "Microphone, camera not used (Seeed XIAO ESP32S3 Sense)",
              "plain": "No camera, no microphone (other ESP32-S3 board, 8 MB flash or more)"}
# Kinds of board without a camera: what their package tells the firmware ("hw" in the header; firmware/main/model.c).
# The firmware then does not look for a camera. mic_only runs the sound questions of its base model. plain runs no
# model: its rules use the radio, the pins, add-on sensors and the other boards.
NO_CAMERA = {"mic_only": {"camera": False}, "plain": {"camera": False, "profile": "plain"}}


def _sound_questions(base: str) -> set[str]:
    h = json.loads((PKG / "base" / base / "out" / "model_header.json").read_text(encoding="utf-8"))
    return {q["id"] for q in h["questions"] if q.get("m") == "a"}


def have_firmware() -> bool:
    return (fw_dir() / "flash.json").exists()


def boards_for(recipe: dict) -> list[str]:
    """Kinds of board whose base model answers everything the recipe asks."""
    if not have_firmware():
        return []
    out = []
    for b, base in BOARD_BASE.items():
        if b in NO_CAMERA:
            sound = _sound_questions(base) if b == "mic_only" and base in recipe.get("models", []) else set()
            if recipe["status"] != "teach" and all(n == "radio" or (n.startswith("in:") and not n.startswith("in:eye_")) or n in sound
                                                   for n in recipe.get("needs", [])):
                out.append(b)
        elif base in recipe.get("models", []):
            out.append(b)
    return out


def board_from_sensors(sensors: dict | None) -> str | None:
    """Which kind of board answered; None when it did not answer."""
    if sensors is None:
        return None
    return "xiao_sense" if sensors.get("mic") else "s3cam"


def package_for(recipe: dict, board: str) -> bytes:
    """The board's base model package with the recipe's rules in place of the base's own."""
    if recipe["status"] == "teach":
        raise SystemExit(f"{recipe['id']} needs a question that the base models do not answer. "
                         f"You must teach this idea. You cannot install it.\n"
                         f"  record: {recipe['record']}\n  then: decidio new my-idea -t teach  (GUIDE.md section 2)")
    if board not in boards_for(recipe):
        why = "needs the microphone" if recipe.get("mic") else "needs a question that the base model of that board does not answer"
        raise SystemExit(f"{recipe['id']} {why}. Boards that it operates on: {', '.join(boards_for(recipe)) or 'none'}")
    out = PKG / "base" / BOARD_BASE[board] / "out"
    h = json.loads((out / "model_header.json").read_text(encoding="utf-8"))
    h["actions"] = recipe["rules"]
    h["recipe"] = recipe["id"]
    if board in NO_CAMERA:
        h["hw"] = NO_CAMERA[board]
    return dcd.package(h, (out / "model_int8.tflite").read_bytes())


def firmware_image(board: str | None = None) -> bytes:
    """Bootloader, partition table, boot selector and app as one image for offset 0 (gaps are 0xFF, as erased flash)."""
    img = bytearray()
    for off, path in firmware_pairs(board):
        data = Path(path).read_bytes()
        at = int(off, 16)
        if at < len(img):
            raise SystemExit(f"{path} overlaps the file before it")
        img += b"\xff" * (at - len(img)) + data
    return bytes(img)


def listing(log=print) -> None:
    cat = recipes.load()
    boards = list(BOARD_BASE) if have_firmware() else []
    log(f"{'idea':<28} {'#':>3}  boards               what it does")
    for e in sorted(cat, key=lambda e: (e["status"] == "teach", e["idea"])):
        if e["status"] == "teach":
            where = "(teach it first)"
        else:
            where = ", ".join(b for b in boards if b in boards_for(e))
        log(f"{e['id']:<28} {e['idea']:>3}  {where:<20} {e['title']}")
    n = sum(e["status"] != "teach" for e in cat)
    log(f"\n{n} ideas install as they are: decidio install <idea> --port COMx   ({len(cat) - n} more need a taught question)")


def install(rid: str, port: str, board: str | None = None, baud: int = 921600, firmware: bool = True, log=print) -> None:
    e = recipes.find(rid)
    if e["status"] == "teach":
        package_for(e, board or "xiao_sense")                        # explains what to record, then exits
    fit = boards_for(e)
    if board is None:
        if firmware:
            flash_firmware(None, port, baud)
            firmware = False
            log("\nthe firmware is on the board. Now decidio asks the board for its sensors ...")
        sensors = ask_sensors(port)
        board = board_from_sensors(sensors)
        if board is None:
            board = fit[0] if fit else "xiao_sense"
            log(f"the board did not answer on {port}. decidio sends the build for '{BOARD_NAME[board]}'.\n"
                "  This build also operates on a board without a microphone. On that board, the sound questions are off.")
        else:
            log(f"the board reports: {sensors.get('profile')}, camera {sensors.get('camera') or 'none'}, "
                f"microphone {'yes' if sensors.get('mic') else 'none'}, memory card {'yes' if sensors.get('card') else 'none'}")
            if not sensors.get("camera") and e["status"] != "radio":
                log("  the board found no camera. Examine the ribbon cable of the camera. Without a camera, only radio ideas operate.")
        if board not in fit and not e.get("mic") and fit:
            board = fit[0]                                           # the other base answers it too: the board feeds it silence
    blob = package_for(e, board)
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / f"{rid}.dcd"
        f.write_bytes(blob)
        flash_package(board, port, f, baud, with_firmware=firmware)
    log(f"\ninstalled '{e['title']}' with the base model {BOARD_BASE[board]}.\n  {e['setup']}")
    if e.get("alerts"):
        log("  For alerts, connect the board to Wi-Fi with  decidio wifi --port %s  or with its own network 'decidio-xxxx'.\n"
            "  Then open the page of the board and type the alert settings on the Alerts tab." % port)


def make(out: Path = OUT, log=print) -> dict:
    """Write firmware.bin (one, for every board), one package and one ESP Web Tools manifest per idea and kind of
    board, and index.json."""
    out = Path(out)
    cat = [e for e in recipes.load() if e["status"] != "teach"]
    index = {"version": __version__, "firmware": "firmware.bin", "boards": {}, "ideas": []}
    total = 0
    if have_firmware():
        out.mkdir(parents=True, exist_ok=True)
        fw = firmware_image()
        (out / "firmware.bin").write_bytes(fw)
        total += len(fw)
    for board in (BOARD_BASE if have_firmware() else []):
        d = out / board
        d.mkdir(parents=True, exist_ok=True)
        index["boards"][board] = {"name": BOARD_NAME[board], "base": BOARD_BASE[board], "firmware": "firmware.bin",
                                  "model_offset": model_offset(board)}
        for e in cat:
            if board not in boards_for(e):
                continue
            blob = package_for(e, board)
            (d / f"{e['id']}.dcd").write_bytes(blob)
            total += len(blob)
            manifest = {"name": f"decidio: {e['title']}", "version": __version__, "new_install_prompt_erase": False,
                        "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": "../firmware.bin", "offset": 0},
                                                                         {"path": f"{e['id']}.dcd", "offset": model_offset(board)}]}]}
            (d / f"{e['id']}.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8", newline="\n")
    for e in cat:
        fit = [b for b in index["boards"] if b in boards_for(e)]
        if not fit:
            continue
        index["ideas"].append({
            "id": e["id"], "idea": e["idea"], "title": e["title"], "setup": e["setup"], "needs": e["needs"],
            "mic": e["mic"], "radio": e["status"] == "radio", "photo": e["photo"], "alerts": e["alerts"],
            "boards": {b: {"manifest": f"{b}/{e['id']}.json", "package": f"{b}/{e['id']}.dcd",
                           "sha256": hashlib.sha256((out / b / f"{e['id']}.dcd").read_bytes()).hexdigest()} for b in fit}})
    (out / "index.json").write_text(json.dumps(index, indent=1), encoding="utf-8", newline="\n")
    log(f"{len(index['ideas'])} ideas for {', '.join(index['boards'])} -> {out} ({total / 1e6:.0f} MB)")
    return index
