"""image_utils.py -- Pure Python PPM (P6/P3) to PNG conversion and thumbnailing.

Standard library only (struct, zlib). Supports converting captured swapchain
frames for browser viewing without requiring Pillow or external tools.
"""
from __future__ import annotations

import io
import os
import struct
import zlib


def read_ppm_data(data: bytes) -> tuple[int, int, bytes]:
    """Parse raw bytes of a P6 or P3 PPM file and return (width, height, rgb_pixels)."""
    if len(data) < 3:
        raise ValueError("PPM data too short")
    magic = data[:2]
    if magic not in (b"P6", b"P3"):
        raise ValueError(f"Not a valid PPM image: magic {magic!r}")

    fields: list[int] = []
    pos = 2
    data_len = len(data)

    while len(fields) < 3:
        while pos < data_len and data[pos : pos + 1].isspace():
            pos += 1
        if pos < data_len and data[pos : pos + 1] == b"#":
            while pos < data_len and data[pos : pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < data_len and not data[pos : pos + 1].isspace():
            pos += 1
        if start == pos:
            break
        fields.append(int(data[start:pos]))

    if len(fields) < 3:
        raise ValueError("Invalid PPM header: missing dimensions or maxval")

    width, height, maxval = fields
    pos += 1

    expected_bytes = width * height * 3
    if magic == b"P6":
        pixels = data[pos : pos + expected_bytes]
    else:
        values = data[pos:].split()
        pixels = bytes(int(v) * 255 // maxval for v in values[:expected_bytes])

    if len(pixels) < expected_bytes:
        # Pad if slightly truncated
        pixels = pixels + b"\x00" * (expected_bytes - len(pixels))

    return width, height, pixels


def encode_png(width: int, height: int, rows: list[bytes]) -> bytes:
    """Encode RGB rows into a valid PNG byte stream using standard library zlib."""
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        crc = zlib.crc32(body) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", crc)

    out = io.BytesIO()
    out.write(b"\x89PNG\r\n\x1a\n")
    out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
    out.write(chunk(b"IDAT", zlib.compress(raw, 6)))
    out.write(chunk(b"IEND", b""))
    return out.getvalue()


def ppm_to_png_bytes(data: bytes, max_width: int = 0) -> bytes:
    """Convert PPM byte content to PNG bytes, optionally downscaling by an integer factor."""
    width, height, pixels = read_ppm_data(data)
    step = 1
    if max_width and width > max_width:
        step = 2
        while width // step > max_width:
            step += 1

    stride = width * 3
    rows: list[bytes] = []
    for y in range(0, height, step):
        row = pixels[y * stride : (y + 1) * stride]
        if step > 1:
            row = b"".join(row[x * 3 : x * 3 + 3] for x in range(0, width, step))
        rows.append(row)

    out_width = (width + step - 1) // step
    out_height = len(rows)
    return encode_png(out_width, out_height, rows)


def ppm_file_to_png(src_path: str, dst_path: str, max_width: int = 0) -> None:
    """Convert a PPM file on disk to a PNG file."""
    with open(src_path, "rb") as f:
        data = f.read()
    png_bytes = ppm_to_png_bytes(data, max_width=max_width)
    with open(dst_path, "wb") as f:
        f.write(png_bytes)
