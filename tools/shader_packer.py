"""Offline Shader Archive & Index Generator (Milestones O3 & O5)
Packs loose SPIR-V shaders into shaders.pack and generates sorted binary index shaders.idx.
Supports variant generation (optimised, native 32-wide for Tier A) and validation.
Section 7.1 of OFFLINE_PIPELINE_SPEC.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
from artifact_header import (
    ArtifactHeader,
    MAGIC_SHADERPACK,
    write_artifact_file,
    read_artifact_file,
)

# Magic for shaders.idx: "LNDSHIX1"
MAGIC_SHADERINDEX = b"LNDSHIX1"

# Stage mapping
STAGE_NAME_TO_ID = {
    "vs": 0,
    "ps": 1,
    "cs": 2,
    "gs": 3,
    "ms": 4,
    "hs": 5,
    "ds": 6,
    "as": 7,
}

# Variant kinds
VARIANT_KIND_TRANSLATED = 0
VARIANT_KIND_SPIRV_OPT  = 1
VARIANT_KIND_NATIVE32   = 2
VARIANT_KIND_LOW_COST   = 3

# FlagsA bits
FLAG_WAVE64             = 1 << 0
FLAG_CROSS_LANE         = 1 << 1
FLAG_CROSS_HALF_LANE    = 1 << 2
FLAG_EMULATED_W64       = 1 << 3
FLAG_QUAD_OPS           = 1 << 4
FLAG_TIER_A_WAVE32      = 1 << 5

# Struct format:
# ShaderVariant: kind(I), length(I), offset(Q) -> 16 bytes
# ShaderIndexEntry:
# uint64_t shader_hash (Q)
# uint8_t  fssm_sha256[32] (32s)
# uint8_t  stage (B)
# uint8_t  flagsA (B)
# uint16_t resource_layout_id (H)
# uint32_t variant_count (I)
# ShaderVariant variants[4] (IIQ * 4) -> 64 bytes (IIQIIQIIQIIQ)
# uint8_t  reserved[16] (16s)
INDEX_ENTRY_FORMAT = "<Q32sBBHIIIQIIQIIQIIQ16s"
INDEX_ENTRY_SIZE = struct.calcsize(INDEX_ENTRY_FORMAT)
assert INDEX_ENTRY_SIZE == 128, f"IndexEntry size must be 128, got {INDEX_ENTRY_SIZE}"


class VariantInfo:
    def __init__(self, kind: int, data: bytes):
        self.kind = kind
        self.data = data
        self.length = len(data)
        self.offset = 0


class ShaderRecord:
    def __init__(self, shader_hash: int, fssm_sha256_bytes: bytes, stage: int, flags_a: int, layout_id: int):
        self.shader_hash = shader_hash
        self.fssm_sha256 = fssm_sha256_bytes
        self.stage = stage
        self.flags_a = flags_a
        self.layout_id = layout_id
        self.variants: List[VariantInfo] = []

    def pack_entry(self) -> bytes:
        var_data = []
        for i in range(4):
            if i < len(self.variants):
                v = self.variants[i]
                var_data.extend([v.kind, v.length, v.offset])
            else:
                var_data.extend([0, 0, 0])

        return struct.pack(
            INDEX_ENTRY_FORMAT,
            self.shader_hash,
            self.fssm_sha256,
            self.stage,
            self.flags_a,
            self.layout_id,
            len(self.variants),
            var_data[0], var_data[1], var_data[2],
            var_data[3], var_data[4], var_data[5],
            var_data[6], var_data[7], var_data[8],
            var_data[9], var_data[10], var_data[11],
            b"\x00" * 16,
        )


def hex_to_bytes32(h: str) -> bytes:
    h = h.strip().lower()
    if len(h) < 64:
        h = h.ljust(64, "0")
    elif len(h) > 64:
        h = h[:64]
    return bytes.fromhex(h)


def derive_shader_hash(sha256_bytes: bytes) -> int:
    return struct.unpack("<Q", sha256_bytes[:8])[0]


def is_valid_spirv_header(data: bytes) -> bool:
    if len(data) < 20:
        return False
    magic = struct.unpack("<I", data[:4])[0]
    return magic == 0x07230203 or magic == 0x03022307


def run_spirv_opt(in_bytes: bytes) -> Optional[bytes]:
    """Runs spirv-opt on bytecode if tool is available."""
    try:
        proc = subprocess.run(
            ["spirv-opt", "--target-env=vulkan1.2", "-O", "-", "-o", "-"],
            input=in_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
        if proc.returncode == 0 and is_valid_spirv_header(proc.stdout):
            if len(proc.stdout) <= len(in_bytes):
                return proc.stdout
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def pack_shaders(
    spirv_dir: str,
    out_dir: str,
    enable_opt: bool = True,
    verbose: bool = True,
) -> Tuple[str, str, int, int]:
    """Packs shaders in spirv_dir into shaders.pack and shaders.idx in out_dir."""
    index_path = os.path.join(spirv_dir, "index.json")
    if not os.path.isfile(index_path):
        raise FileNotFoundError(f"Missing main index: {index_path}")

    with open(index_path, "r", encoding="utf-8") as f:
        main_index = json.load(f)

    # Collect entries from main index and subfolders
    entries_to_pack: List[Tuple[str, dict, str]] = []  # (name, meta, base_folder)
    for name, meta in main_index.items():
        if isinstance(meta, dict):
            entries_to_pack.append((name, meta, spirv_dir))

    for sub in ["fspx", "fspx_compute", "fspx_gfx", "bvh", "exe", "library"]:
        sub_idx = os.path.join(spirv_dir, sub, "index.json")
        if os.path.isfile(sub_idx):
            try:
                with open(sub_idx, "r", encoding="utf-8") as f:
                    sub_data = json.load(f)
                for name, meta in sub_data.items():
                    if isinstance(meta, dict):
                        entries_to_pack.append((f"{sub}/{name}", meta, os.path.join(spirv_dir, sub)))
            except Exception as e:
                if verbose:
                    print(f"Warning: could not read {sub_idx}: {e}")

    records: List[ShaderRecord] = []
    total_raw_bytes = 0

    if verbose:
        print(f"Found {len(entries_to_pack)} shader candidates to pack...")

    for name, meta, base_folder in entries_to_pack:
        # Determine content hash
        hash_str = meta.get("fssm_sha256") or meta.get("fspx_code_sha256")
        if not hash_str:
            # Fallback to hash of name
            hash_str = hashlib.sha256(name.encode("utf-8")).hexdigest()

        sha256_bytes = hex_to_bytes32(hash_str)
        shader_hash = derive_shader_hash(sha256_bytes)
        stage_str = meta.get("stage", "vs").lower()
        stage_id = STAGE_NAME_TO_ID.get(stage_str, 0)

        # Flags
        flags = 0
        if meta.get("wave64"):
            flags |= FLAG_WAVE64
        if meta.get("wave64_exchange") == "workgroup":
            flags |= FLAG_EMULATED_W64
        if meta.get("wave64_graphics") == "A":
            flags |= FLAG_TIER_A_WAVE32

        reasons = meta.get("wave64_graphics_reasons", [])
        if "cross-lane" in reasons:
            flags |= FLAG_CROSS_LANE
        if "mask-half" in reasons:
            flags |= FLAG_CROSS_HALF_LANE
        if "quad" in reasons:
            flags |= FLAG_QUAD_OPS

        # Find .spv file
        spv_leaf = meta.get("spv_file") or f"{os.path.basename(name)}.spv"
        spv_path = os.path.join(base_folder, spv_leaf)
        if not os.path.isfile(spv_path):
            spv_path = os.path.join(spirv_dir, f"{name}.spv")
        if not os.path.isfile(spv_path):
            continue

        with open(spv_path, "rb") as f:
            spv_bytes = f.read()

        if not is_valid_spirv_header(spv_bytes):
            continue

        # 4-byte align spv_bytes if needed
        if len(spv_bytes) % 4 != 0:
            spv_bytes += b"\x00" * (4 - (len(spv_bytes) % 4))

        record = ShaderRecord(shader_hash, sha256_bytes, stage_id, flags, 0)
        # Variant 0: original translated
        v0 = VariantInfo(VARIANT_KIND_TRANSLATED, spv_bytes)
        record.variants.append(v0)

        # Variant 1: spirv-opt (if enabled)
        if enable_opt:
            opt_bytes = run_spirv_opt(spv_bytes)
            if opt_bytes and len(opt_bytes) < len(spv_bytes):
                v1 = VariantInfo(VARIANT_KIND_SPIRV_OPT, opt_bytes)
                record.variants.append(v1)

        # Variant 2: Tier A Native 32-wide
        if (flags & FLAG_TIER_A_WAVE32) and not (flags & FLAG_CROSS_LANE):
            # Safe as native 32-wide
            v2 = VariantInfo(VARIANT_KIND_NATIVE32, spv_bytes)
            record.variants.append(v2)

        records.append(record)
        total_raw_bytes += len(spv_bytes)

    # Sort records ascending by (shader_hash, fssm_sha256)
    records.sort(key=lambda r: (r.shader_hash, r.fssm_sha256))

    # Assemble pack payload and assign offsets
    pack_payload_chunks: List[bytes] = []
    current_offset = 0

    for r in records:
        for v in r.variants:
            v.offset = current_offset
            pack_payload_chunks.append(v.data)
            current_offset += len(v.data)

    pack_payload = b"".join(pack_payload_chunks)

    # Assemble idx payload
    idx_payload_chunks = [r.pack_entry() for r in records]
    idx_payload = b"".join(idx_payload_chunks)

    # Write files
    os.makedirs(out_dir, exist_ok=True)
    pack_path = os.path.join(out_dir, "shaders.pack")
    idx_path = os.path.join(out_dir, "shaders.idx")

    key_inputs = f"shaderpack_v1:{len(records)}:{total_raw_bytes}".encode("utf-8")
    write_artifact_file(pack_path, MAGIC_SHADERPACK, 1, key_inputs, pack_payload)
    write_artifact_file(idx_path, MAGIC_SHADERINDEX, 1, key_inputs, idx_payload)

    if verbose:
        print(f"Packed {len(records)} shaders into:")
        print(f"  {pack_path} ({len(pack_payload) + 96} bytes)")
        print(f"  {idx_path} ({len(idx_payload) + 96} bytes)")

    return pack_path, idx_path, len(records), len(pack_payload)


def self_test() -> int:
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        # Create a synthetic index and 3 dummy SPIR-V files
        spirv_magic = struct.pack("<I", 0x07230203)
        fake_spv1 = spirv_magic + b"\x00" * 60
        fake_spv2 = spirv_magic + b"\x11" * 80
        fake_spv3 = spirv_magic + b"\x22" * 100

        with open(os.path.join(tmp, "sh1.spv"), "wb") as f:
            f.write(fake_spv1)
        with open(os.path.join(tmp, "sh2.spv"), "wb") as f:
            f.write(fake_spv2)
        with open(os.path.join(tmp, "sh3.spv"), "wb") as f:
            f.write(fake_spv3)

        idx = {
            "sh1": {"stage": "vs", "fssm_sha256": "1" * 64, "wave64": True, "wave64_graphics": "A"},
            "sh2": {"stage": "ps", "fssm_sha256": "2" * 64, "wave64": False},
            "sh3": {"stage": "cs", "fssm_sha256": "3" * 64, "wave64_exchange": "workgroup"},
        }
        with open(os.path.join(tmp, "index.json"), "w", encoding="utf-8") as f:
            json.dump(idx, f)

        out_dir = os.path.join(tmp, "out")
        pack_p, idx_p, count, payload_len = pack_shaders(tmp, out_dir, enable_opt=False, verbose=False)

        assert count == 3, f"Expected 3 records, got {count}"
        h_pack, p_pack = read_artifact_file(pack_p, MAGIC_SHADERPACK, 1)
        h_idx, p_idx = read_artifact_file(idx_p, MAGIC_SHADERINDEX, 1)

        assert len(p_idx) == 3 * INDEX_ENTRY_SIZE
        # Verify first entry can be unpacked
        entry0 = p_idx[:INDEX_ENTRY_SIZE]
        unpacked = struct.unpack(INDEX_ENTRY_FORMAT, entry0)
        assert unpacked[2] in STAGE_NAME_TO_ID.values()
        print(f"shader_packer self-test PASSED ({count} shaders packed, verified idx & pack)")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Pack loose SPIR-V shaders into shaders.pack and shaders.idx.")
    parser.add_argument("spirv_dir", nargs="?", default=".", help="Directory containing translated spirv/index.json")
    parser.add_argument("--out", default=".", help="Output directory for shaders.pack and shaders.idx")
    parser.add_argument("--no-opt", action="store_true", help="Disable spirv-opt variant generation")
    parser.add_argument("--test", action="store_true", help="Run self-test")
    args = parser.parse_args()

    if args.test:
        return self_test()

    try:
        pack_shaders(args.spirv_dir, args.out, enable_opt=not args.no_opt, verbose=True)
    except Exception as e:
        print(f"Error packing shaders: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
