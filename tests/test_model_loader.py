"""The firmware's model loader (firmware/main/model.c, compiled for the host with the IDF's cJSON) must read
exactly what `decidio export` wrote: header fields, questions, actions, and the tflite bytes; and reject corruption."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(project: str, cjson_dir: str):
    out = Path(project) / "out"
    blob, hdr, tfl = out / "model.dcd", json.loads((out / "model_header.json").read_text()), (out / "model_int8.tflite").read_bytes()
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "loader"
        subprocess.run(["gcc", "-O1", "-std=gnu11", "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "firmware/main"),
                        "-I", cjson_dir, str(ROOT / "tests/host/model_main.c"), str(ROOT / "firmware/main/model.c"),
                        str(ROOT / "firmware/main/rules.c"),
                        str(Path(cjson_dir) / "cJSON.c"), "-lm", "-o", str(exe)], check=True)
        got = json.loads(subprocess.run([str(exe), str(blob)], capture_output=True, check=True, text=True).stdout)
        bad = json.loads(subprocess.run([str(exe), str(blob), "100"], capture_output=True, check=True, text=True).stdout)
    assert got["err"] == 0, got
    assert got["name"] == hdr["name"][:31]
    if hdr["image"]:
        im = hdr["image"]
        assert got["img"] == [1, im["size"], im["colors"], int(im["motion"]), im["lag"], int(im.get("ego", 0))], got["img"]
    if hdr["audio"]:
        a = hdr["audio"]
        assert got["aud"][:7] == [1, a["sr"], a["clip"], a["win"], a["hop"], a["nfft"], a["n_mels"]], got["aud"]
        assert abs(got["aud"][9] - a["mean"]) < 1e-4 and abs(got["aud"][10] - a["std"]) < 1e-4
    assert got["n_logits"] == hdr["n_logits"] and got["arena_kb"] == hdr["arena_kb"]
    assert [q[0] for q in got["q"]] == [q["id"] for q in hdr["questions"]]
    assert [q[4] for q in got["q"]] == [q["options"] for q in hdr["questions"]]
    assert len(got["act"]) == len(hdr["actions"]) and [a[0] for a in got["act"]] == [a["name"] for a in hdr["actions"]]
    raw = blob.read_bytes()
    assert raw[got["tfl_off"]:got["tfl_off"] + got["tfl_len"]] == tfl and got["tfl_off"] % 16 == 0
    assert bad["err"] != 0, "corrupted package must be rejected"
    print(f"loader OK: {hdr['name']} - {len(got['q'])} questions, {len(got['act'])} actions, tflite {got['tfl_len']} B at +{got['tfl_off']}; corruption rejected")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.environ.get("CJSON_DIR", "cJSON"))      # cJSON's source: <esp-idf>/components/json/cJSON
