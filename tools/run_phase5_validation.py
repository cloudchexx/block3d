#!/usr/bin/env python3
"""Run Phase 5 validation benchmarks sequentially.

This drives the public CLI executables, not library internals. It verifies the
.b3d against the raw .dat, then runs round-robin and contiguous dispatch
benchmarks for the requested dataset, one run at a time, and compares medians
with tools/compare_dispatch_ab.py.

Packaged-submission note: paths are derived from this script location by
default and can be overridden with --data-dir, --scrub-file, --build-dir, and
--output-root. No fixed machine path is required.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import subprocess
import sys
from pathlib import Path
from typing import List


ROOT = Path(__file__).resolve().parents[1]
PACKAGE_ROOT = ROOT.parents[1]
DEFAULT_DATA = PACKAGE_ROOT / "data"
DEFAULT_SCRUB = PACKAGE_ROOT / "work" / "scrub.bin"
COMPARE = ROOT / "tools" / "compare_dispatch_ab.py"

DATASETS = {
    "test18": ("test18.b3d", "test18.dat"),
    "test50": ("test50.b3d", "test50.dat"),
}


def stamp() -> str:
    return _dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def run_logged(cmd: List[str], log_path: Path, cwd: Path) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"[RUN] {' '.join(cmd)}", flush=True)
    print(f"[LOG] {log_path}", flush=True)
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write(f"COMMAND {' '.join(cmd)}\n")
        log.flush()
        proc = subprocess.Popen(
            cmd,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            sys.stdout.write(line)
            log.write(line)
        rc = proc.wait()
        log.write(f"EXIT_CODE {rc}\n")
    if rc != 0:
        raise SystemExit(f"command failed with exit code {rc}: {' '.join(cmd)}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Phase 5 validation sequentially.")
    parser.add_argument("--dataset", choices=sorted(DATASETS), required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--verify-samples", type=int, default=20000)
    parser.add_argument("--random-count", type=int, default=100)
    parser.add_argument("--seq-count", type=int, default=10)
    parser.add_argument("--cold-scrub-ratio", type=float, default=1.5)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--scrub-file", type=Path, default=DEFAULT_SCRUB)
    parser.add_argument("--build-dir", type=Path, default=PACKAGE_ROOT / "build")
    parser.add_argument("--output-root", type=Path, default=DEFAULT_DATA / "phase5_outputs")
    parser.add_argument("--log-root", type=Path, default=DEFAULT_DATA / "phase5_logs")
    args = parser.parse_args()

    if args.rounds < 1:
        raise SystemExit("--rounds must be >= 1")

    data_dir = args.data_dir.resolve()
    build_dir = args.build_dir.resolve()
    scrub_file = args.scrub_file.resolve()
    cli = build_dir / "Release" / "block3d_cli.exe"
    run_test = build_dir / "Release" / "run_test.exe"

    b3d_name, raw_name = DATASETS[args.dataset]
    b3d = data_dir / b3d_name
    raw = data_dir / raw_name
    required = [cli, run_test, COMPARE, b3d, raw, scrub_file]
    missing = [str(p) for p in required if not p.exists()]
    if missing:
        raise SystemExit("missing required files:\n" + "\n".join(missing))

    run_id = f"{args.dataset}_{stamp()}"
    log_dir = args.log_root.resolve() / run_id
    output_root = args.output_root.resolve() / run_id
    log_dir.mkdir(parents=True, exist_ok=True)
    output_root.mkdir(parents=True, exist_ok=True)

    print(f"[PHASE5] dataset={args.dataset} rounds={args.rounds}", flush=True)
    print(f"[PHASE5] data={data_dir}", flush=True)
    print(f"[PHASE5] scrub={scrub_file}", flush=True)
    print(f"[PHASE5] logs={log_dir}", flush=True)
    print(f"[PHASE5] outputs={output_root}", flush=True)

    run_logged([
        str(cli), "verify", str(b3d), str(raw), "--samples", str(args.verify_samples)
    ], log_dir / f"{args.dataset}_verify.log", data_dir)

    logs_by_dispatch = {"round-robin": [], "contiguous": []}
    for dispatch in ["round-robin", "contiguous"]:
        for round_no in range(1, args.rounds + 1):
            out_dir = output_root / dispatch.replace("-", "_") / f"round_{round_no}"
            log_path = log_dir / f"{args.dataset}_{dispatch.replace('-', '_')}_r{round_no}.log"
            logs_by_dispatch[dispatch].append(log_path)
            cmd = [
                str(run_test),
                "--datasets", args.dataset,
                "--axis", "all",
                "--mode", "all",
                "--random-count", str(args.random_count),
                "--seq-count", str(args.seq_count),
                "--cache-mode", "both",
                "--cold-method", "scrub",
                "--cold-scrub-file", str(scrub_file),
                "--cold-scrub-ratio", str(args.cold_scrub_ratio),
                "--cold-scrub-passes", "1",
                "--cold-settle-ms", "1000",
                "--cold-isolation", "suite",
                "--warmup-scope", "workload",
                "--read-dispatch", dispatch,
                "--output-dir", str(out_dir),
            ]
            run_logged(cmd, log_path, data_dir)

    compare_log = log_dir / f"{args.dataset}_dispatch_compare.log"
    run_logged([
        sys.executable, str(COMPARE),
        "--round-robin", *[str(p) for p in logs_by_dispatch["round-robin"]],
        "--contiguous", *[str(p) for p in logs_by_dispatch["contiguous"]],
    ], compare_log, ROOT)

    print(f"[DONE] dataset={args.dataset}", flush=True)
    print(f"[SUMMARY_LOG] {compare_log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
