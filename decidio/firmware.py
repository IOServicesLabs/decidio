"""Flash the prebuilt runtime firmware and models with esptool (no ESP-IDF needed); rebuild the runtime with ESP-IDF."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from .config import BOARDS, Project

PKG = Path(__file__).resolve().parent
BINS = PKG / "firmware_bins"
SRC = PKG.parent / "firmware"
MODEL_OFFSET = 0x510000           # firmware/partitions.csv; the firmware's flash.json carries its own copy
FW = "esp32s3"                    # one firmware for every supported board: it finds its camera and microphone at start


def fw_dir(board: str | None = None) -> Path:
    """The folder with the prebuilt firmware. Since 0.4 there is one for all boards; a folder per board (0.3) still works."""
    if (BINS / FW / "flash.json").exists() or not board:
        return BINS / FW
    return BINS / board


def model_offset(board: str | None = None) -> int:
    """Where the model package goes: from the firmware's flash.json (written with it), else the current table."""
    info = fw_dir(board) / "flash.json"
    if info.exists():
        off = json.loads(info.read_text()).get("model_offset")
        if off:
            return int(off, 16)
    return _table_offset(SRC / "partitions.csv") or MODEL_OFFSET


def _table_offset(csv: Path) -> int | None:
    if csv.exists():
        for line in csv.read_text().splitlines():
            f = [x.strip() for x in line.split(",")]
            if len(f) >= 5 and f[0] == "model":
                return int(f[3], 16)
    return None


def _after(port: str) -> list[str]:
    """A board on its own USB port (XIAO) that was put in download mode with the BOOT button only leaves it through a
    watchdog reset; a board behind a USB-serial chip restarts the usual way."""
    try:
        from serial.tools import list_ports
        if any(p.device == port and p.vid == 0x303A for p in list_ports.comports()):
            return ["--after", "watchdog_reset"]
    except Exception:
        pass
    return []


# USB vendor id -> (board to try, what it is). A guess from the USB chip only: the board itself is not asked.
_USB = {0x303A: ("xiao_sense", "ESP32 native USB (XIAO ESP32S3 Sense uses this)"),
        0x1A86: ("s3cam", "CH340 USB-serial chip (ESP32-S3-CAM UART socket uses this)"),
        0x10C4: (None, "CP210x USB-serial chip (some ESP32 boards)"),
        0x0403: (None, "FTDI USB-serial chip")}


def ports() -> list[dict]:
    """Serial ports that are USB devices, each with a guess at the board behind it."""
    from serial.tools import list_ports
    out = []
    for p in sorted(list_ports.comports(), key=lambda p: p.device):
        if p.vid is None:
            continue
        board, what = _USB.get(p.vid, (None, "unknown USB serial device"))
        out.append({"port": p.device, "usb": f"{p.vid:04x}:{p.pid:04x}", "board": board, "what": what,
                    "description": p.description or ""})
    return out


def _esptool(args: list[str]) -> None:
    import esptool
    major = int(str(getattr(esptool, "__version__", "4")).split(".")[0])
    if major >= 5:    # esptool 5 renamed commands and options to dashes
        args = [a.replace("write_flash", "write-flash").replace("merge_bin", "merge-bin").replace("watchdog_reset", "watchdog-reset")
                if not (a.startswith("--flash_") or a == "--no_stub") else a.replace("_", "-") for a in args]
    cmd = [sys.executable, "-m", "esptool", *args]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def _write(port: str, baud: int, pairs: list[tuple[str, str]]) -> None:
    """write-flash; if the native-USB board drops the connection with the stub loader, retry without it at 115200."""
    base = ["--chip", "esp32s3", "--port", port]
    flat = [x for pr in pairs for x in pr]
    if baud > 230400 and not _after(port) and "://" not in port:
        baud = 230400             # behind a USB-serial chip (CH340 and others) faster speeds fail on many boards and cables
    try:
        _esptool(base + ["--baud", str(baud), *_after(port), "write_flash", *flat])
    except subprocess.CalledProcessError:
        print("trying again without the stub loader at 115200 baud. This is slower, but it continues when a native-USB "
              "board disconnects and connects again")
        _esptool(base + ["--baud", "115200", "--no-stub", *_after(port), "write_flash", *flat])


def flash_firmware(board: str | None, port: str, baud: int = 921600) -> None:
    _write(port, baud, firmware_pairs(board))


def firmware_pairs(board: str | None = None) -> list[tuple[str, str]]:
    """(offset, file) for the prebuilt firmware, lowest offset first."""
    d = fw_dir(board)
    info = d / "flash.json"
    if not info.exists():
        raise SystemExit(f"there is no prebuilt firmware in {d}. Build it with  decidio build-firmware")
    files = json.loads(info.read_text())["flash_files"]
    return [(off, str(d / name)) for off, name in sorted(files.items(), key=lambda kv: int(kv[0], 16))]


