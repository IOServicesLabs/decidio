"""Talk to the runtime firmware over USB / UART: collect samples, watch decisions."""
from __future__ import annotations

import base64
import binascii
import json
import time
from pathlib import Path

import numpy as np

from .config import CAM, Project
from .media import luma
from .store import Store


def open_port(port: str, baud: int = 921600):
    import serial
    if "://" in port:                  # socket://host:port (a board behind a serial-over-TCP bridge)
        s = serial.serial_for_url(port, do_not_open=True)
    else:
        s = serial.Serial()
        s.port = port
    s.baudrate, s.timeout = baud, 2
    s.dtr = s.rts = False          # opening must not reset the board
    s.open()
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def send(s, line: str) -> None:
    s.write((line + "\n").encode())
    s.flush()


def collect(p: Project, port: str, count: int = 0, every_ms: int = 1000, prefix: str = "dev-") -> int:
    s = open_port(port)
    st = Store(p)
    session = time.strftime("%Y%m%d-%H%M%S")
    send(s, f"collect {every_ms}")
    n, cur, warned = 0, {}, set()
    print(f"recording samples from {port} every {every_ms} ms into {p.samples}  (Ctrl+C to stop)")
    try:
        while not count or n < count:
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("ascii", "ignore").strip()
            if line.startswith("{\"error\""):
                print(line)
            if not line.startswith("@"):
                continue
            tag, _, payload = line.partition(" ")
            try:
                if tag == "@S":
                    kv = dict(x.split("=", 1) for x in payload.split()[1:] if "=" in x)
                    cur = {"c": int(kv.get("c", 0)), "sr": int(kv.get("sr", 0))}
                elif tag in ("@I", "@P", "@A"):
                    cur[tag] = base64.b64decode(payload, validate=True)
                elif tag == "@E":
                    sample = _to_sample(p, cur, warned)
                    if sample is None:
                        continue
                    sid = f"{prefix}{session}-{n:05d}"
                    st.add(sid, group=f"{prefix}{session}-{n // 30}", source="collect", **sample)
                    n += 1
                    print(f"[{n}] {sid}")
            except (binascii.Error, ValueError):
                cur = {}
    except KeyboardInterrupt:
        pass
    finally:
        st.flush()
        send(s, "collect off")
        s.close()
    print(f"saved {n} samples")
    return n


def _to_sample(p: Project, cur: dict, warned: set):
    out = {}
    c = cur.get("c", 0)
    if p.image and "@I" in cur and c in (1, 3):
        size = CAM * CAM * c
        if len(cur["@I"]) != size or len(cur.get("@P", b"")) != size:
            return None
        img = np.frombuffer(cur["@I"], np.uint8).reshape(CAM, CAM, c)
        prev = np.frombuffer(cur["@P"], np.uint8).reshape(CAM, CAM, c)
        if c == 3 and p.image.colors == 1:
            img, prev = luma(img)[..., None], luma(prev)[..., None]
        elif c == 1 and p.image.colors == 3:
            if "rgb" not in warned:
                print("the board sends gray frames because it has a gray model. "
                      "To collect color, erase the model or flash an RGB model")
                warned.add("rgb")
            return None
        out.update(image=img, prev=prev if p.image.motion else None)
    if p.audio and "@A" in cur:
        if cur.get("sr") != p.audio.sample_rate:
            if "sr" not in warned:
                print(f"the board sends audio at {cur.get('sr')} Hz, but the project uses {p.audio.sample_rate} Hz. "
                      "The audio is not saved")
                warned.add("sr")
        else:
            pcm = np.frombuffer(cur["@A"], np.int16)
            if len(pcm) >= p.audio.clip:
                out["pcm"] = pcm[-p.audio.clip:]
    return out or None


def monitor(port: str, log: str | None = None) -> None:
    s = open_port(port)
    send(s, "info")
    send(s, "run")
    f = open(log, "a", encoding="utf-8") if log else None
    try:
        while True:
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "ignore").strip()
            if not line.startswith("{"):
                if line:
                    print(line)
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                print(line)
                continue
            if f:
                f.write(json.dumps({"t": time.time(), **d}) + "\n")
                f.flush()
            if "answers" in d:
                parts = []
                for q, a in d["answers"].items():
                    if "p_true" in a:
                        parts.append(f"{q} {a['p_true']:.2f}")
                    elif "choice" in a:
                        parts.append(f"{q} {a['choice']} ({a['confidence']:.2f})")
                    else:
                        parts.append(f"{q} {a['score']:.1f}")
                ego = f", camera moved {d['ego'][0]:+d},{d['ego'][1]:+d} px" if "ego" in d else ""
                print(" | ".join(parts) + f"   [{d['invoke_us'] / 1000:.1f} ms, {d['hz']:.0f}/s{ego}]")
            else:
                print(line)
    except KeyboardInterrupt:
        pass
    finally:
        if f:
            f.close()
        s.close()


def _wifi_state(s, wait: float = 3.0) -> dict | None:
    """Ask the board for its Wi-Fi state; None if it did not answer (older firmware, or busy)."""
    send(s, "wifi")
    end = time.time() + wait
    while time.time() < end:
        line = s.readline().decode("utf-8", "replace").strip()
        if line.startswith('{"wifi"'):
            try:
                return json.loads(line)["wifi"]
            except ValueError:
                pass
    return None


def wifi(port: str, ssid: str | None = None, password: str | None = None, forget: bool = False, log=print) -> dict | None:
    """Join the board to a Wi-Fi network over USB (the other route is its own setup network, decidio-xxxx).
    The password goes to the board over the cable and is stored there; it is not printed or kept on this PC."""
    s = open_port(port)
    try:
        if forget:
            send(s, "wifi forget")
        elif ssid:
            if any(c in (ssid + (password or "")) for c in '"\\\r\n'):
                raise SystemExit("decidio cannot send a name or password that contains a double quote or a backslash. "
                                 "Use the setup page of the board")
            send(s, f'wifi "{ssid}" "{password or ""}"')
            log(f"connecting to {ssid} ...")
        st = None
        for _ in range(15 if ssid and not forget else 1):
            time.sleep(2 if ssid and not forget else 0.5)
            st = _wifi_state(s)
            if st is None or st.get("state") == "online" or not ssid:
                break
        if st is None:
            log("the board did not answer. It needs firmware 0.2 or later. "
                "To write the firmware, run  decidio flash firmware --port %s" % port)
        elif st.get("state") == "online":
            log(f"connected, signal {st.get('rssi')} dBm. Open http://{st.get('ip')}  or  http://{st.get('host')}.local")
        elif st.get("state") == "connecting":
            log(f"not connected yet (reason {st.get('reason')}. 15, 202 and 204: the password is wrong. "
                "201: the board did not find the network)")
        else:
            log(f"Wi-Fi is {st.get('state')}. The setup network of the board is {st.get('host')}" +
                (" (no password)" if st.get("setup_open") else ""))
        return st
    finally:
        s.close()
