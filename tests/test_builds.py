"""The shipped firmware files and the build generator: offsets match the partition table, the firmware image is laid
out as the board expects, and every generated build is a valid package with a manifest that points at it."""
import json
from pathlib import Path

import pytest

from decidio import builds, dcd, firmware

ROOT = Path(__file__).resolve().parent.parent
BOARDS = list(builds.BOARD_BASE) if builds.have_firmware() else []
FW = [firmware.FW] if builds.have_firmware() else []


def table():
    out = {}
    for line in (ROOT / "firmware" / "partitions.csv").read_text().splitlines():
        f = [x.strip() for x in line.split(",")]
        if len(f) >= 5 and not f[0].startswith("#"):
            out[f[0]] = (int(f[3], 16), int(f[4], 16))
    return out


def test_partition_table_layout():
    t = table()
    spans = sorted(t.values())
    assert all(a + n <= b for (a, n), (b, _) in zip(spans, spans[1:])), "partitions overlap"
    assert spans[-1][0] + spans[-1][1] <= 8 * 1024 * 1024, "must fit the XIAO's 8 MB flash"
    assert t["model"] == (firmware.MODEL_OFFSET, dcd.PARTITION_BYTES)
    assert t["ota_0"][1] == t["ota_1"][1]


@pytest.mark.parametrize("board", FW)
def test_shipped_firmware_matches_the_table(board):
    t = table()
    info = json.loads((firmware.BINS / board / "flash.json").read_text())
    assert info["flash_settings"]["flash_size"] == "8MB", "one image for 8 and 16 MB boards"
    assert int(info["model_offset"], 16) == t["model"][0] == firmware.model_offset(board) == firmware.model_offset()
    files = {int(k, 16): firmware.BINS / board / v for k, v in info["flash_files"].items()}
    assert set(files) == {0x0, 0x8000, t["otadata"][0], t["ota_0"][0]}, "bootloader, table, boot selector, app"
    app = files[t["ota_0"][0]].read_bytes()
    assert app[0] == 0xE9 and len(app) <= t["ota_0"][1], "the app must fit one slot (or it cannot be updated from the page)"
    img = builds.firmware_image(board)
    for off, f in files.items():
        data = f.read_bytes()
        assert img[off:off + len(data)] == data
    assert len(img) <= t["ota_0"][0] + t["ota_0"][1]
    assert b"recipes" in app and b"/api/rules" in app, "this firmware has no web page"
    assert b"xiao_sense" in app and b"s3cam" in app and b"/api/eye.bin" in app, "one firmware for every board, with the eye"


def test_make_builds(tmp_path):
    index = builds.make(tmp_path, log=lambda *_: None)
    assert set(index["boards"]) == set(BOARDS) and len(index["ideas"]) >= 57
    ids = [e["id"] for e in index["ideas"]]
    assert len(set(ids)) == len(ids)
    for e in index["ideas"]:
        assert e["boards"], e["id"]
        assert not (e["mic"] and "s3cam" in e["boards"]), f"{e['id']} needs a microphone"
        for board, b in e["boards"].items():
            h, tfl = dcd.unpack((tmp_path / b["package"]).read_bytes())
            assert h["recipe"] == e["id"] and h["actions"] and len(tfl) > 100_000
            m = json.loads((tmp_path / b["manifest"]).read_text())
            parts = m["builds"][0]["parts"]
            assert m["builds"][0]["chipFamily"] == "ESP32-S3" and m["new_install_prompt_erase"] is False
            assert [p["offset"] for p in parts] == [0, index["boards"][board]["model_offset"]]
            assert all((tmp_path / board / p["path"]).exists() for p in parts)
            assert parts[0]["path"] == "../firmware.bin", "every build shares the one firmware"
    assert (tmp_path / index["firmware"]).read_bytes() == builds.firmware_image()
    assert (tmp_path / index["app"]).read_bytes()[:1] == bytes([0xE9])    # an app image, for the update button
    assert json.loads((tmp_path / "index.json").read_text()) == index