def ask_sensors(port: str, wait: float = 25.0) -> dict | None:
    """What the board found at start: {"profile":..,"camera":..,"mic":..,"card":..,...}; None if it did not answer.
    Right after flashing, the port may be gone for a few seconds (a board on its own USB comes back as a new device)."""
    import time
    from .serialio import open_port, send
    end = time.time() + wait
    while time.time() < end:
        try:
            s = open_port(port)
        except Exception:
            time.sleep(1.0)
            continue
        try:
            s.timeout = 0.5
            for _ in range(4):
                send(s, "info")
                stop = time.time() + 1.5
                while time.time() < stop:
                    line = s.readline().decode("utf-8", "replace").strip()
                    if line.startswith('{"info"'):
                        try:
                            got = json.loads(line)["info"].get("sensors") or {}
                        except ValueError:
                            continue
                        if got.get("profile") not in (None, "", "unknown") or time.time() > end - 5:
                            return got                   # "unknown" early on = it has not looked yet: ask again
        except Exception:
            pass
        finally:
            try:
                s.close()
            except Exception:
                pass
        time.sleep(1.0)
    return None


def flash_package(board: str | None, port: str, blob: Path, baud: int = 921600, with_firmware: bool = False) -> None:
    """Write a model package (and, if asked, the firmware in the same pass) to the board."""
    pairs = firmware_pairs(board) if with_firmware else []
    _write(port, baud, pairs + [(hex(model_offset(board)), str(blob))])


def flash_model(p: Project, port: str, baud: int = 921600) -> None:
    blob = p.out / "model.dcd"
    if not blob.exists():
        raise SystemExit("there is no out/model.dcd. Run  decidio export  first")
    hdr = json.loads((p.out / "model_header.json").read_text()) if (p.out / "model_header.json").exists() else {}
    if hdr.get("audio") and not BOARDS[p.board]["mic"]:
        print(f"note: this model uses sound, but board {p.board} has no microphone. The board gives silence to the model "
              "and turns off its sound questions")
    _write(port, baud, [(hex(model_offset(p.board)), str(blob))])
    print("the model is on the board. The board starts again with the new model. "
          "To see its decisions, run  decidio monitor --port %s" % port)


def build_runtime(board: str = FW, out: str | None = None) -> Path:
    """Build firmware/ inside an ESP-IDF shell, collect the .bin files + flash.json."""
    idf = os.environ.get("IDF_PATH")
    if not idf:
        raise SystemExit("ESP-IDF not found. Open the 'ESP-IDF 5.5 PowerShell', or run export.ps1 or export.sh. "
                         "Then try again")
    py = os.environ.get("IDF_PYTHON_ENV_PATH")
    py = (Path(py) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")) if py else Path(shutil.which("python") or sys.executable)
    build = SRC / f"build_{board}"
    cfg = SRC / f"sdkconfig.{board}"
    cmd = [str(py), str(Path(idf) / "tools" / "idf.py"), "-C", str(SRC), "-B", str(build), "-D", f"SDKCONFIG={cfg}",
           "-D", f"SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.{board}", "build"]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    return collect_bins(build, Path(out) if out else BINS / board)


def collect_bins(build: Path, dst: Path) -> Path:
    fa = json.loads((build / "flasher_args.json").read_text())
    dst.mkdir(parents=True, exist_ok=True)
    files = {}
    for off, rel in fa["flash_files"].items():
        name = Path(rel).name
        shutil.copy2(build / rel, dst / name)
        files[off] = name
    (dst / "flash.json").write_text(json.dumps({"chip": "esp32s3", "flash_files": files,
                                                 "flash_settings": fa.get("flash_settings", {}),
                                                 "model_offset": hex(_table_offset(SRC / "partitions.csv") or MODEL_OFFSET)},
                                                indent=2))
    args = ["--chip", "esp32s3", "merge_bin", "-o", str(dst / "decidio_rt_merged.bin")]
    fs = fa.get("flash_settings", {})
    for k in ("flash_mode", "flash_freq", "flash_size"):
        if fs.get(k):
            args += [f"--{k}", fs[k]]
    for off, name in sorted(files.items(), key=lambda kv: int(kv[0], 16)):
        args += [off, str(dst / name)]
    try:
        _esptool(args)
    except subprocess.CalledProcessError:
        print("merge_bin failed. You can use the separate .bin files")
    print(f"firmware binaries in {dst}")
    return dst
