"""Offline Memory Profile Generator (Milestone O8)
Processes recorded session telemetry (sotm_memory.jsonl) to produce memory_profile.bin.
Keyed by package version and SHA-256 for optimal tier classification at boot.
Section 7.5 of OFFLINE_PIPELINE_SPEC.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
from artifact_header import (
    ArtifactHeader,
    MAGIC_MEMPROFILE,
    write_artifact_file,
    read_artifact_file,
)

# Struct format:
# uint64_t base_offset (Q)
# uint64_t size_bytes (Q)
# uint8_t  recommended_tier (B)
# uint8_t  access_pattern (B)
# uint16_t confidence_score (H)
# uint32_t sample_count (I)
# uint8_t  reserved[8] (8s)
# Total = 8 + 8 + 1 + 1 + 2 + 4 + 8 = 32 bytes
MEM_ENTRY_FORMAT = "<QQBBHI8s"
MEM_ENTRY_SIZE = struct.calcsize(MEM_ENTRY_FORMAT)
assert MEM_ENTRY_SIZE == 32, f"MemoryProfileEntry size must be 32, got {MEM_ENTRY_SIZE}"

# Tiers
TIER_GPU_LOCAL       = 0
TIER_HOST_VISIBLE    = 1
TIER_PAGED_SYSTEM    = 2
TIER_READBACK        = 3

# Access flags
ACCESS_CPU_WRITE     = 1 << 0
ACCESS_GPU_READ      = 1 << 1
ACCESS_GPU_WRITE     = 1 << 2
ACCESS_ALIASING_SAFE = 1 << 3


class ProfileEntry:
    def __init__(self, base: int, size: int, tier: int, access: int, confidence: int, samples: int):
        self.base = base
        self.size = size
        self.tier = tier
        self.access = access
        self.confidence = min(1000, confidence)
        self.samples = samples

    def pack(self) -> bytes:
        return struct.pack(
            MEM_ENTRY_FORMAT,
            self.base,
            self.size,
            self.tier,
            self.access,
            self.confidence,
            self.samples,
            b"\x00" * 8,
        )


def process_telemetry_records(records: List[dict]) -> List[ProfileEntry]:
    """Aggregates memory allocation records across sessions into confidence-scored entries."""
    grouped: Dict[int, List[dict]] = {}
    for r in records:
        base = r.get("base") or r.get("offset", 0)
        grouped.setdefault(base, []).append(r)

    entries: List[ProfileEntry] = []
    for base, group in grouped.items():
        sample_count = len(group)
        avg_size = int(sum(item.get("size", 0) for item in group) / sample_count)
        
        # Decide tier based on category and observed access
        cat = group[0].get("category", "")
        access = 0
        tier = TIER_PAGED_SYSTEM

        if "texture" in cat.lower():
            tier = TIER_GPU_LOCAL
            access |= ACCESS_GPU_READ | ACCESS_ALIASING_SAFE
        elif "rendertarget" in cat.lower() or "depth" in cat.lower():
            tier = TIER_GPU_LOCAL
            access |= ACCESS_GPU_READ | ACCESS_GPU_WRITE
        elif "upload" in cat.lower() or "ring" in cat.lower():
            tier = TIER_HOST_VISIBLE
            access |= ACCESS_CPU_WRITE | ACCESS_GPU_READ
        elif "bvh" in cat.lower():
            tier = TIER_GPU_LOCAL
            access |= ACCESS_GPU_READ

        confidence = min(1000, sample_count * 200)
        entries.append(ProfileEntry(base, avg_size, tier, access, confidence, sample_count))

    # Sort entries by base address ascending
    entries.sort(key=lambda e: e.base)
    return entries


def generate_memory_profile(
    telemetry_path: str,
    out_path: str,
    package_key: bytes = b"package_xbox_1.0.2.1",
    verbose: bool = True,
) -> int:
    records: List[dict] = []
    if os.path.isfile(telemetry_path):
        with open(telemetry_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    try:
                        records.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass

    if verbose:
        print(f"Loaded {len(records)} telemetry lines from {telemetry_path}")

    entries = process_telemetry_records(records)
    payload = b"".join(e.pack() for e in entries)

    write_artifact_file(out_path, MAGIC_MEMPROFILE, 1, package_key, payload)

    if verbose:
        print(f"Generated {out_path} ({len(payload) + 96} bytes, {len(entries)} profile entries)")
    return len(entries)


def self_test() -> int:
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        telemetry_p = os.path.join(tmp, "sotm_memory.jsonl")
        with open(telemetry_p, "w", encoding="utf-8") as f:
            f.write(json.dumps({"base": 0x10000000, "size": 65536, "category": "TextureVram"}) + "\n")
            f.write(json.dumps({"base": 0x10000000, "size": 65536, "category": "TextureVram"}) + "\n")
            f.write(json.dumps({"base": 0x20000000, "size": 1048576, "category": "UploadRing"}) + "\n")

        bin_p = os.path.join(tmp, "memory_profile.bin")
        count = generate_memory_profile(telemetry_p, bin_p, verbose=False)
        assert count == 2, f"Expected 2 profile entries, got {count}"

        hdr, payload = read_artifact_file(bin_p, MAGIC_MEMPROFILE, 1)
        assert len(payload) == 2 * MEM_ENTRY_SIZE
        entry0 = struct.unpack(MEM_ENTRY_FORMAT, payload[:MEM_ENTRY_SIZE])
        assert entry0[0] == 0x10000000
        assert entry0[2] == TIER_GPU_LOCAL
        assert entry0[4] == 400 # 2 samples * 200
        print("memprofile_gen self-test PASSED")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate memory_profile.bin from SOTM session logs.")
    parser.add_argument("telemetry", nargs="?", default="sotm_memory.jsonl", help="Path to sotm_memory.jsonl")
    parser.add_argument("--out", default="memory_profile.bin", help="Output path for memory_profile.bin")
    parser.add_argument("--test", action="store_true", help="Run self-test")
    args = parser.parse_args()

    if args.test:
        return self_test()

    try:
        generate_memory_profile(args.telemetry, args.out, verbose=True)
    except Exception as e:
        print(f"Error generating memory profile: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
