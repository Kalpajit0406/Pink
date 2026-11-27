"""Offline Pipeline Warmer (Milestone O4)
Parses pipelines.manifest, prioritises pipelines by seenCount descending,
and warms the driver pipeline cache and pipeline_cache.bin.
Section 7.2 of OFFLINE_PIPELINE_SPEC.md.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional


def load_manifest(manifest_path: str) -> List[Dict[str, Any]]:
    if not os.path.isfile(manifest_path):
        raise FileNotFoundError(f"Manifest not found: {manifest_path}")

    records: List[Dict[str, Any]] = []
    with open(manifest_path, "r", encoding="utf-8") as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                rec = json.loads(line)
                records.append(rec)
            except json.JSONDecodeError as e:
                print(f"Warning: line {line_num} in {manifest_path} invalid JSON: {e}", file=sys.stderr)

    return records


def sort_and_filter_manifest(
    records: List[Dict[str, Any]],
    budget_mb: int = 256,
) -> List[Dict[str, Any]]:
    """Sorts pipelines by seen count descending and filters duplicates."""
    seen_keys = set()
    unique_records = []
    for r in records:
        key = r.get("key") or str(r.get("stages", []))
        if key not in seen_keys:
            seen_keys.add(key)
            unique_records.append(r)

    # Sort descending by seen count (and compile latency as secondary)
    unique_records.sort(
        key=lambda r: (r.get("seen", 1), r.get("compile_us", 0)),
        reverse=True,
    )
    return unique_records


def warm_pipelines(
    manifest_path: str,
    out_dir: str,
    runtime_exe: Optional[str] = None,
    budget_mb: int = 256,
    verbose: bool = True,
) -> int:
    records = load_manifest(manifest_path)
    if verbose:
        print(f"Loaded {len(records)} records from {manifest_path}")

    prioritized = sort_and_filter_manifest(records, budget_mb=budget_mb)
    if verbose:
        print(f"Prioritized {len(prioritized)} unique pipelines for background warming.")

    sorted_manifest_path = os.path.join(out_dir, "pipelines_warm.manifest")
    with open(sorted_manifest_path, "w", encoding="utf-8") as f:
        for r in prioritized:
            f.write(json.dumps(r) + "\n")

    # If runtime_exe is specified or found, invoke warming
    exe_candidates = []
    if runtime_exe:
        exe_candidates.append(runtime_exe)
    exe_candidates.extend([
        os.path.join(out_dir, "gta6rt.exe"),
        os.path.join("runtime", "build_msvc", "gta6rt.exe"),
        os.path.join(os.environ.get("LEONIDA_RUNTIME_DIR", ""), "gta6rt.exe"),
        r"E:\Leonida\gta6rt.exe",
    ])

    resolved_exe = None
    for cand in exe_candidates:
        if cand and os.path.isfile(cand):
            resolved_exe = os.path.abspath(cand)
            break

    if not resolved_exe:
        if verbose:
            print("gta6rt.exe not found for live driver warming. Manifest is sorted and prepared.")
        return 0

    if verbose:
        print(f"Invoking {resolved_exe} --precompile-pipelines with sorted manifest...")

    env = os.environ.copy()
    env["LEONIDA_MANIFEST_PATH"] = sorted_manifest_path
    env["LEONIDA_WARM_BUDGET_MB"] = str(budget_mb)

    start_t = time.time()
    try:
        proc = subprocess.run(
            [resolved_exe, "--precompile-pipelines"],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=300,
            check=False,
        )
        elapsed = time.time() - start_t
        if verbose:
            print(f"Warming completed in {elapsed:.2f}s (exit code {proc.returncode})")
            if proc.stdout:
                print(proc.stdout[-1000:])
        return proc.returncode
    except Exception as e:
        print(f"Warming error: {e}", file=sys.stderr)
        return 1


def self_test() -> int:
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        manifest_p = os.path.join(tmp, "test.manifest")
        with open(manifest_p, "w", encoding="utf-8") as f:
            f.write(json.dumps({"key": "p1", "type": "graphics", "seen": 5, "stages": ["vs1", "ps1"]}) + "\n")
            f.write(json.dumps({"key": "p2", "type": "graphics", "seen": 50, "stages": ["vs2", "ps2"]}) + "\n")
            f.write(json.dumps({"key": "p3", "type": "compute", "seen": 20, "stages": ["cs1"]}) + "\n")
            f.write(json.dumps({"key": "p1", "type": "graphics", "seen": 5, "stages": ["vs1", "ps1"]}) + "\n") # duplicate

        recs = load_manifest(manifest_p)
        assert len(recs) == 4
        sorted_recs = sort_and_filter_manifest(recs)
        assert len(sorted_recs) == 3
        assert sorted_recs[0]["key"] == "p2"  # highest seen (50)
        assert sorted_recs[1]["key"] == "p3"  # second seen (20)
        assert sorted_recs[2]["key"] == "p1"  # third seen (5)
        print("pipeline_warmer self-test PASSED")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Offline Pipeline Warmer for Leonida.")
    parser.add_argument("manifest", nargs="?", default="pipelines.manifest", help="Path to pipelines.manifest")
    parser.add_argument("--out", default=".", help="Output directory")
    parser.add_argument("--runtime-exe", default=None, help="Path to gta6rt.exe")
    parser.add_argument("--budget-mb", type=int, default=256, help="Cache size budget in MB")
    parser.add_argument("--test", action="store_true", help="Run self-test")
    args = parser.parse_args()

    if args.test:
        return self_test()

    return warm_pipelines(args.manifest, args.out, runtime_exe=args.runtime_exe, budget_mb=args.budget_mb)


if __name__ == "__main__":
    sys.exit(main())
