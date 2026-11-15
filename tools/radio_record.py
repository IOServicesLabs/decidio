"""Record Wi-Fi channel state (CSI) from a board and show how much the radio path is moving.

    python tools/radio_record.py --port COM5 scan
    python tools/radio_record.py --port COM5 listen --channel 6 [--sender aa:bb:cc:dd:ee:ff] [--seconds 60] [--out room.npz]
                                 [--label empty]

The board joins no network and sends nothing: it listens on one channel and reports, for each frame it hears, how every
subcarrier arrived (firmware/main/radio.c). A body moving between sender and board changes that pattern.

`listen` prints one line a second - frames heard and a movement score (how much the pattern changed over the last two
seconds, 0 = perfectly still) - and saves what it heard: amplitudes [frames x subcarriers], time in ms, signal strength
and the sender of every frame, plus the label you gave. Recordings like "empty", "sitting" and "walking" are the
training data for a radio question.

Needs: pip install pyserial numpy
"""
from __future__ import annotations

import argparse
import base64
import binascii
import collections
import json
import sys
import time

import numpy as np


def open_port(port: str):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 921600, 1
    s.dtr = s.rts = False          # opening must not reset the board
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def send(s, line: str) -> None:
    s.write((line + "\n").encode())
    s.flush()


def amplitudes(raw: bytes) -> np.ndarray:
    """int8 (imaginary, real) pairs -> |h| per subcarrier."""
    v = np.frombuffer(raw[:len(raw) // 2 * 2], np.int8).astype(np.float32).reshape(-1, 2)
    return np.hypot(v[:, 0], v[:, 1])


def movement(frames: list[np.ndarray]) -> float:
    """How much the shape of the channel changed over these frames: each frame is scaled to mean 1 (the receiver's gain
    moves on its own), then the spread over time is averaged over the subcarriers that carry signal."""
    if len(frames) < 4:
        return float("nan")
    a = np.stack(frames)
    live = a.mean(0) > 0.2 * a.mean()             # guard bands and the DC carrier are ~0
    a = a[:, live]
    a = a / np.maximum(a.mean(1, keepdims=True), 1e-6)
    return float(a.std(0).mean())


def scan(s) -> None:
    send(s, "radio scan")
    t0 = time.time()
    while time.time() - t0 < 15:
        line = s.readline().decode("utf-8", "ignore").strip()
        if line.startswith('{"radio_scan"'):
            rows = json.loads(line)["radio_scan"]
            print(f"{len(rows)} senders heard (strongest first):")
            for r in rows:
                print(f"  {r['sender']}  channel {r['channel']:2d}  {r['rssi']} dBm")
            return
        if line.startswith('{"error"'):
            print(line)
            return
    print("no answer: is this firmware built with radio.c? (the `info` command works on any build)")


def listen(s, a) -> None:
    send(s, "stop")
    send(s, f"radio {a.channel}" + (f" {a.sender}" if a.sender else ""))
    t0 = time.time()
    rows, by_sender = [], collections.defaultdict(lambda: collections.deque())
    count = collections.Counter()
    nxt = t0 + 1
    try:
        while time.time() - t0 < a.seconds:
            line = s.readline().decode("ascii", "ignore").strip()
            now = time.time()
            if line.startswith("@R "):
                try:
                    _, ms, sender, rssi, ch, payload = line.split(" ", 5)
                    amp = amplitudes(base64.b64decode(payload, validate=True))
                except (ValueError, binascii.Error):
                    continue
                rows.append((int(ms), sender, int(rssi), amp))
                by_sender[(sender, len(amp))].append((now, amp))
                count[sender] += 1
            elif line.startswith('{"error"'):
                print(line)
            if now >= nxt:
                nxt += 1
                parts = []
                for key, q in sorted(by_sender.items(), key=lambda kv: -len(kv[1]))[:3]:
                    while q and now - q[0][0] > 2.0:
                        q.popleft()
                    if q:
                        parts.append(f"{key[0][-8:]} x{key[1]}: {len(q) / 2:.0f}/s move {movement([x for _, x in q]):.3f}")
                print(f"{now - t0:5.0f}s  frames {len(rows):5d}  " + "  |  ".join(parts), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        send(s, "radio off")
        send(s, "run")
    print(f"\n{len(rows)} frames from {len(count)} senders in {time.time() - t0:.0f} s")
    for snd, n in count.most_common(6):
        sizes = collections.Counter(len(r[3]) for r in rows if r[1] == snd)
        print(f"  {snd}  {n} frames  subcarriers {dict(sizes)}")
    if a.out and rows:
        width = collections.Counter(len(r[3]) for r in rows).most_common(1)[0][0]
        keep = [r for r in rows if len(r[3]) == width]          # one width per file, the commonest
        np.savez_compressed(a.out, amp=np.stack([r[3] for r in keep]).astype(np.float32),
                            ms=np.array([r[0] for r in keep], np.int64), rssi=np.array([r[2] for r in keep], np.int8),
                            sender=np.array([r[1] for r in keep]), label=a.label or "", channel=a.channel)
        print(f"saved {len(keep)} frames x {width} subcarriers -> {a.out}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan", help="list the senders the board hears")
    li = sub.add_parser("listen", help="record channel state on one channel")
    li.add_argument("--channel", type=int, required=True)
    li.add_argument("--sender", help="only this sender (address from `scan`)")
    li.add_argument("--seconds", type=float, default=60)
    li.add_argument("--out", help="save to this .npz")
    li.add_argument("--label", help="what was happening in the room (saved with the recording)")
    a = ap.parse_args()
    s = open_port(a.port)
    try:
        scan(s) if a.cmd == "scan" else listen(s, a)
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main())
