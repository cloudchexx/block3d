#!/usr/bin/env python3
"""Compare Phase 4 read-dispatch A/B benchmark logs.

The script is intentionally read-only: run the benchmarks separately with identical
control variables except `--read-dispatch`, then pass all logs here.

Example:
  python tools/compare_dispatch_ab.py --round-robin logs/rr_1.log logs/rr_2.log logs/rr_3.log \
      --contiguous logs/contig_1.log logs/contig_2.log logs/contig_3.log
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Tuple


FIELD_RE = re.compile(r"(\w+)=([^\s]+)")


@dataclass(frozen=True)
class Sample:
    path: Path
    strategy: str
    cache: str
    mode: str
    axis: str
    total_sec: float
    plan_hash: str

    @property
    def key(self) -> Tuple[str, str, str]:
        return (self.cache, self.mode, self.axis)


def fields_from_line(line: str) -> Dict[str, str]:
    return {match.group(1): match.group(2) for match in FIELD_RE.finditer(line)}


def normalize_strategy(value: str) -> str:
    value = value.strip().replace("-", "_")
    if value not in {"round_robin", "contiguous"}:
        raise ValueError(f"unknown dispatch strategy: {value}")
    return value


def parse_log(path: Path, expected_strategy: str) -> List[Sample]:
    strategy = ""
    samples: List[Sample] = []
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("READ_DISPATCH "):
                fields = fields_from_line(line)
                if "strategy" in fields:
                    strategy = normalize_strategy(fields["strategy"])
            elif line.startswith("BENCHMARK_RESULT "):
                fields = fields_from_line(line)
                required = {"cache", "mode", "axis", "total_sec", "plan_hash"}
                if not required.issubset(fields):
                    continue
                active_strategy = strategy or expected_strategy
                samples.append(Sample(
                    path=path,
                    strategy=active_strategy,
                    cache=fields["cache"],
                    mode=fields["mode"],
                    axis=fields["axis"],
                    total_sec=float(fields["total_sec"]),
                    plan_hash=fields["plan_hash"].lower(),
                ))
    if not samples:
        raise ValueError(f"{path}: no BENCHMARK_RESULT rows found")
    if strategy and strategy != expected_strategy:
        raise ValueError(
            f"{path}: READ_DISPATCH strategy={strategy}, expected {expected_strategy}"
        )
    return samples


def collect(paths: Iterable[Path], strategy: str) -> Dict[Tuple[str, str, str], List[Sample]]:
    grouped: Dict[Tuple[str, str, str], List[Sample]] = {}
    for path in paths:
        for sample in parse_log(path, strategy):
            grouped.setdefault(sample.key, []).append(sample)
    return grouped


def median_total(samples: List[Sample]) -> float:
    return statistics.median(s.total_sec for s in samples)


def check_plan_hashes(key: Tuple[str, str, str], rr: List[Sample], cc: List[Sample]) -> List[str]:
    rr_hashes = {s.plan_hash for s in rr}
    cc_hashes = {s.plan_hash for s in cc}
    if len(rr_hashes) != 1 or len(cc_hashes) != 1 or rr_hashes != cc_hashes:
        return [
            f"PLAN_HASH mismatch for cache={key[0]} mode={key[1]} axis={key[2]}: "
            f"round_robin={sorted(rr_hashes)} contiguous={sorted(cc_hashes)}"
        ]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare round-robin and contiguous dispatch benchmark medians."
    )
    parser.add_argument("--round-robin", nargs="+", type=Path, required=True,
                        help="Log files produced with --read-dispatch round-robin")
    parser.add_argument("--contiguous", nargs="+", type=Path, required=True,
                        help="Log files produced with --read-dispatch contiguous")
    parser.add_argument("--min-repeats", type=int, default=3,
                        help="Minimum samples required per strategy/key (default: 3)")
    parser.add_argument("--switch-threshold", type=float, default=0.03,
                        help="Required overall median improvement to switch default (default: 0.03)")
    parser.add_argument("--max-regression", type=float, default=0.05,
                        help="Allowed per-case stable regression (default: 0.05)")
    args = parser.parse_args()

    try:
        rr = collect(args.round_robin, "round_robin")
        cc = collect(args.contiguous, "contiguous")
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    keys = sorted(set(rr) & set(cc))
    missing_rr = sorted(set(cc) - set(rr))
    missing_cc = sorted(set(rr) - set(cc))
    warnings: List[str] = []
    if missing_rr:
        warnings.append(f"missing round_robin keys: {missing_rr}")
    if missing_cc:
        warnings.append(f"missing contiguous keys: {missing_cc}")

    rows = []
    for key in keys:
        rr_samples = rr[key]
        cc_samples = cc[key]
        if len(rr_samples) < args.min_repeats or len(cc_samples) < args.min_repeats:
            warnings.append(
                f"sample count below {args.min_repeats} for cache={key[0]} mode={key[1]} axis={key[2]}: "
                f"round_robin={len(rr_samples)} contiguous={len(cc_samples)}"
            )
        warnings.extend(check_plan_hashes(key, rr_samples, cc_samples))
        rr_med = median_total(rr_samples)
        cc_med = median_total(cc_samples)
        ratio = cc_med / rr_med if rr_med > 0 else float("inf")
        rows.append((key, rr_med, cc_med, ratio))

    if not rows:
        print("ERROR: no comparable benchmark keys", file=sys.stderr)
        return 2

    print("cache,mode,axis,round_robin_median_sec,contiguous_median_sec,contiguous_vs_round_robin")
    for key, rr_med, cc_med, ratio in rows:
        print(f"{key[0]},{key[1]},{key[2]},{rr_med:.6f},{cc_med:.6f},{ratio:.6f}")

    rr_overall = statistics.mean(row[1] for row in rows)
    cc_overall = statistics.mean(row[2] for row in rows)
    overall_ratio = cc_overall / rr_overall if rr_overall > 0 else float("inf")
    worst_regression = max((row[3] - 1.0 for row in rows), default=0.0)

    print()
    print(f"overall_round_robin_mean_of_medians_sec={rr_overall:.6f}")
    print(f"overall_contiguous_mean_of_medians_sec={cc_overall:.6f}")
    print(f"overall_contiguous_vs_round_robin={overall_ratio:.6f}")
    print(f"worst_contiguous_regression={worst_regression:.6f}")

    if warnings:
        print("\nWARNINGS:")
        for warning in warnings:
            print(f"- {warning}")

    switch_cutoff = 1.0 - args.switch_threshold
    regression_cutoff = 1.0 + args.max_regression
    if warnings:
        print("\nDECISION=hold (warnings must be resolved before changing defaults)")
        return 1
    if overall_ratio <= switch_cutoff and all(row[3] <= regression_cutoff for row in rows):
        print("\nDECISION=contiguous_candidate (meets configured median thresholds)")
        return 0
    print("\nDECISION=keep_round_robin (contiguous did not meet configured switch thresholds)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
