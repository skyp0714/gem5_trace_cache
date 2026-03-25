#!/usr/bin/env python3

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import (
    ThreadPoolExecutor,
    as_completed,
)
from dataclasses import (
    asdict,
    dataclass,
)
from pathlib import Path
from typing import (
    Dict,
    List,
    Tuple,
)

TRACE_RE = re.compile(r"^(tr\d+)_reduced_(lz4|zstd)_4096\.txt$")
AVG_RE = re.compile(r"Average Latency:\s+([0-9.]+)")
HIT_RE = re.compile(r"Average Hit Latency:\s+[0-9.]+\s+ns \((\d+) hits\)")
MISS_RE = re.compile(r"Average Miss Latency:\s+[0-9.]+\s+ns \((\d+) misses\)")


@dataclass
class RunResult:
    trace_id: str
    codec: str
    trace_file: str
    expected_requests: int
    request_count: int
    avg_latency_ns: float
    hit_count: int
    miss_count: int
    elapsed_sec: float
    run_dir: str
    summary_source: str
    complete: bool


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    default_workers = min(72, os.cpu_count() or 1)
    default_trace_root = Path(
        "~/cold-memory-model/output_traces/reconstruct"
    ).expanduser()
    default_results_root = repo_root / "src/cxl_objects/results/codec_sweep"

    parser = argparse.ArgumentParser(
        description="Run all 4KB LZ4/ZSTD trace pairs through cxl_trace_replay.py and compare latency."
    )
    parser.add_argument(
        "--gem5-bin",
        default=str(repo_root / "build/X86/gem5.opt"),
        help="Path to gem5.opt",
    )
    parser.add_argument(
        "--config",
        default=str(repo_root / "src/cxl_objects/config/cxl_trace_replay.py"),
        help="Path to the replay config",
    )
    parser.add_argument(
        "--trace-root",
        default=str(default_trace_root),
        help="Root containing tr*/reduced directories",
    )
    parser.add_argument(
        "--results-root",
        default=str(default_results_root),
        help="Directory for summary outputs and optional retained run directories",
    )
    parser.add_argument(
        "--max-workers",
        type=int,
        default=default_workers,
        help="Maximum parallel gem5 processes to run at once (default: min(72, nproc))",
    )
    parser.add_argument(
        "--dram-type",
        default="DDR4_2400_16x4",
        help="DRAM model passed to cxl_trace_replay.py",
    )
    parser.add_argument(
        "--mem-channels",
        type=int,
        default=4,
        help="Power-of-two memory-channel count passed to cxl_trace_replay.py",
    )
    parser.add_argument(
        "--num-engines",
        type=int,
        default=8,
        help="Number of decompression engines passed to cxl_trace_replay.py",
    )
    parser.add_argument(
        "--chunk-send-delay-ticks",
        type=int,
        default=10,
        help="Chunk-send delay passed to cxl_trace_replay.py",
    )
    parser.add_argument(
        "--inter-memory-request-delay-ticks",
        type=int,
        default=1000,
        help="Inter-memory-request delay passed to cxl_trace_replay.py",
    )
    parser.add_argument(
        "--trace-ids",
        nargs="*",
        default=None,
        help="Optional subset of trace IDs to run, e.g. tr1 tr3",
    )
    parser.add_argument(
        "--keep-run-dirs",
        action="store_true",
        help="Keep per-run gem5 output directories and latency logs",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=None,
        help="Optional timeout per gem5 run",
    )
    return parser.parse_args()


def tail_text(path: Path, max_bytes: int = 32768) -> str:
    with path.open("rb") as fh:
        fh.seek(0, os.SEEK_END)
        size = fh.tell()
        fh.seek(max(size - max_bytes, 0), os.SEEK_SET)
        return fh.read().decode("utf-8", errors="replace")


def parse_latency_summary(output_path: Path) -> Tuple[float, int, int]:
    text = tail_text(output_path)
    avg_match = AVG_RE.search(text)
    hit_match = HIT_RE.search(text)
    miss_match = MISS_RE.search(text)
    if not avg_match or not hit_match or not miss_match:
        raise ValueError(f"Could not parse latency summary from {output_path}")
    avg_latency = float(avg_match.group(1))
    hit_count = int(hit_match.group(1))
    miss_count = int(miss_match.group(1))
    return avg_latency, hit_count, miss_count


