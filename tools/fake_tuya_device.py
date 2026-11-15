"""A pretend Smart Life (Tuya) switch on this PC, to test a board's device control without a real device or its key.

It speaks the local protocol (3.1, 3.3, 3.4 or 3.5) on TCP port 6668 and keeps one on/off value. Its framing and
ciphers come from tinytuya (pip install tinytuya), the reference client, so the bytes are the ones a real client
expects. On the board's Devices tab add a device with this PC's address, the id and key below, and the same version.

    python tools/fake_tuya_device.py --version 3.5
"""
from __future__ import annotations

import argparse
import hmac
import json
import os
import socket
import threading
import time
from hashlib import md5, sha256

from tinytuya.core import command_types as CT
from tinytuya.core import header as H
from tinytuya.core.crypto_helper import AESCipher
from tinytuya.core.message_helper import TuyaMessage, pack_message, parse_header, unpack_message

ZERO = b"\x00\x00\x00\x00"


class Device:
    def __init__(self, dev_id: str, key: str, version: str, dp: int, log=print):
        self.id, self.key, self.version, self.dp, self.log = dev_id, key.encode(), version, str(dp), log
        self.v = float(version)
        self.dps = {self.dp: False}
        self.changes = []                                   # (time, value) of every accepted command

    # ---- one connection ----
    def serve(self, c: socket.socket) -> None:
        key, seq, nonce, buf = self.key, 1, b"", b""
        hdr = self.version.encode() + 12 * b"\x00"

        def send(cmd, payload, k):
            nonlocal seq
            if self.v >= 3.5:
                m = TuyaMessage(seq, cmd, 0, payload, 0, True, H.PREFIX_6699_VALUE, True)
                data = pack_message(m, hmac_key=k)
            else:
                if self.v >= 3.4:
                    payload = AESCipher(k).encrypt(payload, False)
                m = TuyaMessage(seq, cmd, 0, ZERO + payload, 0, True, H.PREFIX_55AA_VALUE, False)
                data = pack_message(m, hmac_key=k if self.v >= 3.4 else None)
            seq += 1
            c.sendall(data)

        def status_json(wrapped: bool) -> bytes:
            if wrapped:
                return json.dumps({"protocol": 4, "t": int(time.time()), "data": {"dps": self.dps}}).replace(" ", "").encode()
            return json.dumps({"devId": self.id, "dps": self.dps}).replace(" ", "").encode()

        try:
            while True:
                while True:
                    try:
                        head = parse_header(buf)
                        if len(buf) >= head.total_length:
                            break
                    except Exception:
                        pass
                    more = c.recv(2048)
                    if not more:
                        return
                    buf += more
                raw, buf = buf[:head.total_length], buf[head.total_length:]
                m = unpack_message(raw, hmac_key=key if self.v >= 3.4 else None, header=head, no_retcode=True)
                if not m.crc_good:
                    self.log("  a message that does not check out (wrong key?): ignored")
                    continue
                p = m.payload
                if self.v == 3.4:
                    p = AESCipher(key).decrypt(p, False, decode_text=False)
                if m.cmd == CT.SESS_KEY_NEG_START and self.v >= 3.4:
                    client, nonce = p[:16], os.urandom(16)
                    send(CT.SESS_KEY_NEG_RESP, nonce + hmac.new(self.key, client, sha256).digest(), self.key)
                    xor = bytes(a ^ b for a, b in zip(client, nonce))
                    if self.v >= 3.5:
                        session = AESCipher(self.key).encrypt(xor, use_base64=False, pad=False, iv=client[:12])[12:28]
                    else:
                        session = AESCipher(self.key).encrypt(xor, False, pad=False)
                    continue
                if m.cmd == CT.SESS_KEY_NEG_FINISH and self.v >= 3.4:
                    if p[:32] != hmac.new(self.key, nonce, sha256).digest():
                        self.log("  session check failed (wrong key)")
                        return
                    key = session
                    continue
                if self.v >= 3.4 and key is self.key:
                    self.log("  a command before the session was agreed: ignored")
                    continue
                if m.cmd in (CT.DP_QUERY, CT.DP_QUERY_NEW):
                    if self.v == 3.3:
                        AESCipher(key).decrypt(p, False, decode_text=False)      # must decrypt, or the key is wrong
                        send(m.cmd, AESCipher(key).encrypt(status_json(False), False), key)
                    else:
                        send(m.cmd, status_json(False), key)
                    self.log(f"  status asked -> {self.dps}")
                elif m.cmd in (CT.CONTROL, CT.CONTROL_NEW):
                    if self.v == 3.1:
                        body = p[3 + 16:]
                        want = md5(b"data=" + body + b"||lpv=3.1||" + self.key).hexdigest()[8:24].encode()
                        if p[:3] != b"3.1" or p[3:19] != want:
                            self.log("  command with a wrong signature: refused")
                            continue
                        p = AESCipher(key).decrypt(body, True, decode_text=False)
                    elif self.v == 3.3:
                        p = AESCipher(key).decrypt(p[len(hdr):], False, decode_text=False)
                    else:
                        p = p[len(hdr):]
                    j = json.loads(p)
                    dps = j.get("dps") or j.get("data", {}).get("dps") or {}
                    for k, val in dps.items():
                        self.dps[str(k)] = val
                        self.changes.append((time.time(), str(k), val))
                    self.log(f"  command {dps} -> now {self.dps}")
                    send(m.cmd, b"", key)
                    if self.v >= 3.4:
                        send(CT.STATUS, hdr + status_json(True), key)
                    elif self.v == 3.3:
                        send(CT.STATUS, hdr + AESCipher(key).encrypt(status_json(False), False), key)
                elif m.cmd == CT.HEART_BEAT:
                    send(m.cmd, b"", key)
        except (ConnectionError, OSError):
            pass
        except Exception as e:                              # what a wrong key decrypts to: not a command
            self.log(f"  a message that cannot be read (wrong key?): {e}")
        finally:
            c.close()

    def listen(self, port: int = 6668) -> socket.socket:
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", port))
        srv.listen(4)

        def loop():
            while True:
                try:
                    c, who = srv.accept()
                except OSError:
                    return
                self.log(f"connection from {who[0]}")
                threading.Thread(target=self.serve, args=(c,), daemon=True).start()

        threading.Thread(target=loop, daemon=True).start()
        return srv


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", default="3.5", choices=["3.1", "3.3", "3.4", "3.5"])
    ap.add_argument("--id", default="fake0000000000000000dv")
    ap.add_argument("--key", default="0123456789abcdef", help="16 characters")
    ap.add_argument("--dp", type=int, default=1, help="the on/off data point (bulbs often use 20)")
    ap.add_argument("--port", type=int, default=6668)
    a = ap.parse_args()
    if len(a.key) != 16:
        raise SystemExit("--key must be 16 characters")
    Device(a.id, a.key, a.version, a.dp).listen(a.port)
    print(f"pretend device: id {a.id}  key {a.key}  version {a.version}  on/off data point {a.dp}  port {a.port}")
    print("Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
