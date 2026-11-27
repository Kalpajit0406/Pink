"""Build manifest and artifact key management for offline Leonida stages.
Section 6.2 of OFFLINE_PIPELINE_SPEC.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from typing import Any, Dict, Optional

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import artifact_header


MANIFEST_FILENAME = "build_manifest.json"


def compute_inputs_key(inputs: Dict[str, Any]) -> str:
    """Computes a canonical SHA-256 hex string over key inputs."""
    data = json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


class BuildManifest:
    def __init__(self, path: str):
        self.path = path
        self.data: Dict[str, Any] = {
            "format_version": 1,
            "created_utc": int(time.time()),
            "stages": {}
        }
        self.load()

    def load(self) -> None:
        if os.path.isfile(self.path):
            try:
                with open(self.path, "r", encoding="utf-8") as f:
                    loaded = json.load(f)
                    if isinstance(loaded, dict) and "stages" in loaded:
                        self.data = loaded
            except (json.JSONDecodeError, OSError):
                pass

    def save(self) -> None:
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        temp_path = self.path + ".tmp"
        with open(temp_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.data, f, indent=2, sort_keys=True)
        os.replace(temp_path, self.path)

    def record_stage(self, stage_name: str, inputs: Dict[str, Any], artifact_files: list[str],
                     duration_s: float, outcome: str = "ok", peak_ram_mb: Optional[int] = None) -> None:
        key = compute_inputs_key(inputs)
        artifacts_meta = {}
        for path in artifact_files:
            if os.path.isfile(path):
                size = os.path.getsize(path)
                with open(path, "rb") as f:
                    sha256 = hashlib.sha256(f.read()).hexdigest()
                artifacts_meta[os.path.basename(path)] = {
                    "path": path,
                    "size": size,
                    "sha256": sha256
                }

        self.data["stages"][stage_name] = {
            "key": key,
            "inputs": inputs,
            "completed_utc": int(time.time()),
            "duration_s": round(duration_s, 2),
            "peak_ram_mb": peak_ram_mb,
            "outcome": outcome,
            "artifacts": artifacts_meta
        }
        self.save()

    def is_stage_reusable(self, stage_name: str, inputs: Dict[str, Any], required_artifacts: list[str]) -> bool:
        if stage_name not in self.data["stages"]:
            return False
        stage_info = self.data["stages"][stage_name]
        expected_key = compute_inputs_key(inputs)
        if stage_info.get("key") != expected_key or stage_info.get("outcome") != "ok":
            return False

        artifacts_meta = stage_info.get("artifacts", {})
        for req in required_artifacts:
            base = os.path.basename(req)
            if base not in artifacts_meta or not os.path.isfile(req):
                return False
            # Verify size matches
            if os.path.getsize(req) != artifacts_meta[base].get("size", -1):
                return False
        return True


def check_all_artifacts(out_dir: str) -> bool:
    """Verifies all headers and payload hashes for artifacts in out_dir."""
    print(f"[check-artifacts] Verifying artifacts in {out_dir}...")
    manifest_path = os.path.join(out_dir, MANIFEST_FILENAME)
    if not os.path.isfile(manifest_path):
        print(f"[check-artifacts] No {MANIFEST_FILENAME} found in {out_dir}.")
        return False

    with open(manifest_path, "r", encoding="utf-8") as f:
        m = json.load(f)

    all_ok = True
    stages = m.get("stages", {})
    for stage_name, info in stages.items():
        print(f"[check-artifacts] Checking stage: {stage_name}")
        for art_name, art_data in info.get("artifacts", {}).items():
            path = art_data.get("path")
            if not path or not os.path.isfile(path):
                print(f"  FAILED: Missing artifact {art_name} at {path}")
                all_ok = False
                continue

            with open(path, "rb") as f:
                data = f.read()

            sha = hashlib.sha256(data).hexdigest()
            if sha != art_data.get("sha256"):
                print(f"  FAILED: Checksum mismatch for {art_name}")
                all_ok = False
                continue

            # If it has an ArtifactHeader, validate header fields
            if len(data) >= artifact_header.HEADER_SIZE:
                try:
                    hdr = artifact_header.ArtifactHeader.unpack(data[:artifact_header.HEADER_SIZE])
                    if not hdr.validate_payload(data[artifact_header.HEADER_SIZE:]):
                        print(f"  FAILED: Internal header payload mismatch for {art_name}")
                        all_ok = False
                        continue
                except ValueError:
                    pass  # Non-standard artifact or loose json

            print(f"  OK: {art_name} ({len(data)} bytes, hash verified)")

    if all_ok:
        print("[check-artifacts] VERDICT: All artifacts validated successfully.")
    else:
        print("[check-artifacts] VERDICT: Artifact verification failed.")
    return all_ok


def main() -> int:
    ap = argparse.ArgumentParser(description="Leonida build manifest and artifact checker")
    ap.add_argument("--out", default=".", help="Out directory containing build_manifest.json")
    ap.add_argument("--check-artifacts", action="store_true", help="Validate all recorded artifacts")
    args = ap.parse_args()

    if args.check_artifacts:
        return 0 if check_all_artifacts(args.out) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