def parse_latency_rows(output_path: Path) -> Tuple[float, int, int]:
    total_latency = 0.0
    hit_count = 0
    miss_count = 0
    with output_path.open() as fh:
        for line in fh:
            if not line.startswith("0x"):
                continue
            fields = line.split()
            if len(fields) < 5:
                continue
            total_latency += float(fields[3])
            if fields[4] == "1":
                hit_count += 1
            else:
                miss_count += 1
    request_count = hit_count + miss_count
    if request_count == 0:
        raise ValueError(f"No request rows found in {output_path}")
    return total_latency / request_count, hit_count, miss_count


def count_trace_requests(trace_file: Path) -> int:
    count = 0
    with trace_file.open() as fh:
        for line in fh:
            stripped = line.strip()
            if not stripped or stripped.startswith("//"):
                continue
            count += 1
    return count


def discover_trace_pairs(
    trace_root: Path, selected_ids: List[str] | None
) -> Dict[str, Dict[str, Path]]:
    pairs: Dict[str, Dict[str, Path]] = {}
    wanted = set(selected_ids or [])
    for trace_file in sorted(trace_root.glob("tr*/reduced/*_4096.txt")):
        match = TRACE_RE.match(trace_file.name)
        if not match:
            continue
        trace_id, codec = match.groups()
        if wanted and trace_id not in wanted:
            continue
        pairs.setdefault(trace_id, {})[codec] = trace_file

    missing = [
        trace_id
        for trace_id, codecs in pairs.items()
        if {"lz4", "zstd"} - set(codecs)
    ]
    if missing:
        raise ValueError(
            f"Missing LZ4/ZSTD pair for: {', '.join(sorted(missing))}"
        )
    return dict(sorted(pairs.items(), key=lambda item: int(item[0][2:])))


def run_trace(
    gem5_bin: Path,
    config: Path,
    dram_type: str,
    mem_channels: int,
    num_engines: int,
    chunk_send_delay_ticks: int,
    inter_memory_request_delay_ticks: int,
    results_dir: Path,
    keep_run_dirs: bool,
    timeout_seconds: int | None,
    trace_id: str,
    codec: str,
    trace_file: Path,
) -> RunResult:
    run_dir = results_dir / "runs" / trace_id / codec
    run_dir.mkdir(parents=True, exist_ok=True)
    output_file = run_dir / "latency.txt"
    stdout_file = run_dir / "stdout.txt"
    stderr_file = run_dir / "stderr.txt"

    cmd = [
        str(gem5_bin),
        "-d",
        str(run_dir),
        str(config),
        "--compression-block-size=4096",
        f"--dram-type={dram_type}",
        f"--mem-channels={mem_channels}",
        f"--num-engines={num_engines}",
        f"--chunk-send-delay-ticks={chunk_send_delay_ticks}",
        f"--inter-memory-request-delay-ticks={inter_memory_request_delay_ticks}",
        f"--trace-file={trace_file}",
        f"--output-file={output_file}",
    ]

    start = time.monotonic()
    timed_out = False
    with stdout_file.open("w") as stdout, stderr_file.open("w") as stderr:
        try:
            completed = subprocess.run(
                cmd,
                stdout=stdout,
                stderr=stderr,
                timeout=timeout_seconds,
                check=False,
            )
        except subprocess.TimeoutExpired:
            timed_out = True
            completed = None
    elapsed = time.monotonic() - start
    expected_requests = count_trace_requests(trace_file)

    if completed is not None and completed.returncode == 0:
        try:
            avg_latency, hit_count, miss_count = parse_latency_summary(
                output_file
            )
            summary_source = "summary"
        except ValueError:
            avg_latency, hit_count, miss_count = parse_latency_rows(
                output_file
            )
            summary_source = "rows"
    elif output_file.exists():
        avg_latency, hit_count, miss_count = parse_latency_rows(output_file)
        summary_source = "rows_timeout" if timed_out else "rows_failed_exit"
    else:
        stderr_tail = (
            tail_text(stderr_file, max_bytes=8192)
            if stderr_file.exists()
            else ""
        )
        if timed_out:
            raise RuntimeError(
                f"gem5 timed out for {trace_id}/{codec} after {timeout_seconds}s and no output rows were available\n"
                f"{stderr_tail}"
            )
        raise RuntimeError(
            f"gem5 failed for {trace_id}/{codec} with exit code {completed.returncode}\n{stderr_tail}"
        )

    request_count = hit_count + miss_count
    complete = request_count == expected_requests
    result = RunResult(
        trace_id=trace_id,
        codec=codec,
        trace_file=str(trace_file),
        expected_requests=expected_requests,
        request_count=request_count,
        avg_latency_ns=avg_latency,
        hit_count=hit_count,
        miss_count=miss_count,
        elapsed_sec=elapsed,
        run_dir=str(run_dir),
        summary_source=summary_source,
        complete=complete,
    )

    if not keep_run_dirs:
        for path in (output_file, stdout_file, stderr_file):
            if path.exists():
                path.unlink()
        shutil.rmtree(run_dir, ignore_errors=True)

    return result


