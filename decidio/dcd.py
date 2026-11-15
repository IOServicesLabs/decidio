"""The model package (.dcd) the board reads from its `model` partition (firmware/main/model.c):

    "DCD1" | u32 header length | u32 model length | u32 crc32 of the rest | JSON header | zero padding to 16 | tflite

No TensorFlow here, so packages can be re-made (other rules on the same model) on a PC that only flashes boards.
"""
from __future__ import annotations

import json
import struct
import zlib

MAGIC = b"DCD1"
PARTITION_BYTES = 0x100000         # firmware/partitions.csv: model partition size


def package(h: dict, tfl: bytes) -> bytes:
    js = json.dumps(h, separators=(",", ":")).encode()
    pad = (-(16 + len(js))) % 16
    body = js + b"\0" * pad + tfl
    blob = MAGIC + struct.pack("<III", len(js), len(tfl), zlib.crc32(body) & 0xFFFFFFFF) + body
    if len(blob) > PARTITION_BYTES:
        raise SystemExit(f"the model package ({len(blob)} B) is larger than the model partition ({PARTITION_BYTES} B). "
                         "Use a smaller model preset")
    return blob


def unpack(blob: bytes) -> tuple[dict, bytes]:
    """(header, tflite) of a package; ValueError if it is not one or its checksum is wrong."""
    if len(blob) < 16 or blob[:4] != MAGIC:
        raise ValueError("not a decidio model package")
    jl, ml, crc = struct.unpack("<III", blob[4:16])
    pad = (-(16 + jl)) % 16
    body = blob[16:16 + jl + pad + ml]
    if len(body) != jl + pad + ml or zlib.crc32(body) & 0xFFFFFFFF != crc:
        raise ValueError("the model package is not complete or its checksum is wrong")
    return json.loads(body[:jl]), body[jl + pad:]
