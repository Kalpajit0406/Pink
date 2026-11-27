"""Standard 88-byte Artifact Header generator and verifier for offline Leonida artifacts.
Section 5.2 of OFFLINE_PIPELINE_SPEC.md.
"""
from __future__ import annotations

import hashlib
import os
import struct
import sys
import time
from typing import Optional

HEADER_FORMAT = "<8sII32sQQ32s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 96, f"ArtifactHeader size must be 96, got {HEADER_SIZE}"

MAGIC_SHADERPACK = b"LNDSHPK1"
MAGIC_TEXPACK    = b"LNDTXPK1"
MAGIC_PATCHES    = b"LNDPTCH1"
MAGIC_MEMPROFILE = b"LNDMPRF1"
MAGIC_PASSCOSTS  = b"LNDPPCM1"


class ArtifactHeader:
    def __init__(self, magic: bytes, format_version: int, key: bytes,
                 created_utc: int, payload_size: int, payload_sha256: bytes):
        if len(magic) != 8:
            raise ValueError(f"Magic must be exactly 8 bytes, got {len(magic)}")
        if len(key) != 32:
            raise ValueError(f"Key must be exactly 32 bytes SHA-256, got {len(key)}")
        if len(payload_sha256) != 32:
            raise ValueError(f"Payload SHA-256 must be exactly 32 bytes, got {len(payload_sha256)}")

        self.magic = magic
        self.format_version = format_version
        self.header_size = HEADER_SIZE
        self.key = key
        self.created_utc = created_utc
        self.payload_size = payload_size
        self.payload_sha256 = payload_sha256

    def pack(self) -> bytes:
        return struct.pack(
            HEADER_FORMAT,
            self.magic,
            self.format_version,
            self.header_size,
            self.key,
            self.created_utc,
            self.payload_size,
            self.payload_sha256
        )

    @classmethod
    def unpack(cls, data: bytes) -> ArtifactHeader:
        if len(data) < HEADER_SIZE:
            raise ValueError(f"Data too short for ArtifactHeader: {len(data)} < {HEADER_SIZE}")
        (magic, fmt_ver, hdr_size, key,
         created_utc, payload_size, payload_sha) = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])
        if hdr_size != HEADER_SIZE:
            raise ValueError(f"Header size mismatch: recorded {hdr_size} != expected {HEADER_SIZE}")
        return cls(magic, fmt_ver, key, created_utc, payload_size, payload_sha)

    @classmethod
    def create(cls, magic: bytes, format_version: int, key_inputs: bytes, payload: bytes) -> ArtifactHeader:
        key = hashlib.sha256(key_inputs).digest()
        payload_sha256 = hashlib.sha256(payload).digest()
        created_utc = int(time.time())
        return cls(magic, format_version, key, created_utc, len(payload), payload_sha256)

    def validate_payload(self, payload: bytes) -> bool:
        if len(payload) != self.payload_size:
            return False
        return hashlib.sha256(payload).digest() == self.payload_sha256


def write_artifact_file(path: str, magic: bytes, format_version: int, key_inputs: bytes, payload: bytes) -> None:
    header = ArtifactHeader.create(magic, format_version, key_inputs, payload)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header.pack())
        f.write(payload)


def read_artifact_file(path: str, expected_magic: Optional[bytes] = None, expected_version: Optional[int] = None) -> tuple[ArtifactHeader, bytes]:
    with open(path, "rb") as f:
        hdr_bytes = f.read(HEADER_SIZE)
        if len(hdr_bytes) < HEADER_SIZE:
            raise ValueError(f"File {path} is smaller than artifact header ({len(hdr_bytes)} < {HEADER_SIZE})")
        header = ArtifactHeader.unpack(hdr_bytes)
        if expected_magic and header.magic != expected_magic:
            raise ValueError(f"Magic mismatch in {path}: {header.magic!r} != {expected_magic!r}")
        if expected_version is not None and header.format_version != expected_version:
            raise ValueError(f"Version mismatch in {path}: {header.format_version} != {expected_version}")
        payload = f.read()
        if not header.validate_payload(payload):
            raise ValueError(f"Payload corrupted or truncated in {path}")
        return header, payload


def main() -> int:
    if "--test" in sys.argv:
        magic = MAGIC_SHADERPACK
        version = 1
        key_inputs = b"test_key_v1"
        payload = b"Sample SPIR-V or patch payload 123456789"
        hdr = ArtifactHeader.create(magic, version, key_inputs, payload)
        packed = hdr.pack()
        assert len(packed) == 96, f"Packed header size {len(packed)} != 96"
        hdr2 = ArtifactHeader.unpack(packed)
        assert hdr2.magic == magic
        assert hdr2.format_version == version
        assert hdr2.validate_payload(payload)
        print("artifact_header self-test PASSED (header size = 96 bytes)")
        return 0
    print("Usage: python -I tools/artifact_header.py --test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
