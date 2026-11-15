"""The idea catalog: recipes/*.yaml resolve against the shipped base models, the generated catalogs are current,
USE_CASES.md and the catalog agree, and every installable idea packages into a model the loader format accepts."""
import json
import re
from pathlib import Path

import pytest

from decidio import boardrules, builds, dcd, recipes

ROOT = Path(__file__).resolve().parent.parent
CAT = recipes.check(recipes.source(), recipes.base_headers())


def rows():
    """(number, needs, install column) for every idea row of USE_CASES.md"""
    out = []
    for line in (ROOT / "USE_CASES.md").read_text(encoding="utf-8").splitlines():
        m = re.match(r"^\| (\d+) \|(.*)\|(.*)\|(.*)\|$", line)
        if m:
            out.append((int(m.group(1)), m.group(3).strip(), m.group(4).strip()))
    return out


def test_generated_catalogs_are_current():
    assert json.loads(recipes.CATALOG.read_text(encoding="utf-8")) == CAT, "run: python -m decidio.recipes"
    fw = json.loads(recipes.FW_CATALOG.read_text(encoding="utf-8"))
    assert [e["id"] for e in fw] == [e["id"] for e in CAT if e["status"] != "teach"]
    assert all(a["rules"] == b["rules"] for a, b in zip(fw, (e for e in CAT if e["status"] != "teach")))
    assert recipes.FW_CATALOG.stat().st_size < 48 * 1024           # it is compiled into the firmware


def test_use_cases_and_catalog_agree():
    table = rows()
    assert [n for n, _, _ in table] == list(range(1, 112))
    by = {}
    for e in CAT:
        by.setdefault(e["idea"], []).append(e)
    for n, needs, install in table:
        ids = re.findall(r"`([a-z0-9-]+)`", install)
        assert ids == [e["id"] for e in by.get(n, [])], f"idea {n}: the Install column and recipes/ differ"
        kinds = {e["status"] for e in by.get(n, [])}
        if needs.startswith("base"):
            assert kinds == {"ready"}, f"idea {n} is answered by a base model but has no ready recipe"
        elif needs.startswith("teach"):
            assert kinds == {"teach"} and "(teach)" in install, f"idea {n} needs a teach stub"
        elif needs.startswith("eye"):
            assert kinds == {"ready"} and all("in:eye_" in " ".join(e["needs"]) for e in by[n]), f"idea {n} needs an eye recipe"
        elif kinds:
            assert needs.startswith("radio") and kinds == {"radio"}, f"idea {n}: unexpected recipe"
    assert sum(e["status"] == "ready" for e in CAT) >= 54 and sum(e["status"] == "teach" for e in CAT) == 32


def test_every_rule_fits_the_board():
    for e in CAT:
        if e["status"] == "teach":
            assert e["kind"] in ("image", "sound") and e["record"] and ":" in e["question"]
            continue
        assert 1 <= len(e["rules"]) <= 8 and e["setup"], e["id"]            # the board holds 16: room for two ideas
        for r in e["rules"]:
            assert len(r["name"]) <= boardrules.MAX_NAME and len(r["text"]) <= boardrules.MAX_TEXT, e["id"]
            assert 1 <= len(r["when"]) <= boardrules.MAX_COND and r["gpio"] == -1, e["id"]       # pins are the user's choice
            assert not r["notify"] or r["text"], f"{e['id']}/{r['name']}: an alert needs its words"
            assert not r["photo"] or r["notify"], e["id"]
        assert e["mic"] == ("general-cam" not in e["models"]), e["id"]
    photos = [e["id"] for e in CAT if e.get("photo")]
    assert photos == ["trail-camera"], f"photos must stay off by default: {photos}"


@pytest.mark.parametrize("board", list(builds.BOARD_BASE))
def test_every_idea_packages(board):
    base = json.loads((builds.PKG / "base" / builds.BOARD_BASE[board] / "out" / "model_header.json").read_text())
    tfl = (builds.PKG / "base" / builds.BOARD_BASE[board] / "out" / "model_int8.tflite").read_bytes()
    n = 0
    for e in CAT:
        if e["status"] == "teach" or builds.BOARD_BASE[board] not in e["models"]:
            continue
        h, model = dcd.unpack(builds.package_for(e, board))
        assert model == tfl and h["actions"] == e["rules"] and h["questions"] == base["questions"]
        ids = {q["id"] for q in h["questions"]}
        assert all(c["q"] in ids or c["q"] == "radio_motion" or c["q"][3:] in boardrules.INPUTS for r in h["actions"] for c in r["when"])
        n += 1
    assert n >= (57 if board == "xiao_sense" else 30)
    teach = next(e for e in CAT if e["status"] == "teach")
    with pytest.raises(SystemExit, match="record"):
        builds.package_for(teach, board)


def test_package_roundtrip_and_corruption():
    blob = dcd.package({"format": 1, "name": "x", "actions": []}, b"\x01\x02\x03" * 11)
    assert blob[:4] == dcd.MAGIC and dcd.unpack(blob) == ({"format": 1, "name": "x", "actions": []}, b"\x01\x02\x03" * 11)
    bad = bytearray(blob)
    bad[20] ^= 0xFF
    for wrong in (bytes(bad), blob[:-1], b"nope" + blob[4:]):
        with pytest.raises(ValueError):
            dcd.unpack(wrong)
