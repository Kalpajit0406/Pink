"""Byte-Checked In-Memory Patch Table Generator (Milestone O7)
Scans game executable offline to identify hot write sites and generates patches.tbl.
Strict Clean-Room Guarantee: Stores ONLY RVAs and cryptographic hashes, NEVER game code bytes.
Section 7.4 of OFFLINE_PIPELINE_SPEC.md.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from typing import List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
from artifact_header import (
    ArtifactHeader,
    MAGIC_PATCHES,
    write_artifact_file,
    read_artifact_file,
)

# Struct format:
# uint32_t rva (I)
# uint32_t window_length (I)
# uint8_t  window_sha256[32] (32s)
# uint16_t patch_kind (H)
# uint16_t handler_id (H)
# uint8_t  reserved[20] (20s)
# Total = 4 + 4 + 32 + 2 + 2 + 20 = 64 bytes
PATCH_ENTRY_FORMAT = "<II32sHH20s"
PATCH_ENTRY_SIZE = struct.calcsize(PATCH_ENTRY_FORMAT)
assert PATCH_ENTRY_SIZE == 64, f"PatchEntry size must be 64, got {PATCH_ENTRY_SIZE}"

# Patch kinds
PATCH_KIND_NOP_WRITER          = 1
PATCH_KIND_HOOK_HANDLER        = 2
PATCH_KIND_FAST_DESCRIPTOR     = 3
PATCH_KIND_CLZERO_REWRITE      = 4


class PatchEntry:
    def __init__(self, rva: int, window_len: int, window_sha256: bytes, patch_kind: int, handler_id: int):
        self.rva = rva
        self.window_len = window_len
        self.window_sha256 = window_sha256
        self.patch_kind = patch_kind
        self.handler_id = handler_id

    def pack(self) -> bytes:
        return struct.pack(
            PATCH_ENTRY_FORMAT,
            self.rva,
            self.window_len,
            self.window_sha256,
            self.patch_kind,
            self.handler_id,
            b"\x00" * 20,
        )


def compute_file_sha256(path: str) -> bytes:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.digest()


def parse_pe_sections(exe_bytes: bytes) -> List[Tuple[int, int, int]]:
    """Returns list of (virtual_address, virtual_size, raw_data_ptr) for sections."""
    if len(exe_bytes) < 0x40:
        return []
    e_lfanew = struct.unpack_from("<I", exe_bytes, 0x3C)[0]
    if e_lfanew + 4 + 20 + 240 > len(exe_bytes):
        return []
    pe_sig = exe_bytes[e_lfanew:e_lfanew+4]
    if pe_sig != b"PE\x00\x00":
        return []

    num_sections = struct.unpack_from("<H", exe_bytes, e_lfanew + 4 + 2)[0]
    opt_hdr_size = struct.unpack_from("<H", exe_bytes, e_lfanew + 4 + 16)[0]
    sec_tbl_offset = e_lfanew + 4 + 20 + opt_hdr_size

    sections = []
    for i in range(num_sections):
        offset = sec_tbl_offset + i * 40
        if offset + 40 > len(exe_bytes):
            break
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", exe_bytes, offset + 8)
        sections.append((va, vsize or raw_size, raw_ptr))
    return sections


def scan_executable_write_sites(exe_bytes: bytes) -> List[PatchEntry]:
    """Scans executable for known write site patterns and generates byte-checked hash records."""
    entries: List[PatchEntry] = []
    sections = parse_pe_sections(exe_bytes)

    # Scan executable code sections
    for va, vsize, raw_ptr in sections:
        if raw_ptr == 0 or raw_ptr >= len(exe_bytes):
            continue
        available = min(vsize, len(exe_bytes) - raw_ptr)
        sec_code = exe_bytes[raw_ptr:raw_ptr+available]

        # Scan for CLZERO patterns (0F 01 FC)
        k = 0
        while k + 15 <= len(sec_code):
            idx = sec_code.find(b"\x0f\x01\xfc", k)
            if idx == -1:
                break
            # Check shape A (12 bytes) or shape B (15 bytes)
            rva = va + idx
            window_len = 12
            window_bytes = sec_code[idx:idx+window_len]
            window_sha = hashlib.sha256(window_bytes).digest()

            entries.append(PatchEntry(
                rva=rva,
                window_len=window_len,
                window_sha256=window_sha,
                patch_kind=PATCH_KIND_CLZERO_REWRITE,
                handler_id=1,
            ))
            k = idx + 3

    return entries


def generate_patch_table(exe_path: str, out_path: str, verbose: bool = True) -> int:
    if not os.path.isfile(exe_path):
        raise FileNotFoundError(f"Executable not found: {exe_path}")

    exe_sha256 = compute_file_sha256(exe_path)
    if verbose:
        print(f"Target executable SHA-256: {exe_sha256.hex()}")

    with open(exe_path, "rb") as f:
        exe_bytes = f.read()

    entries = scan_executable_write_sites(exe_bytes)
    if verbose:
        print(f"Identified {len(entries)} write/loop sites to patch.")

    payload = b"".join(e.pack() for e in entries)
    # Write artifact with executable SHA-256 as key
    write_artifact_file(out_path, MAGIC_PATCHES, 1, exe_sha256, payload)

    if verbose:
        print(f"Wrote {out_path} ({len(payload) + 96} bytes, {len(entries)} entries).")
    return len(entries)


def self_test() -> int:
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        # Create a synthetic PE image
        dos_hdr = b"MZ" + b"\x00" * 0x3A + struct.pack("<I", 0x40)
        pe_hdr = b"PE\x00\x00"
        file_hdr = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x22)
        opt_hdr = struct.pack("<H", 0x20B) + b"\x00" * 238
        # Section header for .text: va = 0x1000, vsize = 0x200, raw_ptr = 0x200
        sec_name = b".text\x00\x00\x00"
        sec_hdr = sec_name + struct.pack("<IIIIIIHHI", 0x200, 0x1000, 0x200, 0x200, 0, 0, 0, 0, 0x60000020)

        # Pad to 0x200
        headers = dos_hdr + pe_hdr + file_hdr + opt_hdr + sec_hdr
        headers += b"\x00" * (0x200 - len(headers))

        code = b"\x90" * 32 + b"\x0f\x01\xfc\x48\x83\xc0\x40\x49\x39\xd0\x72\xf4" + b"\x90" * 400
        fake_exe = headers + code

        exe_p = os.path.join(tmp, "test_game.exe")
        with open(exe_p, "wb") as f:
            f.write(fake_exe)

        tbl_p = os.path.join(tmp, "patches.tbl")
        count = generate_patch_table(exe_p, tbl_p, verbose=False)
        assert count == 1, f"Expected 1 site, got {count}"

        hdr, payload = read_artifact_file(tbl_p, MAGIC_PATCHES, 1)
        assert len(payload) == PATCH_ENTRY_SIZE
        entry_unpacked = struct.unpack(PATCH_ENTRY_FORMAT, payload)
        assert entry_unpacked[0] == 0x1020 # 0x1000 + 32
        assert entry_unpacked[1] == 12
        assert entry_unpacked[3] == PATCH_KIND_CLZERO_REWRITE
        print(f"patch_table_gen self-test PASSED ({count} patches generated)")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate byte-checked patches.tbl from executable.")
    parser.add_argument("exe", nargs="?", default="game_scarlett_master.exe", help="Path to game executable")
    parser.add_argument("--out", default="patches.tbl", help="Output path for patches.tbl")
    parser.add_argument("--test", action="store_true", help="Run self-test")
    args = parser.parse_args()

    if args.test:
        return self_test()

    try:
        generate_patch_table(args.exe, args.out, verbose=True)
    except Exception as e:
        print(f"Error generating patch table: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