def write_csv(path: Path, rows: List[dict], fieldnames: List[str]) -> None:
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    gem5_bin = Path(args.gem5_bin).expanduser().resolve()
    config = Path(args.config).expanduser().resolve()
    trace_root = Path(args.trace_root).expanduser().resolve()
    results_root = Path(args.results_root).expanduser().resolve()
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    results_dir = results_root / timestamp

    if not gem5_bin.is_file():
        raise FileNotFoundError(f"gem5 binary not found: {gem5_bin}")
    if not config.is_file():
        raise FileNotFoundError(f"Config not found: {config}")
    if not trace_root.is_dir():
        raise FileNotFoundError(f"Trace root not found: {trace_root}")
    if args.max_workers < 1:
        raise ValueError("--max-workers must be >= 1")

    trace_pairs = discover_trace_pairs(trace_root, args.trace_ids)
    if not trace_pairs:
        raise ValueError("No matching 4KB LZ4/ZSTD trace pairs found")

    results_dir.mkdir(parents=True, exist_ok=True)
    jobs = []
    for trace_id, codec_paths in trace_pairs.items():
        for codec, trace_file in sorted(codec_paths.items()):
            jobs.append((trace_id, codec, trace_file))

    max_workers = min(args.max_workers, len(jobs))
    print(f"Found {len(trace_pairs)} trace pairs ({len(jobs)} runs).")
    print(f"Using up to {max_workers} workers.")
    print(f"Summary output: {results_dir}")
    sys.stdout.flush()

    results: List[RunResult] = []
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        future_map = {
            executor.submit(
                run_trace,
                gem5_bin,
                config,
                args.dram_type,
                args.mem_channels,
                args.num_engines,
                args.chunk_send_delay_ticks,
                args.inter_memory_request_delay_ticks,
                results_dir,
                args.keep_run_dirs,
                args.timeout_seconds,
                trace_id,
                codec,
                trace_file,
            ): (trace_id, codec)
            for trace_id, codec, trace_file in jobs
        }
        for future in as_completed(future_map):
            trace_id, codec = future_map[future]
            result = future.result()
            results.append(result)
            print(
                f"[done] {trace_id}/{codec}: avg={result.avg_latency_ns:.2f} ns, "
                f"reqs={result.request_count}/{result.expected_requests}, "
                f"source={result.summary_source}, elapsed={result.elapsed_sec:.1f}s"
            )
            sys.stdout.flush()

    results.sort(key=lambda item: (int(item.trace_id[2:]), item.codec))
    by_trace: Dict[str, Dict[str, RunResult]] = {}
    for result in results:
        by_trace.setdefault(result.trace_id, {})[result.codec] = result

    per_trace_rows = []
    total_lz4_weighted = 0.0
    total_zstd_weighted = 0.0
    total_lz4_requests = 0
    total_zstd_requests = 0
    lz4_mean_sum = 0.0
    zstd_mean_sum = 0.0

    for trace_id, codec_results in sorted(
        by_trace.items(), key=lambda item: int(item[0][2:])
    ):
        lz4 = codec_results["lz4"]
        zstd = codec_results["zstd"]
        delta_ns = lz4.avg_latency_ns - zstd.avg_latency_ns
        reduction_pct = (
            (delta_ns / lz4.avg_latency_ns * 100.0)
            if lz4.avg_latency_ns
            else 0.0
        )
        per_trace_rows.append(
            {
                "trace_id": trace_id,
                "lz4_requests": lz4.request_count,
                "zstd_requests": zstd.request_count,
                "lz4_complete": str(lz4.complete),
                "zstd_complete": str(zstd.complete),
                "lz4_source": lz4.summary_source,
                "zstd_source": zstd.summary_source,
                "lz4_avg_latency_ns": f"{lz4.avg_latency_ns:.2f}",
                "zstd_avg_latency_ns": f"{zstd.avg_latency_ns:.2f}",
                "delta_ns": f"{delta_ns:.2f}",
                "reduction_pct": f"{reduction_pct:.2f}",
            }
        )
        total_lz4_weighted += lz4.avg_latency_ns * lz4.request_count
        total_zstd_weighted += zstd.avg_latency_ns * zstd.request_count
        total_lz4_requests += lz4.request_count
        total_zstd_requests += zstd.request_count
        lz4_mean_sum += lz4.avg_latency_ns
        zstd_mean_sum += zstd.avg_latency_ns

    pair_count = len(per_trace_rows)
    overall_lz4_weighted = (
        total_lz4_weighted / total_lz4_requests if total_lz4_requests else 0.0
    )
    overall_zstd_weighted = (
        total_zstd_weighted / total_zstd_requests
        if total_zstd_requests
        else 0.0
    )
    overall_weighted_delta = overall_lz4_weighted - overall_zstd_weighted
    overall_weighted_reduction = (
        overall_weighted_delta / overall_lz4_weighted * 100.0
        if overall_lz4_weighted
        else 0.0
    )
    overall_lz4_mean = lz4_mean_sum / pair_count if pair_count else 0.0
    overall_zstd_mean = zstd_mean_sum / pair_count if pair_count else 0.0
    overall_mean_delta = overall_lz4_mean - overall_zstd_mean
    overall_mean_reduction = (
        overall_mean_delta / overall_lz4_mean * 100.0
        if overall_lz4_mean
        else 0.0
    )

    csv_path = results_dir / "per_trace_summary.csv"
    json_path = results_dir / "summary.json"
    write_csv(
        csv_path,
        per_trace_rows,
        [
            "trace_id",
            "lz4_requests",
            "zstd_requests",
            "lz4_complete",
            "zstd_complete",
            "lz4_source",
            "zstd_source",
            "lz4_avg_latency_ns",
            "zstd_avg_latency_ns",
            "delta_ns",
            "reduction_pct",
        ],
    )

    summary = {
        "trace_root": str(trace_root),
        "gem5_bin": str(gem5_bin),
        "config": str(config),
        "dram_type": args.dram_type,
        "mem_channels": args.mem_channels,
        "num_engines": args.num_engines,
        "chunk_send_delay_ticks": args.chunk_send_delay_ticks,
        "inter_memory_request_delay_ticks": args.inter_memory_request_delay_ticks,
        "max_workers": max_workers,
        "trace_pairs": pair_count,
        "weighted": {
            "lz4_avg_latency_ns": round(overall_lz4_weighted, 2),
            "zstd_avg_latency_ns": round(overall_zstd_weighted, 2),
            "delta_ns": round(overall_weighted_delta, 2),
            "reduction_pct": round(overall_weighted_reduction, 2),
            "lz4_requests": total_lz4_requests,
            "zstd_requests": total_zstd_requests,
        },
        "mean_of_trace_averages": {
            "lz4_avg_latency_ns": round(overall_lz4_mean, 2),
            "zstd_avg_latency_ns": round(overall_zstd_mean, 2),
            "delta_ns": round(overall_mean_delta, 2),
            "reduction_pct": round(overall_mean_reduction, 2),
        },
        "per_trace": per_trace_rows,
    }
    with json_path.open("w") as fh:
        json.dump(summary, fh, indent=2)
        fh.write("\n")

    print()
    print("Per-trace summary:")
    for row in per_trace_rows:
        print(
            f"{row['trace_id']}: "
            f"lz4={row['lz4_avg_latency_ns']} ns, "
            f"zstd={row['zstd_avg_latency_ns']} ns, "
            f"delta={row['delta_ns']} ns, "
            f"reduction={row['reduction_pct']}%"
        )

    print()
    print(
        "Weighted overall: "
        f"lz4={overall_lz4_weighted:.2f} ns, "
        f"zstd={overall_zstd_weighted:.2f} ns, "
        f"delta={overall_weighted_delta:.2f} ns, "
        f"reduction={overall_weighted_reduction:.2f}%"
    )
    print(
        "Mean of trace averages: "
        f"lz4={overall_lz4_mean:.2f} ns, "
        f"zstd={overall_zstd_mean:.2f} ns, "
        f"delta={overall_mean_delta:.2f} ns, "
        f"reduction={overall_mean_reduction:.2f}%"
    )
    print(f"CSV: {csv_path}")
    print(f"JSON: {json_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
